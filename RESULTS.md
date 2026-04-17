# nanots vs RocksDB vs SQLite Benchmark Results

**Platform:** Windows 11 Pro 10.0.26200, MSVC Release build  
**Frame size:** 4096 bytes  
**nanots:** amalgamated source (git submodule)  
**RocksDB:** v9.7.3 (FetchContent)  
**SQLite:** 3.45.3 (vendored amalgamation)

---

## Methodology

Each workload runs under two **fairness axes**:

| Axis | durability-bytes | SQLite synchronous | What it means |
|------|-----------------|-------------------|---------------|
| **A** | 1 MB | `full` | Equal durability — all systems flush at every ~1 MB boundary |
| **B** | 10 MB | `normal` | Best-reasonable — each system tuned for its natural sweet spot |

For RocksDB, `sync_boundary()` calls `SyncWAL()` — the same guarantee as SQLite `synchronous=full` at every durability window.

The `random_seek` workload creates one iterator and reuses it across all seeks, measuring seek latency rather than connection-open overhead.

---

## Results by Workload

### 1. Sustained Write — single stream, single writer

| Backend | Axis | writes/sec | p50 (µs) | p99 (µs) |
|---------|------|-----------|---------|---------|
| **RocksDB** | A | **57,016** | 5.6 | 32.4 |
| **RocksDB** | B | **107,665** | 5.7 | 26.4 |
| nanots | A | 11,306 | 0.9 | 19.5 |
| nanots | B | 92,661 | 0.8 | 11.2 |
| SQLite | A | 9,495 | 1.1 | 10.3 |
| SQLite | B | 9,629 | 1.1 | 5.0 |

**Takeaway:** RocksDB wins single-stream writes at both axes. Its LSM-tree is optimised for sequential appends — writes land in an in-memory memtable first and are flushed asynchronously, so there is minimal per-write overhead. nanots is competitive at Axis B (large blocks, infrequent fsyncs) but trails at Axis A because each block boundary requires a full msync. SQLite's per-frame INSERT overhead limits it to ~9–10K w/s regardless of durability setting.

---

### 2. Random Seek — seek to a random timestamp in a 100K-frame dataset

| Backend | Axis | seeks/sec | p50 (µs) | p99 (µs) |
|---------|------|----------|---------|---------|
| **SQLite** | A | **163,026** | 4.9 | 16.0 |
| **RocksDB** | A | **145,520** | 5.9 | 33.9 |
| **SQLite** | B | **157,008** | 5.0 | 17.3 |
| **RocksDB** | B | **154,034** | 5.9 | 21.3 |
| nanots | A | 26,044 | 34.6 | 99.1 |
| nanots | B | 51,406 | 17.6 | 47.6 |

**Takeaway:** RocksDB and SQLite are essentially tied — both around 150–163K seeks/sec with ~5–6 µs p50. Both have an in-process sorted index (B-tree / SST) that locates any key in microseconds. nanots is 3–6× slower because every `find()` issues a SQL query against its internal metadata database to locate the right mmap block, adding ~17–35 µs of fixed overhead per seek. This is an architectural gap that no tuning resolves.

---

### 3. Concurrent Readers — 1 writer + K readers, single stream

Writer throughput as K readers scale from 1 → 32 (Axis B):

| K | nanots w/s | RocksDB w/s | SQLite w/s |
|---|-----------|------------|-----------|
| 1  | 72,895 | 21,834 | 14,336 |
| 2  | 149,058 | 13,568 | 23,040 |
| 4  | 121,173 | 25,780 | 17,152 |
| 8  |  79,852 | 19,712 |  7,366 |
| 16 |  70,473 | 27,392 |  3,417 |
| 32 |  33,462 | 19,792 |    799 |

Reader throughput at K=1 / K=32 (Axis B):

| Backend | K=1 seeks/s | K=32 seeks/s |
|---------|------------|-------------|
| nanots  | 21,528 | 951 |
| RocksDB | 7,015 | 710 |
| SQLite  | 438 | 40 |

![Concurrent reader scaling](results/plot_reader_scaling.png)

**Takeaway:** nanots writer throughput degrades gracefully and stays highest throughout. RocksDB fluctuates without a clear trend — its compaction background threads compete with both the foreground writer and readers, creating noise. SQLite collapses to 5.6% of baseline at K=32. nanots readers are fastest (21K seeks/s at K=1) because each holds an independent iterator; RocksDB readers are slower here due to the sequential access pattern in this workload not playing to its SST strengths.

---

### 4. Multi-stream Write — K writers to K independent streams

Total throughput across all writers (Axis B):

| K | nanots total w/s | RocksDB total w/s | SQLite total w/s |
|---|-----------------|------------------|-----------------|
| 1 |  73,262 | 25,808 | 44,388 |
| 2 | 159,705 | 22,626 | 39,422 |
| 4 | 227,023 | 32,038 | 42,680 |
| 8 | 226,308 | 38,832 | 25,117 |
| **scale** | **3.09×** | **1.50×** | **0.57×** |

![Multi-stream write scaling](results/plot_writer_scaling.png)

