// nanots benchmark backend
//
// Wraps the nanots C++ API for use with the bench harness.
//
// DURABILITY MAPPING
//   nanots durability is determined entirely by block size: a crash can lose at
//   most one in-flight block worth of data.  We map the harness's
//   --durability-bytes flag directly to nanots block size so that the fairness
//   axes are honoured:
//
//     Axis A: --durability-bytes 1048576  → block_size = 1 MB
//     Axis B: --durability-bytes 10485760 → block_size = 10 MB
//
//   sync_boundary() is therefore a no-op at runtime.
//
// BLOCK COUNT
//   We need enough blocks so the benchmark's data fits without recycling during
//   a run.  The auto formula is:
//
//     n_blocks = max(16, ceil(expected_bytes / block_size) + 4)
//
//   where expected_bytes = num_frames × frame_size, estimated conservatively as
//   2 × durability_bytes × 128 (a large but not unlimited cap).  The user can
//   override with --nanots-num-blocks.
//
// THREAD SAFETY
//   nanots_writer is single-writer.  The concurrent_readers workload calls
//   write() from one thread and iterate() from many reader threads.  The writer
//   and each reader use entirely separate nanots objects:
//     - Writer:  one nanots_writer + per-stream write_context (writer thread only).
//     - Readers: each reader constructs its own nanots_iterator (reader thread only).
//   No shared mutable state between writer and readers in this backend.
//
// SQLITE DEPENDENCY
//   nanots uses SQLite internally for its metadata database.  We compile
//   amalgamated_src/nanots.cpp against our vendored SQLite 3.45.3 (see
//   CMakeLists.txt) rather than nanots's bundled 3.38.5, to avoid duplicate
//   symbol linker errors.  The API delta between the two versions is backward-
//   compatible for all functions nanots uses.

#include "bench/backend.h"
#include "bench/registry.h"

// nanots amalgamated header — pulls in utils.h, sqlite3.h (3.38.5 decls),
// and all nanots class declarations.
#include "nanots.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace bench {

// ---------------------------------------------------------------------------
// NanotsIterator
// ---------------------------------------------------------------------------

class NanotsIterator : public Iterator {
public:
    explicit NanotsIterator(const std::string& file_path,
                            const std::string& stream_tag)
        : _iter(file_path, stream_tag) {}

    // Seek to first frame >= timestamp_us.
    // nanots uses int64_t timestamps; our Frame uses uint64_t.
    // Safe cast: timestamps used by the harness are well within int64_t range.
    bool find(uint64_t timestamp_us) override {
        return _iter.find(static_cast<int64_t>(timestamp_us));
    }

    bool valid() const override { return _iter.valid(); }

    // Return a Frame backed by the nanots memory-mapped region.
    //
    // LIFETIME: the pointers in the returned Frame are valid until the next
    // call to next(), prev(), or find() on this iterator.  The harness
    // workloads consume the frame before advancing, so this is safe.  Future
    // workload authors must not hold Frame::data across iterator movement.
    Frame current() const override {
        const frame_info& fi = *_iter;
        Frame f{};
        f.data         = fi.data;
        f.size         = fi.size;
        f.timestamp_us = static_cast<uint64_t>(fi.timestamp);
        f.flags        = static_cast<uint32_t>(fi.flags);
        return f;
    }

    void next() override { ++_iter; }
    void prev() override { --_iter; }

private:
    nanots_iterator _iter;
};

// ---------------------------------------------------------------------------
// NanotsBackend
// ---------------------------------------------------------------------------

class NanotsBackend : public Backend {
public:
    NanotsBackend() = default;
    ~NanotsBackend() override { if (_writer) teardown(); }

