#include "mustela.hpp"
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <errno.h>
#include <iostream>
#include <algorithm>
#include <memory>

using namespace mustela;
	
const size_t additional_granularity = 1;// 65536;  // on Windows mmapped regions should be aligned to 65536

static uint64_t grow_to_granularity(uint64_t value, uint64_t page_size){
	return ((value + page_size - 1) / page_size) * page_size;
}
static uint64_t grow_to_granularity(uint64_t value, uint64_t a, uint64_t b, uint64_t c){
	return grow_to_granularity(grow_to_granularity(grow_to_granularity(value, a), b), c);
}
DB::FD::~FD(){
	close(fd); fd = -1;
}
DB::FileLock::FileLock(int fd):fd(fd){
 	int res = flock(fd, LOCK_EX);
	ass( res == 0, "Failed to exclusively lock file");
}
DB::FileLock::~FileLock(){
	int res = flock(fd, LOCK_UN);
	ass( res == 0, "Failed to exclusively lock file");
}

std::string DB::lib_version(){
	return "0.02";
}

DB::DB(const std::string & file_path, DBOptions options):fd(open(file_path.c_str(), (options.read_only ? O_RDONLY : O_RDWR) | O_CREAT, (mode_t)0600)), lock_fd(-1), options(options), physical_page_size(static_cast<decltype(physical_page_size)>(sysconf(_SC_PAGESIZE))){
	if((options.new_db_page_size & (options.new_db_page_size - 1)) != 0)
		throw Exception("new_db_page_size must be power of 2");
	if( fd.fd == -1)
		throw Exception("file open failed for {" + file_path + "}");
	FileLock wr_lock(fd.fd);
	file_size = static_cast<uint64_t>(lseek(fd.fd, 0, SEEK_END));
	if( file_size == uint64_t(-1))
		throw Exception("file lseek SEEK_END failed");
	lock_fd.fd = open((file_path + ".lock").c_str(), O_RDWR | O_CREAT, (mode_t)0600);
	if( lock_fd.fd == -1)
		throw Exception("file open failed for {" + file_path + ".lock}");
	FileLock reader_table_lock(lock_fd.fd);
	if( file_size == 0 ){
		page_size = options.new_db_page_size == 0 ? GOOD_PAGE_SIZE : options.new_db_page_size;
		create_db();
//		return;
	}
	if( file_size < sizeof(MetaPage) )
		throw Exception("File size less than 1 meta page - corrupted by truncation");
	page_size = MAX_PAGE_SIZE;
	grow_c_mappings();
	page_size = readable_meta_page(0)->page_size;
	const MetaPage * newest_meta = nullptr;
	Pid oldest_index = 0;
	Tid earliest_tid = 0;
	if(page_size < MIN_PAGE_SIZE || page_size > MAX_PAGE_SIZE || (page_size & (page_size - 1)) != 0 ||
		!(newest_meta = get_newest_meta_page(&oldest_index, &earliest_tid, false))){
		// If meta page 0 page_size is corrupted, will have to try all page sizes
		for(page_size = MIN_PAGE_SIZE; page_size <= MAX_PAGE_SIZE; page_size *= 2)
			if( (newest_meta = get_newest_meta_page(&oldest_index, &earliest_tid, false)) )
				break;
		if(page_size > MAX_PAGE_SIZE)
			throw Exception("Failed to find valid meta page of any supported page size");
	}
	debug_print_db();
	if(newest_meta->version != OUR_VERSION)
		throw Exception("Incompatible database version");
	if(newest_meta->pid_size != NODE_PID_SIZE)
		throw Exception("Incompatible pid size");
	if( !get_newest_meta_page(&oldest_index, &earliest_tid, true))
		throw Exception("Database corrupted (possibly truncated or meta pages are mismatched)");
}
DB::~DB(){
	for(auto && ma : c_mappings)
		ass(ma.ref_count == 0, "Some TX still exist while in DB::~DB");
}
const MetaPage * DB::readable_meta_page(Pid index)const {
	ass(!c_mappings.empty() && (index + 1)*page_size <= c_mappings.at(0).end_addr, "writable_page out of range");
	return (const MetaPage * )(c_mappings.at(0).addr + page_size * index);
}
MetaPage * DB::writable_meta_page(Pid index) {
	ass(!wr_mappings.empty() && (index + 1)*page_size <= wr_mappings.at(0).end_addr, "writable_page out of range");
	return (MetaPage * )(wr_mappings.at(0).addr + page_size * index);
}
bool DB::is_valid_meta(Pid index, const MetaPage * mp)const{
	if((index + 1) * page_size > file_size )
		return false;
	if( mp->pid != index || mp->magic != META_MAGIC)
		return false; // throw Exception("file is either not mustela DB or corrupted - wrong meta page");
	if( mp->pid_size < 4 || mp->pid_size > 8 || mp->page_size != page_size || mp->page_count < 4 )
		return false;
	if( mp->crc32 != crc32c(0, mp, sizeof(MetaPage) - sizeof(uint32_t)))
		return false;
	return true;
}
bool DB::is_valid_meta_strict(const MetaPage * mp)const{
	if( mp->meta_bucket.root_page >= mp->page_count || mp->page_count * page_size > file_size )
		return false;
	if( mp->version != OUR_VERSION || mp->pid_size != NODE_PID_SIZE )
		return false;
	return true;
}