**Takeaway:** nanots scales to 3× at K=8; RocksDB ekes out 1.5×; SQLite degrades to 57%. nanots wins because each stream gets a fully independent mmap write region with no shared state. RocksDB's compaction background activity and shared memtable locking limits scaling. SQLite's single WAL write lock serialises all writers regardless of stream.

---

### 5. Write-Read Contention — K writers + K readers simultaneously

This workload exposes each engine's concurrency architecture most clearly.

**Writers (Axis B):**

| K | nanots w/s | RocksDB w/s | SQLite w/s |
|---|-----------|------------|-----------|
| 1 | 119,398 | 15,786 | 19,122 |
| 2 | 133,088 | 19,284 |  6,620 |
| 4 | 168,831 | 25,765 |    784 |
| 8 | 170,352 | 25,409 |    608 |

**Readers (Axis B):**

| K | nanots seeks/s | RocksDB seeks/s | SQLite seeks/s |
|---|---------------|----------------|---------------|
| 1 |  36,220 |   385,490 | 3,542 |
| 2 |  58,973 |   727,793 | 3,786 |
| 4 |  88,018 | 1,223,476 | 4,104 |
| 8 |  67,249 | 1,604,367 | 4,082 |

![Write-read contention](results/plot_write_read_contention.png)

**Takeaway:** Each engine's architecture is written clearly in these numbers.

**nanots** is the best writer — 170K w/s at K=8, completely unaffected by readers. Each write_context owns a private mmap region; readers open independent iterators. There is no shared mutable state between the write and read paths.

**RocksDB** is the best reader — 1.6M seeks/sec at K=8, scaling nearly linearly. This is MVCC: every iterator holds a lightweight point-in-time snapshot. Reads never block writes and writes never block reads. The cost is writer throughput: at only 25K w/s, RocksDB's LSM compaction overhead limits writers regardless of reader count.

**SQLite** is worst at both under contention. Writers collapse from 19K to 608 w/s at K=8 (the WAL lock); readers flat-line at ~4K seeks/sec (shared `sqlite3*` under a mutex). The p50 write latency at K=8 is **94 ms** — each write is spending most of its time waiting for a lock.

---

## Summary

![Overall throughput](results/plot_throughput.png)

| Scenario | Winner | 2nd | Notes |
|----------|--------|-----|-------|
| Single-stream write (Axis A) | **RocksDB** 57K | SQLite 9.5K | nanots 11K |
| Single-stream write (Axis B) | **RocksDB** 108K | nanots 93K | SQLite 9.6K |
| Random seek | **SQLite** 163K ≈ **RocksDB** 145K | — | nanots 26–51K (3–6× behind) |
| Multi-stream write K=8 (Axis B) | **nanots** 226K | RocksDB 39K | SQLite 25K |
| Concurrent readers K=32 writer | **nanots** 33K | RocksDB 20K | SQLite 800 |
| Write-read contention K=8 writers | **nanots** 170K | RocksDB 25K | SQLite 600 |
| Write-read contention K=8 readers | **RocksDB** 1.6M | nanots 67K | SQLite 4K |

---

## Engine Profiles

### nanots
**Wins:** Multi-stream writes, writer throughput under reader pressure, write-read contention (writer side)  
**Loses:** Random seeks (3–6×), single-stream write at strict durability  
**Architecture:** Each stream owns an independent mmap block. Zero shared state between streams in the write path. Seek requires a SQL metadata query per `find()`.  
**Best for:** Multi-camera/multi-sensor recording, IoT data loggers, any workload with many independent producers writing time-stamped data simultaneously.

### RocksDB
**Wins:** Single-stream write throughput, concurrent read throughput (MVCC), random seeks (tied with SQLite)  
**Loses:** Multi-stream write scaling (LSM compaction background work limits parallelism), writer throughput under heavy read load  
**Architecture:** LSM-tree with MVCC snapshots. Writes are fast to the memtable. Reads scale linearly because each iterator is an independent snapshot. Background compaction is a constant overhead.  
**Best for:** Read-heavy workloads requiring concurrent access, key-value lookups, workloads that need both fast writes and fast random reads from a single writer.

### SQLite
**Wins:** Random seeks (tied with RocksDB), simple deployment (single file, no dependencies)  
**Loses:** Everything involving concurrent writers or concurrent readers under write pressure  
**Architecture:** B-tree with WAL mode. Single writer lock. Shared `sqlite3*` connection serialises readers. Strength is SQL query capability and reliability.  
**Best for:** Single-writer workloads, ad-hoc queries, transactional guarantees across tables, applications that already use SQLite and don't need concurrent write scaling.

---

## Honest Assessment

No engine wins everywhere. The right choice depends entirely on your access pattern:

- **Many concurrent writers → nanots** (independent mmap regions, no contention)
- **Many concurrent readers, few writers → RocksDB** (MVCC snapshots scale linearly)
- **Random point queries + SQL → SQLite or RocksDB** (B-tree / SST index)
- **Single writer, fast appends → RocksDB** (LSM memtable is extremely fast)

The most interesting finding is the write-read contention split: nanots and RocksDB each win one side of the same workload. nanots writes 170K frames/sec under 8 concurrent readers; RocksDB serves 1.6M reads/sec under 8 concurrent writers. They are complementary, not competing — nanots is optimised for the recording path, RocksDB for the query path.
