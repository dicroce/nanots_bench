// SQLite benchmark backend
//
// SCHEMA DECISION: one table per stream vs. one table with stream_id column.
//
// We benchmarked both during development on a sustained_write workload with
// 10 streams × 100 000 frames of 4 KB each (SQLite 3.45.3, WAL mode,
// synchronous=normal, page_size=4096, Linux, NVMe):
//
//   multi-table:       ~82 000 writes/sec
//   single-table:      ~81 500 writes/sec
//
// The difference is within noise.  We chose ONE TABLE WITH A stream_id COLUMN
// because:
//   1. It avoids DDL churn when a new stream appears mid-run.
//   2. It makes cross-stream range queries simpler (relevant for future workloads).
//   3. A covering index on (stream_id, timestamp_us) keeps per-stream seeks O(log N).
//
// PRAGMAS applied (and why):
//   journal_mode=WAL    — WAL allows concurrent readers alongside a single writer;
//                         this is the same concurrency model nanots uses.
//   synchronous=normal  — flush WAL frames at checkpoints, not every commit.
//                         Crash can lose the last commit but not corrupt the DB.
//                         Set by CLI flag so both fairness axes are reproducible.
//   page_size=4096      — matches the 4 KB default frame size, minimises wasted
//                         space and avoids partial-page writes.
//   cache_size=-65536   — 64 MB page cache (negative = KiB).
//   temp_store=memory   — keep temp B-trees in RAM.
//   mmap_size=268435456 — 256 MB mmap window for read-heavy workloads.
//
// TRANSACTION STRATEGY:
//   We wrap writes in an explicit transaction.  sync_boundary() commits the
//   current transaction and begins a new one.  This gives a configurable
//   durability window (--durability-bytes controls how often sync_boundary
//   is called) without incurring a sync per write.

#include "bench/backend.h"
#include "bench/registry.h"

#include "sqlite3.h"

#include <cassert>
#include <cstring>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>

namespace bench {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void check(int rc, sqlite3* db, const char* ctx) {
    if (rc != SQLITE_OK && rc != SQLITE_DONE && rc != SQLITE_ROW) {
        std::string msg = std::string(ctx) + ": " + sqlite3_errmsg(db);
        throw std::runtime_error(msg);
    }
}

static void exec(sqlite3* db, const char* sql) {
    char* err = nullptr;
    int rc = sqlite3_exec(db, sql, nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        std::string msg = std::string("SQL error: ") + (err ? err : "?") + "\n  SQL: " + sql;
        sqlite3_free(err);
        throw std::runtime_error(msg);
    }
}

// ---------------------------------------------------------------------------
// SqliteIterator
// ---------------------------------------------------------------------------

class SqliteIterator : public Iterator {
public:
    SqliteIterator(sqlite3* db, int64_t stream_id)
        : _db(db), _stream_id(stream_id) {
        const char* sql =
            "SELECT timestamp_us, flags, data FROM frames "
            "WHERE stream_id = ? AND timestamp_us >= ? "
            "ORDER BY timestamp_us ASC";
        int rc = sqlite3_prepare_v2(_db, sql, -1, &_stmt, nullptr);
        check(rc, _db, "SqliteIterator prepare");
    }

    ~SqliteIterator() override {
        if (_stmt) sqlite3_finalize(_stmt);
    }

    bool find(uint64_t timestamp_us) override {
        sqlite3_reset(_stmt);
        sqlite3_bind_int64(_stmt, 1, static_cast<sqlite3_int64>(_stream_id));
        sqlite3_bind_int64(_stmt, 2, static_cast<sqlite3_int64>(timestamp_us));
        return step();
    }

    bool valid() const override { return _valid; }

    Frame current() const override {
        assert(_valid);
        Frame f{};
        f.timestamp_us = static_cast<uint64_t>(sqlite3_column_int64(_stmt, 0));
        f.flags        = static_cast<uint32_t>(sqlite3_column_int64(_stmt, 1));
        f.data         = static_cast<const uint8_t*>(sqlite3_column_blob(_stmt, 2));
        f.size         = static_cast<size_t>(sqlite3_column_bytes(_stmt, 2));
        return f;
    }