const MetaPage * DB::get_newest_meta_page(Pid * overwrite_index, Tid * earliest_tid, bool strict)const{
	const MetaPage * newest_mp = nullptr;
	const MetaPage * corrupted_mp = nullptr;
	const MetaPage * oldest_mp = nullptr;
	for(Pid i = 0; i != META_PAGES_COUNT; ++i){
		const MetaPage * mp = readable_meta_page(i);
		if( !is_valid_meta(i, mp) || (strict && !is_valid_meta_strict(mp)) ){
			corrupted_mp = mp;
			*overwrite_index = i;
			continue;
		}
		if(!newest_mp || mp->tid > newest_mp->tid || (mp->tid == newest_mp->tid && mp->pid > newest_mp->pid)){
			newest_mp = mp;
		}
		if(!oldest_mp || mp->tid < oldest_mp->tid || (mp->tid == oldest_mp->tid && mp->pid < oldest_mp->pid)){
			oldest_mp = mp;
			*earliest_tid = mp->tid;
			if(!corrupted_mp)
				*overwrite_index = i;
		}
	}
	return newest_mp;
}

void DB::start_transaction(TX * tx){
	std::unique_ptr<std::lock_guard<std::mutex>> local_wr_guard;
	std::unique_ptr<FileLock> local_wr_file_lock;
	if(!tx->read_only){
		// write TX from same DB wait on guard
		local_wr_guard = std::make_unique<std::lock_guard<std::mutex>>(wr_mut);
		// write TX from different DB (same or different process) wait on file lock
		local_wr_file_lock = std::make_unique<FileLock>(fd.fd);
		std::cerr << "Obtained main file write lock " << (size_t)this << std::endl;
//		sleep(3);
	}
	std::unique_lock<std::mutex> lock(mu);
	ass((!wr_transaction && !wr_file_lock) || tx->read_only, "We can have only one write transaction");
	ass(!c_mappings.empty(), "c_mappings should not be empty after db is open");
	{ // Shortest possible lock
		FileLock reader_table_lock(lock_fd.fd);
		std::cerr << "Obtained reader table lock " << (size_t)this << std::endl;
//		sleep(2);
		Pid oldest_meta_index = 0;
		const MetaPage * newest_meta_page = get_newest_meta_page(&oldest_meta_index, &tx->oldest_reader_tid, true);
		ass(newest_meta_page, "No meta found in start_transaction - hot corruption of DB");
		tx->meta_page = *newest_meta_page;
		tx->meta_page.pid = 0; // So we do not forget to set it before write
		if(tx->read_only){
			r_transactions_counter += 1;
			tx->reader_slot = reader_table.create_reader_slot(tx->meta_page.tid, lock_fd.fd, std::max(physical_page_size, additional_granularity));
		} else {
			wr_transaction = tx;
			tx->meta_page.tid += 1;
//			tx->oldest_reader_tid = reader_table.find_oldest_tid(tx->meta_page.tid);
			tx->oldest_reader_tid = 0;
			ass(tx->meta_page.tid >= tx->oldest_reader_tid, "We should not be able to treat our own pages as free");
		}
		std::cerr << "Freeing reader table lock " << (size_t)this << std::endl;
	}
	if(!tx->read_only){
		grow_wr_mappings(false);
		wr_guard = std::move(local_wr_guard);
		wr_file_lock = std::move(local_wr_file_lock);
	}
	tx->c_file_ptr = c_mappings.at(0).addr;
	tx->wr_file_ptr = wr_mappings.empty() ? nullptr : wr_mappings.at(0).addr;
	tx->file_page_count = file_size / page_size;
	tx->used_mapping_size = c_mappings.at(0).end_addr;
	c_mappings.at(0).ref_count += 1;
}
void DB::grow_transaction(TX * tx, Pid new_file_page_count){
	std::unique_lock<std::mutex> lock(mu);
	ass(wr_transaction && tx == wr_transaction && !tx->read_only, "We can only grow write transaction");
	ass(!c_mappings.empty() && !wr_mappings.empty(), "Mappings should not be empty in grow_transaction");
	grow_wr_mappings(new_file_page_count);
	tx->c_file_ptr = c_mappings.at(0).addr;
	tx->wr_file_ptr = wr_mappings.at(0).addr;
	tx->file_page_count = file_size / page_size;
}
void DB::commit_transaction(TX * tx, MetaPage meta_page){
	std::unique_lock<std::mutex> lock(mu);
	ass(tx == wr_transaction, "We can only commit write transaction if it started");
	msync(wr_mappings.at(0).addr, wr_mappings.at(0).end_addr, MS_SYNC);

	Pid oldest_meta_index = 0;
	{
		FileLock reader_table_lock(lock_fd.fd);
		const MetaPage * newest_meta_page = get_newest_meta_page(&oldest_meta_index, &tx->oldest_reader_tid, true);
		ass(newest_meta_page, "No meta found in start_transaction - hot corruption of DB");
		meta_page.pid = oldest_meta_index; // We usually save to different slot
		meta_page.crc32 = crc32c(0, &meta_page, sizeof(MetaPage) - sizeof(uint32_t));
		MetaPage * wr_meta = writable_meta_page(oldest_meta_index);
		*wr_meta = meta_page;
		tx->meta_page.tid += 1; // We continue using tx meta_page
		// We locked reader table anyway, take a chance to update oldest_reader_tid
		tx->oldest_reader_tid = reader_table.find_oldest_tid(tx->meta_page.tid);
		ass(tx->meta_page.tid >= tx->oldest_reader_tid, "We should not be able to treat our own pages as free");
	}
	if(options.meta_sync){
		// We can only msync on phys page limits, find them
		size_t low = oldest_meta_index * page_size;
		size_t high = (oldest_meta_index + 1) * page_size;
		low = ((low / physical_page_size)) * physical_page_size;
		high = ((high + physical_page_size - 1) / physical_page_size) *	physical_page_size;
		msync(wr_mappings.at(0).addr + low, high - low, MS_SYNC);
	}
}
void DB::finish_transaction(TX * tx){
	std::unique_lock<std::mutex> lock(mu);
	ass(tx->read_only || tx == wr_transaction, "We can only finish write transaction if it started");
	for(auto && ma : c_mappings)
		if( (tx->read_only && ma.end_addr == tx->used_mapping_size) ||
		 	(!tx->read_only && ma.end_addr >= tx->used_mapping_size)) {
			ma.ref_count -= 1;
		}
	tx->c_file_ptr = nullptr;
	tx->wr_file_ptr = nullptr;
	tx->file_page_count = 0;
	tx->used_mapping_size = 0;
	while(c_mappings.size() > 1 && c_mappings.back().ref_count == 0) {
		munmap(c_mappings.back().addr, c_mappings.back().end_addr);
		c_mappings.pop_back();
	}
	if(tx->read_only){
		// We release slots without blocking, do not care if will be updated later
		reader_table.release_reader_slot(tx->reader_slot);
		r_transactions_counter -= 1;
		ass(r_transactions_counter >= 0, "read transaction finished twice");
		return;
	}
	wr_transaction = nullptr;
	while(wr_mappings.size() > 1) {
//		msync(wr_mappings.back().addr, wr_mappings.back().end_addr, MS_SYNC);
		munmap(wr_mappings.back().addr, wr_mappings.back().end_addr);
		wr_mappings.pop_back();
	}
	std::cerr << "Freeing main file write lock " << (size_t)this << std::endl;
	wr_file_lock.reset();
	wr_guard.reset();
}

