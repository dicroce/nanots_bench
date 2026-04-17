# nanots_bench

A reproducible benchmark harness for comparing [nanots](https://github.com/dicroce/nanots)
against embedded databases on time-series workloads.

**v1 status:** nanots, SQLite, and RocksDB backends are fully implemented and produce real numbers.

> **[Benchmark Results →](RESULTS.md)**

---

## Building

### Prerequisites

- CMake ≥ 3.16
- C++17 compiler (GCC 9+, Clang 10+, MSVC 2019+)
- Internet access at configure time — RocksDB is fetched via `FetchContent` (pinned to v9.7.3)
- SQLite is vendored in `third_party/sqlite/` (no download required)

### Build

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
# Binary: build/bench
```

On Windows (MSVC):
```cmd
cmake -B build -G "Visual Studio 17 2022"
cmake --build build --config Release
```

### Verify

```sh
./build/bench --list-backends
./build/bench --list-workloads
```

---

## Running

### Single run

Axis A (matched durability — `synchronous=full` for SQLite):
```sh
./build/bench \
    --backend sqlite \
    --workload sustained_write \
    --num-frames 1000000 \
    --frame-size 4096 \
    --durability-bytes 1048576 \
    --sqlite-synchronous full \
    --fairness-axis A \
    --output results/sqlite_sustained_axisA.json
```

Axis B (best-reasonable deployment — `synchronous=normal`):
```sh
./build/bench \
    --backend sqlite \
    --workload sustained_write \
    --num-frames 1000000 \
    --frame-size 4096 \
    --durability-bytes 10485760 \
    --sqlite-synchronous normal \
    --fairness-axis B \
    --output results/sqlite_sustained_axisB.json
```

### Full matrix (both axes, all workloads)

```sh
./scripts/run_all.sh
```

Results land in `results/`.  Pass `--backends sqlite` to restrict to one backend.

### Plotting (optional)

```sh
pip install matplotlib numpy
python3 scripts/plot_results.py results/*.json
# Produces results/plot_throughput.png, plot_latency.png, plot_reader_scaling.png
```

---

## Interpreting the JSON

```json
{
  "backend":   { "name": "sqlite", "version": "3.45.3", "config": "..." },
  "workload":  { "name": "sustained_write", "config": { ... } },
  "fairness_axis": "A",
  "environment": { "cpu": "...", "os": "Linux", "compiler": "GCC 13.2", "flush_mechanism": "msync(MS_SYNC)", "timestamp": "..." },
  "metrics": {
    "total_ops":     1000000,
    "total_seconds": 12.34,
    "ops_per_sec":   81037.0,
    "latency_us": { "mean": 9.1, "p50": 8.2, "p95": 14.1, "p99": 31.5, "max": 420.0 }
  }
}
```

- **latency_us** fields are in **microseconds**.
- `p99` is a better signal than `mean` for tail latency.
- `raw_latencies_us` is only present when `--emit-raw` is passed.
- The `concurrent_readers` workload adds a `thread_results[]` array and a
  `reader_scaling` string in `workload.config` that summarises writer throughput
  as reader count grows:
  ```
  "reader_scaling": "k=1: 82345 w/s | k=2: 81902 w/s | k=4: 81100 w/s | ..."
  ```

---

## Workloads

### `sustained_write`

Single writer.  Writes N frames of configurable size to one stream.
Calls `sync_boundary()` every `--durability-bytes` bytes written.
Reports per-write latency histogram and total throughput.

### `random_seek`

Prefills the database with M frames, then performs N random timestamp seeks.
Each seek opens a fresh iterator and calls `Iterator::find()`.
Reports seek latency distribution.

### `concurrent_readers`

One writer thread writes continuously while K reader threads do seek+scan
loops simultaneously.  K sweeps over 1, 2, 4, 8, 16, 32.
Each value of K runs for 10 seconds.

This is the headline workload.  It directly exercises the core architectural
claim of nanots: that its design allows readers to run without blocking the
writer, and the writer can run without blocking readers.  The result table
makes writer-throughput-vs-reader-count immediately visible.

---

## Methodology: Fairness

Every workload is run under exactly two axes.  **No other axes are run by
`scripts/run_all.sh`.**

### Axis A — matched durability

```
--durability-bytes 1048576   --sqlite-synchronous full
```

Both systems configured to lose at most ~1 MB of data on a hard crash.

- **nanots**: 1 MB blocks.  nanots calls `FlushFileBuffers` (Windows) /
  `msync(MS_SYNC)` (Linux/macOS) at every block boundary — a synchronous flush
  to durable storage at every block transition.
- **SQLite**: WAL mode with `synchronous=FULL`.  Each 1 MB transaction commit
  is followed by an `fsync`, matching nanots's per-boundary flush semantics.

Using `synchronous=normal` for SQLite at axis A would be unfair: WAL commits
under `normal` are not fsynced (they are deferred to checkpoint time), giving
SQLite weaker durability than nanots at no cost.  `synchronous=full` is the
correct apples-to-apples setting.

This is the primary comparison axis.

### Axis B — best-reasonable deployment

```
--durability-bytes 10485760  --sqlite-synchronous normal
```

Both systems tuned for a realistic video/IoT ingestion deployment where losing
a few seconds of data on an unexpected shutdown is acceptable.

- **nanots**: 10 MB blocks (fewer block-boundary flushes, higher steady-state
  throughput).
- **SQLite**: WAL mode with `synchronous=normal` and 10 MB transactions.  No
  per-commit fsync; the WAL is only synced at checkpoint time.

This axis answers: "what do you get when both systems are tuned for throughput
within a reasonable crash-loss budget?"

### Why there is no per-write-fsync axis

A per-write-fsync axis would measure the OS page-cache flush floor, not any
property of either database.  On a typical NVMe drive this is 200–2 000 µs per
operation; on a spinning disk it is 5–10 ms.  At those latencies both systems
converge to `1 / fsync_latency` regardless of architecture.

Including such an axis would obscure the interesting differences and mislead
readers into thinking either system can sustain per-write fsync at application
frame rates.  If you need per-write durability guarantees, the right answer is
a battery-backed write cache, not a benchmark axis.

### Methodology note: block-boundary cost

The investigation leading to the axis A correction is documented in
[INVESTIGATION.md](INVESTIGATION.md).  Short version: at axis A with 4 KB frames
and 1 MB blocks, nanots crosses a block boundary every 253 frames.  Each
boundary costs one synchronous OS flush.  The headline "8K writes/sec" number
for nanots on Windows reflects this correctly — it is the amortized cost of the
durability contract, not a wrapper overhead.  On Linux (faster `msync`) and with
larger blocks (axis B), the numbers improve substantially.

---

## Adding a New Backend

Adding DuckDB, LMDB, or any other engine is three steps:

### Step 1 — Vendor the dependency

**Option A: FetchContent (recommended for first integration)**

In `CMakeLists.txt`, inside the `# How to add a new backend` comment block:
```cmake
include(FetchContent)
FetchContent_Declare(duckdb
    GIT_REPOSITORY https://github.com/duckdb/duckdb.git
    GIT_TAG        v0.10.0   # always pin a version
)
FetchContent_MakeAvailable(duckdb)
target_link_libraries(bench_lib PUBLIC duckdb)
```

**Option B: git submodule (better for long-term maintenance)**
```sh
git submodule add https://github.com/duckdb/duckdb third_party/duckdb
# then add_subdirectory(third_party/duckdb) in CMakeLists.txt
```

### Step 2 — Implement the Backend interface

Create `src/backends/duckdb_backend.cpp`.  Implement all virtual methods:

```cpp
#include "bench/backend.h"
#include "bench/registry.h"
#include <duckdb.hpp>

namespace bench {

class DuckDBIterator : public Iterator {
    // ... implement find(), valid(), current(), next(), prev()
};

class DuckDBBackend : public Backend {
public:
    void setup(const std::string& path, const BackendConfig& cfg) override { ... }
    void teardown() override { ... }
    void write(const std::string& stream, const Frame& frame) override { ... }
    void sync_boundary(size_t bytes) override { ... }
    std::unique_ptr<Iterator> iterate(const std::string& stream) override { ... }
    std::string name()           const override { return "duckdb"; }
    std::string version()        const override { return duckdb::DuckDB::LibraryVersion(); }
    std::string config_summary() const override { return "..."; }
};

REGISTER_BACKEND(DuckDBBackend, "duckdb");

} // namespace bench
```

### Step 3 — Add to CMakeLists.txt

Add the new `.cpp` file to `bench_lib`'s sources list:
```cmake
src/backends/duckdb_backend.cpp
```

That's it.  No changes to `main.cpp`, `runner.cpp`, or any workload file.
`run_all.sh` will automatically pick up the new backend on next run.

---

## Reproducibility

Every result JSON records:

- Backend name + version string
- All backend configuration (pragmas, block size, etc.)
- Workload parameters (num_frames, frame_size, durability_bytes)
- Fairness axis label
- CPU model, OS, compiler, `flush_mechanism` (OS-level sync primitive), build timestamp

To reproduce a result, match all fields in `environment` and `backend.config`,
use the same `workload.config` values, and build from the same commit.

The SQLite amalgamation version is pinned in `third_party/sqlite/VERSION`.

---

## Pinned versions

| Component | Version  | Location                        |
|-----------|----------|---------------------------------|
| SQLite    | 3.45.3   | `third_party/sqlite/`           |
| RocksDB   | v9.7.3   | fetched via FetchContent        |
| nanots    | submodule | `third_party/nanots/`          |

---

## Hardware template

See [HARDWARE.md](HARDWARE.md) for the template to fill in when publishing results.