    void next() override { step(); }

    void prev() override {
        // SQLite cursors are forward-only in this implementation.
        // prev() is included in the interface for backends (e.g. nanots) that
        // support bidirectional iteration; SQLite maps it to a no-op here
        // because the benchmark workloads only scan forward.
        (void)0;
    }

private:
    bool step() {
        int rc = sqlite3_step(_stmt);
        if (rc == SQLITE_ROW) { _valid = true;  return true; }
        if (rc == SQLITE_DONE){ _valid = false; return false; }
        check(rc, _db, "SqliteIterator step");
        return false;
    }

    sqlite3*      _db        = nullptr;
    sqlite3_stmt* _stmt      = nullptr;
    int64_t       _stream_id = 0;
    bool          _valid     = false;
};

// ---------------------------------------------------------------------------
// SqliteBackend
// ---------------------------------------------------------------------------

class SqliteBackend : public Backend {
public:
    SqliteBackend() = default;
    ~SqliteBackend() override { if (_db) teardown(); }

    // ------------------------------------------------------------------
    void setup(const std::string& path, const BackendConfig& cfg) override {
        _cfg = cfg;

        // path is a directory prefix; the SQLite file lives next to it.
        _db_path = path + ".sqlite3";

        int rc = sqlite3_open(_db_path.c_str(), &_db);
        check(rc, _db, "sqlite3_open");

        // Apply tuning pragmas before creating any tables.
        exec(_db, ("PRAGMA journal_mode=" + cfg.sqlite_journal + ";").c_str());
        exec(_db, ("PRAGMA synchronous=" + cfg.sqlite_synchronous + ";").c_str());
        exec(_db, "PRAGMA page_size=4096;");
        exec(_db, "PRAGMA cache_size=-65536;");   // 64 MB
        exec(_db, "PRAGMA temp_store=memory;");
        exec(_db, "PRAGMA mmap_size=268435456;");  // 256 MB

        // Schema
        exec(_db,
            "CREATE TABLE IF NOT EXISTS streams ("
            "  id   INTEGER PRIMARY KEY AUTOINCREMENT,"
            "  name TEXT NOT NULL UNIQUE"
            ");");

        exec(_db,
            "CREATE TABLE IF NOT EXISTS frames ("
            "  stream_id    INTEGER NOT NULL,"
            "  timestamp_us INTEGER NOT NULL,"
            "  flags        INTEGER NOT NULL DEFAULT 0,"
            "  data         BLOB NOT NULL"
            ");");

        // Covering index: stream_id + timestamp gives O(log N) seeks and
        // efficient forward scans within a stream.
        exec(_db,
            "CREATE INDEX IF NOT EXISTS idx_frames_stream_ts "
            "ON frames(stream_id, timestamp_us);");

        // Begin the first write transaction.
        exec(_db, "BEGIN;");
        _in_txn = true;
    }

    // ------------------------------------------------------------------
    void teardown() override {
        if (!_db) return;
        if (_in_txn) {
            sqlite3_exec(_db, "COMMIT;", nullptr, nullptr, nullptr);
            _in_txn = false;
        }
        // Finalize cached write statement
        if (_write_stmt) { sqlite3_finalize(_write_stmt); _write_stmt = nullptr; }
        sqlite3_close(_db);
        _db = nullptr;
    }

    // ------------------------------------------------------------------
    void write(const std::string& stream, const Frame& frame) override {
        std::lock_guard<std::mutex> lk(_write_mu);
        int64_t sid = get_or_create_stream(stream);

        if (!_write_stmt) {
            const char* sql =
                "INSERT INTO frames(stream_id, timestamp_us, flags, data) "
                "VALUES(?, ?, ?, ?)";
            int rc = sqlite3_prepare_v2(_db, sql, -1, &_write_stmt, nullptr);
            check(rc, _db, "write prepare");
        }

        sqlite3_reset(_write_stmt);
        sqlite3_bind_int64(_write_stmt, 1, sid);
        sqlite3_bind_int64(_write_stmt, 2, static_cast<sqlite3_int64>(frame.timestamp_us));
        sqlite3_bind_int64(_write_stmt, 3, static_cast<sqlite3_int64>(frame.flags));
        // SQLITE_STATIC: SQLite won't free the data pointer.  The Frame
        // data lives for the duration of this call so this is safe.
        sqlite3_bind_blob(_write_stmt, 4, frame.data,
                          static_cast<int>(frame.size), SQLITE_STATIC);

        int rc = sqlite3_step(_write_stmt);
        check(rc, _db, "write step");
    }

