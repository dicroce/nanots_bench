// RocksDB benchmark backend
//
// DATA MODEL
//   Each named stream maps to a dedicated RocksDB Column Family.
//   Key:   8-byte big-endian timestamp_us  (ensures correct range-scan order)
//   Value: raw frame payload (flags packed into first 4 bytes, then data)
//
// DURABILITY MAPPING
//   sync_boundary() calls db->SyncWAL(), which flushes the WAL to disk —
//   the same guarantee as SQLite synchronous=full at every durability window.
//   For Axis B (best-reasonable) we leave WAL sync to OS buffering between
//   boundaries, matching SQLite synchronous=normal.
//
// THREAD SAFETY
//   RocksDB is safe for concurrent reads and writes from multiple threads
//   against the same DB instance.  Each iterator is independent.  Column
//   Family handles are thread-safe to use concurrently.

#include "bench/backend.h"
#include "bench/registry.h"

#include "rocksdb/db.h"
#include "rocksdb/options.h"
#include "rocksdb/slice.h"
#include "rocksdb/write_batch.h"

#include <cassert>
#include <cstring>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace bench {

// ---------------------------------------------------------------------------
// Timestamp encoding helpers — big-endian so RocksDB's bytewise comparator
// sorts keys in timestamp order.
// ---------------------------------------------------------------------------
static void encode_ts(uint64_t ts, char out[8]) {
    out[0] = static_cast<char>((ts >> 56) & 0xFF);
    out[1] = static_cast<char>((ts >> 48) & 0xFF);
    out[2] = static_cast<char>((ts >> 40) & 0xFF);
    out[3] = static_cast<char>((ts >> 32) & 0xFF);
    out[4] = static_cast<char>((ts >> 24) & 0xFF);
    out[5] = static_cast<char>((ts >> 16) & 0xFF);
    out[6] = static_cast<char>((ts >>  8) & 0xFF);
    out[7] = static_cast<char>( ts        & 0xFF);
}

static uint64_t decode_ts(const char* p) {
    return (static_cast<uint64_t>(static_cast<uint8_t>(p[0])) << 56)
         | (static_cast<uint64_t>(static_cast<uint8_t>(p[1])) << 48)
         | (static_cast<uint64_t>(static_cast<uint8_t>(p[2])) << 40)
         | (static_cast<uint64_t>(static_cast<uint8_t>(p[3])) << 32)
         | (static_cast<uint64_t>(static_cast<uint8_t>(p[4])) << 24)
         | (static_cast<uint64_t>(static_cast<uint8_t>(p[5])) << 16)
         | (static_cast<uint64_t>(static_cast<uint8_t>(p[6])) <<  8)
         |  static_cast<uint64_t>(static_cast<uint8_t>(p[7]));
}

// Value layout: [4-byte flags LE][payload bytes]
static std::string encode_value(const Frame& f) {
    std::string v(4 + f.size, '\0');
    uint32_t flags = f.flags;
    v[0] = static_cast<char>( flags        & 0xFF);
    v[1] = static_cast<char>((flags >>  8) & 0xFF);
    v[2] = static_cast<char>((flags >> 16) & 0xFF);
    v[3] = static_cast<char>((flags >> 24) & 0xFF);
    std::memcpy(&v[4], f.data, f.size);
    return v;
}

// ---------------------------------------------------------------------------
// RocksIterator
// ---------------------------------------------------------------------------
class RocksIterator : public Iterator {
public:
    RocksIterator(rocksdb::DB* db, rocksdb::ColumnFamilyHandle* cf)
        : _db(db), _cf(cf)
    {
        rocksdb::ReadOptions ro;
        ro.fill_cache = true;
        _iter.reset(_db->NewIterator(ro, _cf));
    }

    bool find(uint64_t timestamp_us) override {
        char key_buf[8];
        encode_ts(timestamp_us, key_buf);
        _iter->Seek(rocksdb::Slice(key_buf, 8));
        if (!_iter->Valid()) return false;
        _decode_current();
        return true;
    }

    bool valid() const override { return _iter->Valid(); }

    Frame current() const override { return _frame; }

    void next() override {
        _iter->Next();
        if (_iter->Valid()) _decode_current();
    }

    void prev() override {
        _iter->Prev();
        if (_iter->Valid()) _decode_current();
    }

private:
    void _decode_current() {
        auto k = _iter->key();
        auto v = _iter->value();
        _frame.timestamp_us = decode_ts(k.data());
        _frame.flags = static_cast<uint32_t>(static_cast<uint8_t>(v[0]))
                     | (static_cast<uint32_t>(static_cast<uint8_t>(v[1])) <<  8)
                     | (static_cast<uint32_t>(static_cast<uint8_t>(v[2])) << 16)
                     | (static_cast<uint32_t>(static_cast<uint8_t>(v[3])) << 24);
        _frame.data = reinterpret_cast<const uint8_t*>(v.data()) + 4;
        _frame.size = v.size() - 4;
    }