void DB::debug_print_db(){
	std::cerr << "DB: page_size=" << page_size << " phys. page_size=" << physical_page_size << " file_size=" << file_size << std::endl;
	for(Pid i = 0; i != META_PAGES_COUNT; ++i){
		std::cerr << "  meta page " << i << ": ";
		if((i + 1) * page_size > file_size ){
			std::cerr << "(partially?) BEYOND END OF FILE" << std::endl;;
			continue;
		}
		const MetaPage * mp = readable_meta_page(i);
		bool crc_ok = mp->crc32 == crc32c(0, mp, sizeof(MetaPage) - sizeof(uint32_t));
		std::cerr << (is_valid_meta(i, mp) ? "GOOD" : crc_ok ? "BAD" : "WRONG CRC");
		std::cerr << " pid=" << mp->pid << " tid=" << mp->tid << " page_count=" << mp->page_count << " ver=" << mp->version << " pid_size=" << mp->pid_size << std::endl;;
		std::cerr << "    meta bucket: height=" << mp->meta_bucket.height << " items=" << mp->meta_bucket.count << " leafs=" << mp->meta_bucket.leaf_page_count << " nodes=" << mp->meta_bucket.node_page_count << " overflows=" << mp->meta_bucket.overflow_page_count << " root_page=" << mp->meta_bucket.root_page << std::endl;
	}
}
size_t DB::max_key_size()const{
    return mustela::max_key_size(page_size);
}
size_t DB::max_bucket_name_size()const{
    return mustela::max_key_size(page_size) - 1;
}