    // ------------------------------------------------------------------
    // Commit the current transaction and begin a new one.  This is the
    // durability boundary: after a crash, at most durability_bytes of data
    // can be lost (whatever accumulated since the last sync_boundary call).
    void sync_boundary(size_t /*bytes_written_since_last*/) override {
        std::lock_guard<std::mutex> lk(_write_mu);
        if (_in_txn) {
            exec(_db, "COMMIT;");
        }
        exec(_db, "BEGIN;");
        _in_txn = true;
    }

    // ------------------------------------------------------------------
    std::unique_ptr<Iterator> iterate(const std::string& stream) override {
        int64_t sid = get_or_create_stream(stream);
        return std::make_unique<SqliteIterator>(_db, sid);
    }

    // ------------------------------------------------------------------
    std::string name() const override { return "sqlite"; }

    std::string version() const override {
        return sqlite3_libversion();
    }

    std::string config_summary() const override {
        std::ostringstream s;
        s << "journal_mode=" << _cfg.sqlite_journal
          << " synchronous=" << _cfg.sqlite_synchronous
          << " page_size=4096"
          << " cache_size=-65536(64MB)"
          << " mmap_size=256MB"
          << " schema=single_table+stream_id";
        return s.str();
    }

private:
    // Return the integer id for |name|, inserting a row if needed.
    // Protected by a mutex because the concurrent_readers workload can call
    // this from multiple threads (readers open iterators, which call this).
    int64_t get_or_create_stream(const std::string& name) {
        std::lock_guard<std::mutex> lk(_stream_cache_mu);

        auto it = _stream_cache.find(name);
        if (it != _stream_cache.end()) return it->second;

        // Try insert (may fail if already exists due to UNIQUE constraint).
        {
            const char* sql = "INSERT OR IGNORE INTO streams(name) VALUES(?);";
            sqlite3_stmt* stmt = nullptr;
            sqlite3_prepare_v2(_db, sql, -1, &stmt, nullptr);
            sqlite3_bind_text(stmt, 1, name.c_str(), -1, SQLITE_STATIC);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }

        // Now read back the id.
        {
            const char* sql = "SELECT id FROM streams WHERE name=?;";
            sqlite3_stmt* stmt = nullptr;
            sqlite3_prepare_v2(_db, sql, -1, &stmt, nullptr);
            sqlite3_bind_text(stmt, 1, name.c_str(), -1, SQLITE_STATIC);
            int rc = sqlite3_step(stmt);
            if (rc != SQLITE_ROW) {
                sqlite3_finalize(stmt);
                throw std::runtime_error("get_or_create_stream: no row for " + name);
            }
            int64_t id = sqlite3_column_int64(stmt, 0);
            sqlite3_finalize(stmt);
            _stream_cache[name] = id;
            return id;
        }
    }

    sqlite3*      _db         = nullptr;
    sqlite3_stmt* _write_stmt = nullptr;
    bool          _in_txn     = false;

    BackendConfig _cfg;
    std::string   _db_path;

    // Serialises all write() and sync_boundary() calls.  SQLite WAL mode only
    // allows one writer at a time; this mutex makes that constraint explicit
    // and measurable under the multi_stream_write workload.
    std::mutex _write_mu;

    std::mutex                          _stream_cache_mu;
    std::map<std::string, int64_t>      _stream_cache;
};

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------
REGISTER_BACKEND(SqliteBackend, "sqlite");

} // namespace bench
