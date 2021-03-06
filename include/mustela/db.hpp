#pragma once

#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include "pages.hpp"
#include "tx.hpp"
#include "lock.hpp"

namespace mustela {
	
	struct DBOptions {
		bool read_only = false;
		bool meta_sync = true;
		size_t new_db_page_size = 0; // 0 - select automatically. Used only when creating file
		size_t minimal_mapping_size = 1024; // Good for test, TODO - set to larger value closer to release
	};

	class DB {
	public:
		explicit DB(const std::string & file_path, DBOptions options = DBOptions{});
		~DB();
		
		static void remove_db(const std::string & file_path);

		static std::string lib_version();
		size_t max_key_size()const;
		size_t max_bucket_name_size()const;
		
		void debug_print_db();
	protected:
		friend class TX;
		void start_transaction(TX * tx);
		void grow_transaction(TX * tx, Pid new_file_page_count);
		void commit_transaction(TX * tx, MetaPage meta_page);
		void finish_transaction(TX * tx);
	private:
		struct FD {
			int fd;
			explicit FD(int fd):fd(fd) {}
			~FD();
		};
		struct FileLock {
			int fd;
			explicit FileLock(int fd);
			~FileLock();
		};
		// Mappings cannot be in chunks, because count pages could fall onto the edge between chunks
		struct Mapping {
			size_t end_addr;
			char * addr;
			int ref_count;
			explicit Mapping(size_t end_addr, char * addr, int ref_count):end_addr(end_addr), addr(addr), ref_count(ref_count)
			{}
		};
		FD fd;
		FD lock_fd;
		const DBOptions options;
		size_t page_size = 0;
		const size_t physical_page_size; // We allow to work with smaller/larger pages when reading file from different platform (or portable variant)

		std::mutex mu; // protect vars shared between all transactions
		uint64_t file_size = 0;
		TX * wr_transaction = nullptr;
		int r_transactions_counter = 0;
		// mappings are expensive to create, so they are shared between transactions
		std::vector<Mapping> c_mappings;
		std::vector<Mapping> wr_mappings;
		
		ReaderTable reader_table;

		std::mutex wr_mut;
		std::unique_ptr<std::lock_guard<std::mutex>> wr_guard;
		std::unique_ptr<FileLock> wr_file_lock;
		
		bool is_valid_meta(Pid index, const MetaPage * mp)const;
		bool is_valid_meta_strict(const MetaPage * mp)const;
		const MetaPage * get_newest_meta_page(Pid * oldest_meta_index, Tid * earliest_tid, bool strict)const;
		
		void grow_c_mappings();
		void grow_wr_mappings(Pid new_file_page_count);

		const MetaPage * readable_meta_page(Pid index)const;
		MetaPage * writable_meta_page(Pid index);
		void create_db();
		bool open_db();
	};
}