void DB::remove_db(const std::string & file_path){
    std::remove(file_path.c_str());
    std::remove((file_path + ".lock").c_str());
}

void DB::create_db(){
	if( lseek(fd.fd, 0, SEEK_SET) == -1 )
		throw Exception("file seek SEEK_SET failed");
	char data_buf[MAX_PAGE_SIZE]; // Variable-length arrays are C99 feature
	memset(data_buf, 0, page_size); // C++ standard URODI "variable size object cannot be initialized"
	MetaPage * mp = (MetaPage *)data_buf;
	mp->magic = META_MAGIC;
	mp->page_count = META_PAGES_COUNT + 1;
	mp->version = OUR_VERSION;
	mp->page_size = static_cast<uint32_t>(page_size);
	mp->pid_size = NODE_PID_SIZE;
	mp->meta_bucket.leaf_page_count = 1;
	mp->meta_bucket.root_page = META_PAGES_COUNT;
	for(mp->pid = 0; mp->pid != META_PAGES_COUNT; ++mp->pid){
		mp->crc32 = crc32c(0, mp, sizeof(MetaPage) - sizeof(uint32_t));
		if( write(fd.fd, data_buf, page_size) == -1)
			throw Exception("file write failed in create_db");
	}
	LeafPtr wr_dap(page_size, (LeafPage *)data_buf);
//	wr_dap.mpage()->pid = META_PAGES_COUNT;
	wr_dap.init_dirty(0);
	if( write(fd.fd, data_buf, page_size) == -1)
		throw Exception("file write failed in create_db");
	if( fsync(fd.fd) == -1 )
		throw Exception("fsync failed in create_db");
	file_size = static_cast<uint64_t>(lseek(fd.fd, 0, SEEK_END));
	if( file_size == uint64_t(-1))
		throw Exception("file lseek SEEK_END failed");
	fsync(fd.fd);
//	grow_c_mappings();
}

void DB::grow_c_mappings() {
	if( !c_mappings.empty() && c_mappings.at(0).end_addr >= file_size )
		return;
	uint64_t fs = file_size;
	if( !options.read_only )
		fs = std::max<uint64_t>(fs, options.minimal_mapping_size) * 128 / 64; // x1.5
	fs = std::max<uint64_t>(fs, META_PAGES_COUNT * MAX_PAGE_SIZE); // for initial meta discovery in open_db
	uint64_t new_fs = grow_to_granularity(fs, page_size, physical_page_size, additional_granularity);
	void * cm = mmap(0, new_fs, PROT_READ, MAP_SHARED, fd.fd, 0);
	if (cm == MAP_FAILED)
		throw Exception("mmap PROT_READ failed");
	c_mappings.insert(c_mappings.begin(), Mapping(new_fs, (char *)cm, wr_transaction ? 1 : 0));
}
void DB::grow_wr_mappings(Pid new_file_page_count){
	uint64_t fs = file_size;
	if( new_file_page_count != 0 )
	 	fs = std::max<uint64_t>(fs, std::max<uint64_t>(options.minimal_mapping_size, new_file_page_count * page_size)) * 77 / 64; // x1.2
	uint64_t new_fs = grow_to_granularity(fs, page_size, physical_page_size, additional_granularity);
	ass(wr_mappings.empty() || wr_mappings.at(0).end_addr == file_size, "latest wr mapping should be exactly at the size of file");
	if(!wr_mappings.empty() && new_fs == file_size && wr_mappings.at(0).end_addr == new_fs)
		return;
	if(new_fs != file_size){
		if( ftruncate(fd.fd, static_cast<off_t>(new_fs)) == -1)
			throw Exception("failed to grow db file using ftruncate");
//		if( lseek(fd.fd, new_fs - 1, SEEK_SET) == -1 )
//			throw Exception("file seek failed in grow_file");
//		if( write(fd.fd, "", 1) != 1 )
//			throw Exception("file write failed in grow_file");
		file_size = static_cast<uint64_t>(lseek(fd.fd, 0, SEEK_END));
		if( new_fs != file_size )
			throw Exception("file failed to grow in grow_file");
	}
	void * wm = mmap(0, new_fs, PROT_READ | PROT_WRITE, MAP_SHARED, fd.fd, 0);
	if (wm == MAP_FAILED)
		throw Exception("mmap PROT_READ | PROT_WRITE failed");
	wr_mappings.insert(wr_mappings.begin(), Mapping(new_fs, (char *)wm, wr_transaction ? 1 : 0));
	grow_c_mappings();
}

