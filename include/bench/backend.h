#pragma once
#include <cstdint>
#include <memory>
#include <string>

namespace bench {

// A single time-series record.
struct Frame {
    const uint8_t* data;
    size_t         size;
    uint64_t       timestamp_us;  // microseconds since epoch (or monotonic origin)
    uint32_t       flags;
};

// Forward cursor over frames in a stream.
class Iterator {
public:
    virtual ~Iterator() = default;

    // Seek to the first frame with timestamp >= timestamp_us.
    // Returns true if such a frame exists.
    virtual bool find(uint64_t timestamp_us) = 0;

    virtual bool  valid()   const = 0;
    virtual Frame current() const = 0;
    virtual void  next()          = 0;
    virtual void  prev()          = 0;
};

// Per-backend configuration knobs passed in via CLI.
struct BackendConfig {
    // General
    size_t   durability_bytes   = 1 * 1024 * 1024; // sync_boundary threshold
    uint32_t page_size          = 4096;

    // SQLite-specific (ignored by other backends)
    std::string sqlite_synchronous = "normal";  // off | normal | full | extra
    std::string sqlite_journal     = "WAL";

    // nanots-specific (ignored by other backends)
    // 0 means "auto-derive from durability_bytes + expected data volume".
    uint32_t nanots_num_blocks = 0;

    // When true, nanots recycles the oldest finalized block when no free block
    // is available.  Use for write-only workloads (multi_stream_write) where
    // there are no concurrent readers to invalidate.
    bool nanots_auto_reclaim = false;

    // Hint for nanots block count auto-sizing: total bytes the workload will write.
    // Set by main.cpp from num_frames × frame_size.  Zero means unknown.
    uint64_t expected_write_bytes = 0;
};

// Abstract storage backend.  One implementation per database engine.
//
// Lifecycle: setup() → [write/iterate]* → teardown()
//
// Thread safety: the concurrent_readers workload calls write() and iterate()
// from different threads simultaneously.  Backends must be safe for one writer
// + many readers at the same time (the same contract nanots makes).
class Backend {
public:
    virtual ~Backend() = default;

    // Called once before any workload runs.  |path| is the on-disk location;
    // the directory is guaranteed to exist and be empty.
    virtual void setup(const std::string& path, const BackendConfig& cfg) = 0;

    // Called once after all workloads finish.  Flush all data and close.
    virtual void teardown() = 0;

    // Write a single frame to a named stream.  May be called from the writer
    // thread while readers are active.
    virtual void write(const std::string& stream, const Frame& frame) = 0;

    // Durability boundary.  The workload calls this once every
    // BackendConfig::durability_bytes cumulative bytes written.
    //
    // SQLite: commit the current transaction and begin a new one.
    // nanots:  no-op at runtime — durability is set architecturally by block
    //          size; this call is a hook for backends that need it.
    // Default implementation: no-op.
    virtual void sync_boundary(size_t /*bytes_written_since_last*/) {}

    // Open a read cursor for a stream.  Can be called concurrently by multiple
    // reader threads.
    virtual std::unique_ptr<Iterator> iterate(const std::string& stream) = 0;

    // Metadata recorded in every result JSON.
    virtual std::string name()           const = 0;
    virtual std::string version()        const = 0;
    virtual std::string config_summary() const = 0;
};

} // namespace bench
