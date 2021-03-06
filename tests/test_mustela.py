import sys
import hashlib
import subprocess
import tempfile
import os.path
import binascii
import hypothesis.strategies as st
from hypothesis import note, settings
from hypothesis.stateful import RuleBasedStateMachine, Bundle, rule, precondition, invariant
from sortedcontainers import SortedDict


MUSTELA_BINARY = './bin/mustela'
MUSTELA_DB = 'db.mustela'


def gen_bucket():
    return st.binary(max_size=45)


def gen_key():
    return st.binary(max_size=46)


def gen_key_prefix():
    return st.binary(max_size=46-1)


def clone_db(db):
    return SortedDict((b, SortedDict((k, v) for k, v in kvs.items())) for b, kvs in db.items())


def encode_nulls(tag, b):
    r = bytearray(tag)
    for c in b:
        r.append(c)
        if c == 0:
            r.append(0xff)
    r.append(0)
    return r


def db_hash(db):
    h = hashlib.blake2b(digest_size=32)
    for b, kvs in db.items():
        h.update(encode_nulls(b'b', b))
        for k, v in kvs.items():
            h.update(encode_nulls(b'k', k))
            h.update(encode_nulls(b'v', v))
    return h.digest()


class MustelaTestMachine(RuleBasedStateMachine):
    def __init__(self):
        super().__init__()
        sys.stderr.write('-' * 30 + ' test run start ' + '-' * 30 + '\n')
        self.dir = tempfile.TemporaryDirectory()
        self.mustela = self.open_db()
        self.committed = SortedDict()
        self.db = SortedDict()
        self.readers = []

    def open_db(self):
        return subprocess.Popen([MUSTELA_BINARY, '--test', os.path.join(self.dir.name, MUSTELA_DB)], stdin=subprocess.PIPE, stdout=subprocess.PIPE, bufsize=0, encoding='utf-8')

    def teardown(self):
        self.mustela.stdin.close()
        self.mustela.wait()
        self.dir.cleanup()

    def send(self, cmd: str, *args):
        input_ = cmd + ',' + ','.join(binascii.hexlify(arg).decode('ascii') for arg in args)
        self.mustela.stdin.write(input_ + '\n')
        self.mustela.stdout.readline()

    @invariant()
    def same_hash(self):
        self.send('ensure-hash', db_hash(self.db))

    @precondition(lambda self: self.readers)
    @invariant()
    def same_reader_hashes(self):
        self.send('ensure-reader-hashes', *(db_hash(r) for r in self.readers))

    @rule()
    def kill(self):
        self.db = clone_db(self.committed)
        self.readers = []
        self.send('kill')
        self.mustela = self.open_db()

    @rule(bucket=gen_bucket())
    def create_bucket(self, bucket):
        if bucket in self.db:
            return
        self.db[bucket] = SortedDict()
        self.send('create-bucket', bucket)

    @precondition(lambda self: self.db)
    @rule(data=st.data())
    def drop_bucket(self, data):
        bucket = data.draw(st.sampled_from(list(self.db)), 'bucket')
        del self.db[bucket]
        self.send('drop-bucket', bucket)

    @rule(reset=st.booleans())
    def commit(self, reset):
        self.committed = clone_db(self.db)
        self.readers = [] if reset else self.readers
        self.send('commit-reset' if reset else 'commit')

    @rule(reset=st.booleans())
    def rollback(self, reset):
        self.db = clone_db(self.committed)
        self.readers = [] if reset else self.readers
        self.send('rollback-reset' if reset else 'rollback')

    @precondition(lambda self: self.db)
    @rule(data=st.data(), k=gen_key(), v=st.binary())
    def put(self, data, k, v):
        bucket = data.draw(st.sampled_from(list(self.db)), 'bucket')
        self.db[bucket][k] = v
        self.send('put', bucket, k, v)

    @precondition(lambda self: self.db)
    @rule(data=st.data(), k_prefix=gen_key_prefix(), v_prefix=st.binary(), n=st.integers(min_value=0, max_value=255))
    def put_n(self, data, k_prefix, v_prefix, n):
        bucket = data.draw(st.sampled_from(list(self.db)), 'bucket')
        for i in range(n):
            p = i.to_bytes(length=1, byteorder='big')
            k = k_prefix + p
            v = v_prefix + p
            self.db[bucket][k] = v
        self.send('put-n', bucket, k_prefix, v_prefix, n.to_bytes(length=1, byteorder='big'))

    @precondition(lambda self: self.db)
    @rule(data=st.data(), k_prefix=gen_key_prefix(), v_prefix=st.binary(), n=st.integers(min_value=0, max_value=255))
    def put_n_rev(self, data, k_prefix, v_prefix, n):
        bucket = data.draw(st.sampled_from(list(self.db)), 'bucket')
        for i in reversed(range(n)):
            p = i.to_bytes(length=1, byteorder='big')
            k = k_prefix + p
            v = v_prefix + p
            self.db[bucket][k] = v
        self.send('put-n-rev', bucket, k_prefix, v_prefix, n.to_bytes(length=1, byteorder='big'))

    @precondition(lambda self: any(self.db.values()))
    @rule(data=st.data(), v=st.binary())
    def change(self, data, v):
        bucket = data.draw(st.sampled_from(list(b for b, kvs in self.db.items() if kvs)), 'bucket')
        k = data.draw(st.sampled_from(list(self.db[bucket])), 'key')
        self.db[bucket][k] = v
        self.send('put', bucket, k, v)

    @precondition(lambda self: any(self.db.values()))
    @rule(data=st.data(), cursor=st.booleans())
    def del_(self, data, cursor):
        bucket = data.draw(st.sampled_from(list(b for b, kvs in self.db.items() if kvs)), 'bucket')
        k = data.draw(st.sampled_from(list(self.db[bucket])), 'key')
        del self.db[bucket][k]
        self.send('del-cursor' if cursor else 'del', bucket, k)

    @precondition(lambda self: any(self.db.values()))
    @rule(data=st.data(), n=st.integers(min_value=0, max_value=255))
    def del_n(self, data, n):
        bucket = data.draw(st.sampled_from(list(b for b, kvs in self.db.items() if kvs)), 'bucket')
        keys = list(self.db[bucket])
        key = data.draw(st.sampled_from(keys), 'key')
        for i, k in enumerate(keys[keys.index(key):]):
            if i == n:
                break
            del self.db[bucket][k]
        self.send('del-n', bucket, key, n.to_bytes(length=1, byteorder='big'))

    @precondition(lambda self: any(self.db.values()))
    @rule(data=st.data(), n=st.integers(min_value=0, max_value=255))
    def del_n_rev(self, data, n):
        bucket = data.draw(st.sampled_from(list(b for b, kvs in self.db.items() if kvs)), 'bucket')
        keys = list(self.db[bucket])
        key = data.draw(st.sampled_from(keys), 'key')
        n = min(n, keys.index(key) + 1)  # TODO: get rid of cyclic mustela cursor semantics
        for i, k in enumerate(reversed(keys[:keys.index(key)+1])):
            if i == n:
                break
            del self.db[bucket][k]
        self.send('del-n-rev', bucket, key, n.to_bytes(length=1, byteorder='big'))

    @rule()
    def create_reader(self):
        self.readers.append(clone_db(self.committed))
        self.send('create-reader')


with settings(max_examples=100, stateful_step_count=100):
    TestMustela = MustelaTestMachine.TestCase