    rocksdb::DB*                              _db;
    rocksdb::ColumnFamilyHandle*              _cf;
    std::unique_ptr<rocksdb::Iterator>        _iter;
    Frame                                     _frame{};
};

// ---------------------------------------------------------------------------
// RocksBackend
// ---------------------------------------------------------------------------
class RocksBackend : public Backend {
public:
    RocksBackend() = default;
    ~RocksBackend() override { if (_db) teardown(); }

    void setup(const std::string& path, const BackendConfig& cfg) override {
        _cfg  = cfg;
        _path = path;

        rocksdb::Options opts;
        opts.create_if_missing              = true;
        opts.create_missing_column_families = true;

        // Write buffer = durability_bytes: flush to L0 roughly every
        // durability window, which is comparable to nanots's block boundary.
        opts.write_buffer_size = cfg.durability_bytes;

        // Parallelism
        opts.IncreaseParallelism();
        opts.OptimizeLevelStyleCompaction();

        // Open with just the default CF; streams get added on first write.
        rocksdb::Status s = rocksdb::DB::Open(opts, path, &_db);
        if (!s.ok())
            throw std::runtime_error("RocksDB open: " + s.ToString());

        _default_cf = _db->DefaultColumnFamily();
    }

    void teardown() override {
        // Close non-default column family handles before closing DB.
        {
            std::lock_guard<std::mutex> lk(_cf_mu);
            for (auto& [name, handle] : _cf_handles) {
                if (handle != _db->DefaultColumnFamily())
                    _db->DestroyColumnFamilyHandle(handle);
            }
            _cf_handles.clear();
        }
        delete _db;
        _db = nullptr;
    }

    void write(const std::string& stream, const Frame& frame) override {
        auto* cf = get_or_create_cf(stream);
        char key_buf[8];
        encode_ts(frame.timestamp_us, key_buf);
        std::string val = encode_value(frame);

        rocksdb::WriteOptions wo;
        wo.disableWAL = false;
        rocksdb::Status s = _db->Put(wo, cf,
                                     rocksdb::Slice(key_buf, 8),
                                     rocksdb::Slice(val));
        if (!s.ok())
            throw std::runtime_error("RocksDB Put: " + s.ToString());
    }

    void sync_boundary(size_t /*bytes*/) override {
        rocksdb::Status s = _db->SyncWAL();
        if (!s.ok())
            throw std::runtime_error("RocksDB SyncWAL: " + s.ToString());
    }

    std::unique_ptr<Iterator> iterate(const std::string& stream) override {
        auto* cf = get_or_create_cf(stream);
        return std::make_unique<RocksIterator>(_db, cf);
    }

    std::string name()    const override { return "rocksdb"; }
    std::string version() const override {
        return rocksdb::GetRocksVersionAsString();
    }
    std::string config_summary() const override {
        std::ostringstream s;
        s << "write_buffer=" << (_cfg.durability_bytes / (1024*1024)) << "MB";
        return s.str();
    }

private:
    rocksdb::ColumnFamilyHandle* get_or_create_cf(const std::string& stream) {
        {
            std::lock_guard<std::mutex> lk(_cf_mu);
            auto it = _cf_handles.find(stream);
            if (it != _cf_handles.end()) return it->second;
        }

        rocksdb::ColumnFamilyHandle* handle = nullptr;
        rocksdb::Status s = _db->CreateColumnFamily(
            rocksdb::ColumnFamilyOptions(), stream, &handle);
        if (!s.ok())
            throw std::runtime_error("RocksDB CreateColumnFamily: " + s.ToString());

        std::lock_guard<std::mutex> lk(_cf_mu);
        // Another thread may have raced us — use the winner's handle.
        auto [it, inserted] = _cf_handles.emplace(stream, handle);
        if (!inserted) {
            _db->DestroyColumnFamilyHandle(handle);
        }
        return it->second;
    }

    BackendConfig                _cfg;
    std::string                  _path;
    rocksdb::DB*                 _db          = nullptr;
    rocksdb::ColumnFamilyHandle* _default_cf  = nullptr;

    std::mutex                                              _cf_mu;
    std::unordered_map<std::string, rocksdb::ColumnFamilyHandle*> _cf_handles;
};

REGISTER_BACKEND(RocksBackend, "rocksdb");

} // namespace bench