    // -----------------------------------------------------------------------
    void setup(const std::string& path, const BackendConfig& cfg) override {
        _cfg      = cfg;
        _db_path  = path + "/data.nts";

        // Block size = durability window.  nanots enforces a minimum of
        // FILE_HEADER_BLOCK_SIZE (65 536 bytes) and a power-of-2 alignment is
        // not required — but we clamp to at least 64 KB to be safe.
        // Parenthesized std::max prevents Windows.h max() macro expansion.
        _block_size = static_cast<uint32_t>(
            (std::max)(static_cast<size_t>(65536), cfg.durability_bytes));

        // Determine block count.
        uint32_t n_blocks = cfg.nanots_num_blocks;
        if (n_blocks == 0) {
            // Auto-derive from expected_write_bytes if provided, otherwise
            // fall back to 256 blocks (generous default for ad-hoc use).
            size_t expected_bytes = (cfg.expected_write_bytes > 0)
                ? static_cast<size_t>(cfg.expected_write_bytes)
                : static_cast<size_t>(_block_size) * 256;
            // Add 4 spare blocks; floor at 16.
            size_t needed = (expected_bytes + _block_size - 1) / _block_size + 4;
            n_blocks = static_cast<uint32_t>((std::max)(static_cast<size_t>(16), needed));
        }
        _n_blocks = n_blocks;

        // Ensure the directory exists.
        {
#if defined(_WIN32)
            std::string cmd = "mkdir \"" + path + "\" 2>nul";
#else
            std::string cmd = "mkdir -p \"" + path + "\"";
#endif
            (void)std::system(cmd.c_str());
        }

        // Allocate the nanots file if it doesn't already exist.
        if (!file_exists(_db_path)) {
            nanots_writer::allocate(_db_path, _block_size, _n_blocks);
        }

        // auto_reclaim=false: don't recycle blocks during a run — recycling
        // would invalidate outstanding iterators (concurrent_readers workload).
        // auto_reclaim=true: recycle oldest finalized blocks when the free list
        // is empty.  Required for write-only workloads (multi_stream_write)
        // where the run duration is unbounded and there are no concurrent readers.
        _writer = std::make_unique<nanots_writer>(_db_path, cfg.nanots_auto_reclaim);
    }

    // -----------------------------------------------------------------------
    void teardown() override {
        // Destroy write contexts first so their destructors release any
        // in-flight state before the writer is torn down.
        _write_contexts.clear();
        _writer.reset();
    }

    // -----------------------------------------------------------------------
    void write(const std::string& stream, const Frame& frame) override {
        assert(_writer);
        write_context& wctx = get_or_create_context(stream);
        _writer->write(wctx,
                       frame.data,
                       frame.size,
                       static_cast<int64_t>(frame.timestamp_us),
                       static_cast<uint8_t>(frame.flags & 0xFF));
    }

    // -----------------------------------------------------------------------
    // No-op: nanots durability is determined by block size at allocate() time,
    // not by runtime calls.  Fairness is established by mapping block_size to
    // --durability-bytes in setup(), so the two systems lose the same amount of
    // data on a crash without any runtime coordination needed here.
    void sync_boundary(size_t /*bytes_written_since_last*/) override {}

    // -----------------------------------------------------------------------
    std::unique_ptr<Iterator> iterate(const std::string& stream) override {
        // Each iterator is independent — safe to call concurrently from
        // multiple reader threads while the writer thread is also active.
        return std::make_unique<NanotsIterator>(_db_path, stream);
    }

    // -----------------------------------------------------------------------
    std::string name()    const override { return "nanots"; }

    std::string version() const override {
        // nanots doesn't expose a programmatic version query in v1.
        // Record the submodule commit when it's relevant for reproducibility.
        // TODO: read from nanots.version or a CMake-generated header once available.
        return "git-submodule";
    }

    std::string config_summary() const override {
        std::ostringstream s;
        s << "block_size=" << _block_size
          << " num_blocks=" << _n_blocks
          << " auto_reclaim=" << (_cfg.nanots_auto_reclaim ? "true" : "false");
        return s.str();
    }

private:
    // Return (creating if needed) the write_context for |stream|.
    // write_context is move-only and non-copyable, so we store it by value in
    // the map and use try_emplace to construct in-place.
    // The mutex protects the map lookup/insert only.  Once a context exists
    // for a stream it is only ever touched by that stream's single writer
    // thread, so the write itself remains lock-free.
    write_context& get_or_create_context(const std::string& stream) {
        std::lock_guard<std::mutex> lk(_ctx_mu);
        auto it = _write_contexts.find(stream);
        if (it != _write_contexts.end()) return it->second;

        // Pass non-empty metadata: nanots's SQLite result reader treats
        // empty strings as nulls (see exec() in nanots.cpp), so passing ""
        // causes bad_optional_access in the iterator.
        auto [ins_it, ok] = _write_contexts.try_emplace(
            stream,
            _writer->create_write_context(stream, /*metadata=*/"bench"));
        (void)ok;
        return ins_it->second;
    }

    std::string   _db_path;
    uint32_t      _block_size = 0;
    uint32_t      _n_blocks   = 0;
    BackendConfig _cfg;

    std::mutex                                 _ctx_mu;
    std::unique_ptr<nanots_writer>             _writer;
    std::map<std::string, write_context>       _write_contexts;
};

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------
REGISTER_BACKEND(NanotsBackend, "nanots");

} // namespace bench
