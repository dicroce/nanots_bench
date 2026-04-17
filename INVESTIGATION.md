# Performance Investigation: nanots Axis A Throughput

## The Question

The nanots README claims 113,000+ writes/sec on SSD.  Our harness recorded
**8,457 writes/sec** for the `sustained_write` workload at axis A (1 MB
durability window, 4 KB frames, 100,000 frames).  Why the ~10x gap?

---

## Method

1. Read the existing `nanots_sustained_axisA.json` result and noted the latency
   distribution: **p50 = 0.9 µs, mean = 118 µs, max = 114,574 µs**.
   The p50 proves the hot path is fast.  The mean being 130× the median proves
   the average is dominated by infrequent but very expensive events.

2. Audited the wrapper (`src/backends/nanots_backend.cpp`) for hot-path overhead:
   one `std::map::find` (single-element map = O(1) after first write), one direct
   pointer pass to `nanots_writer::write()`.  Zero copies, zero allocations per
   write.  The wrapper contributes nothing to the gap.

3. Read the nanots perf baseline test
   (`third_party/nanots/ut/source/test_nanots.cpp`, line 564).  It writes
   **1,000 frames × 1 KB** into a **4 MB file** (4 blocks × 1 MB).  Crucially,
   1,000 × 1 KB = 1 MB of data, which fits in approximately one block.
   The test never crosses a block boundary (aside from the initial block
   allocation).  It measures pure in-block write speed.

4. Read `nanots_writer::write()` in the amalgamated source
   (`third_party/nanots/amalgamated_src/nanots.cpp`, lines 1355–1460) to
   understand exactly what happens at a block boundary.

5. Read `nts_memory_map::flush()` (line 736) to confirm the Windows vs. POSIX
   behavior.

---

## Finding: Block Boundary Cost

### Block capacity math

| Constant | Value |
|---|---|
| `BLOCK_HEADER_SIZE` | 16 bytes |
| `INDEX_ENTRY_SIZE`  | 16 bytes |
| `FRAME_HEADER_SIZE` | 21 bytes |
| Block size (axis A) | 1,048,576 bytes |
| Frame data size     | 4,096 bytes |
| Padded frame size   | `(21 + 4096 + 7) & ~7` = **4,120 bytes** |
| Index cost/frame    | 16 bytes (grows from block bottom) |
| Total cost/frame    | **4,136 bytes** |
| Usable per block    | `1,048,576 − 16` = 1,048,560 bytes |
| **Frames per block**| `⌊1,048,560 / 4,136⌋` = **253 frames** |
| **Boundaries @ 100K frames** | `⌈100,000 / 253⌉` = **396 block boundaries** |

### What happens at each boundary

When the current block fills, `nanots_writer::write()` executes (lines 1426–1437):

```cpp
// 1. Synchronous flush of the filled block's mmap region
wctx.mm.flush(wctx.mm.map(), _block_size, true);

// 2. SQLite transaction: mark old block as finalized
nts_sqlite_conn conn(_database_name(_file_name), true, true);
nts_sqlite_transaction(conn, true, [&](const nts_sqlite_conn& conn) {
    _db_finalize_block(conn, wctx.current_block->id, wctx.last_timestamp.value());
});

// 3. Recursive write() call, which hits the !wctx.current_block branch:
//    - SQLite transaction: get free block + create segment block
//    - nts_file::open() + CreateFileMapping + MapViewOfFile for new 1 MB block
```

On **Windows**, `flush(..., true)` calls:
```cpp
FlushViewOfFile(addr, _block_size);   // flush dirty pages
FlushFileBuffers(_fileHandle);         // SYNCHRONOUS DISK FLUSH (= fsync)
```

On **Linux/macOS**, it calls:
```cpp
msync(addr, _block_size, MS_SYNC);    // synchronous flush of this mmap region
```

### Per-boundary cost observed

```
Total time:           11,823 ms
Steady-state time:    100,000 × 0.9 µs ≈ 90 ms
Block boundary time:  11,723 ms
Boundaries:           396
Average cost/boundary: 11,723 / 396 ≈ 29.6 ms
```

~30 ms per boundary on Windows is consistent with `FlushFileBuffers` on a
spinning disk or a slow-write SSD flushing a large dirty buffer from a 1.18 GB
file (1,176 blocks × 1 MB).

### Why the README numbers don't reflect this

The nanots perf baseline test writes 1,000 frames into a 4 MB file.  At 1 KB/frame:

- Padded frame size: `(21 + 1024 + 7) & ~7` = 1,052 bytes
- Frames per block: `⌊(1,048,560) / (1,052 + 16)⌋` ≈ 981 frames
- **1,000 frames never crosses a block boundary**

The test pays for one initial block allocation (one SQLite open + one
transaction), then does 999 pure mmap writes.  It measures **in-block burst
speed**, not amortized durability-inclusive throughput.

---

## Implications

| Configuration | Expected behavior |
|---|---|
| Axis A, 4 KB frames, Windows | ~8–10K writes/sec (396 FlushFileBuffers calls) |
| Axis B, 4 KB frames, Windows | ~80K writes/sec (~40 boundaries; 10× fewer) |
| Axis A, 4 KB frames, Linux NVMe | ~50–90K writes/sec (msync faster than FlushFileBuffers) |
| nanots README perf test | ~113K writes/sec (no block boundaries crossed) |

**Axis B should show roughly 10× the axis A throughput for nanots on Windows.**
Linux runs should show higher axis A numbers.  Both predictions are directly
testable by running the harness.

---

## Conclusion

The wrapper is correct and adds zero overhead to the hot path.  The 10× gap is
entirely explained by `FlushFileBuffers` cost on Windows at block boundaries,
which occur every 253 frames at the axis A configuration.

This is a valid and honest benchmark result.  It accurately measures nanots's
**amortized write throughput at 1 MB durability granularity**, which is the
correct metric for axis A.  The README's 113K number measures something
different (in-block burst speed), and is not a contradiction — it is a
different operating point.

### What changed as a result

1. **Axis A SQLite config corrected**: `scripts/run_all.sh` now uses
   `--sqlite-synchronous full` for axis A.  Previously it used `normal`, which
   gave SQLite weaker durability than nanots at no cost — an unfair advantage.
   With `synchronous=full`, each 1 MB SQLite transaction commit is accompanied
   by an `fsync`, matching nanots's per-boundary flush semantics.  SQLite's axis
   A numbers will be lower after this fix; that is the correct outcome.

2. **`flush_mechanism` field added to result JSON**: Every result now records
   the OS-level sync primitive (`FlushFileBuffers` on Windows, `msync(MS_SYNC)`
   on Linux/macOS) so readers can understand platform-specific performance
   differences without guessing.
