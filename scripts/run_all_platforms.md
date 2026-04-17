# Cross-Platform Benchmark Checklist

This document is a manual checklist for collecting benchmark results across
platforms.  We are not automating cross-platform CI yet; this formalizes the
process so results are consistent and reproducible.

---

## Before You Start

1. Build the benchmark in Release mode on the target platform:
   ```sh
   cmake -B build -DCMAKE_BUILD_TYPE=Release
   cmake --build build -j$(nproc)
   # Windows (MSVC):
   cmake -B build -G "Visual Studio 17 2022"
   cmake --build build --config Release
   ```

2. Verify the binary starts cleanly:
   ```sh
   ./build/bench --list-backends
   ./build/bench --list-workloads
   ```

3. Record hardware details in `results/<platform>/HARDWARE.md`.
   Use the template in the top-level [HARDWARE.md](../HARDWARE.md).
   Include at minimum: CPU model, RAM, storage type (NVMe/SATA SSD/HDD),
   OS version, and compiler version.

---

## Platform Checklist

### Linux (bare metal — preferred)

- [ ] Build with GCC or Clang in Release mode.
- [ ] Run `./scripts/run_all.sh --backends nanots,sqlite`.
- [ ] Copy results to `results/linux/`.
- [ ] Fill in `results/linux/HARDWARE.md`.

> **WSL2 note**: WSL2 has known-unusual fsync semantics — `msync` and
> `FlushFileBuffers` calls may be rerouted through the Windows host in ways
> that don't reflect native Linux performance.  WSL2 results are interesting
> but should be labelled clearly and not used as the primary Linux data point.
> Prefer bare metal or a VM with direct disk access.

### Windows

- [ ] Build with MSVC (Visual Studio 2019 or 2022) in Release mode.
- [ ] Run `./scripts/run_all.sh --backends nanots,sqlite` from Git Bash or
      WSL2 (the script itself is portable; the binary must be a native Windows
      build).
- [ ] Copy results to `results/windows/`.
- [ ] Fill in `results/windows/HARDWARE.md`.

> **Note**: On Windows, nanots uses `FlushFileBuffers` at block boundaries,
> which is a synchronous flush of the file handle.  This is more expensive than
> Linux `msync(MS_SYNC)` on the same hardware; see [INVESTIGATION.md](../INVESTIGATION.md).

### macOS (if available)

- [ ] Build with Apple Clang in Release mode.
- [ ] Run `./scripts/run_all.sh --backends nanots,sqlite`.
- [ ] Copy results to `results/macos/`.
- [ ] Fill in `results/macos/HARDWARE.md`.

> **Note**: nanots uses `msync(MS_SYNC)` on macOS (no `F_FULLFSYNC`).
> Apple Silicon (M-series) results are particularly interesting for the
> `concurrent_readers` workload due to the high memory bandwidth.

---

## Result Directory Layout

```
results/
  linux/
    HARDWARE.md
    nanots_sustained_axisA.json
    nanots_sustained_axisB.json
    nanots_seek_axisA.json
    ...
  windows/
    HARDWARE.md
    nanots_sustained_axisA.json
    ...
  macos/
    HARDWARE.md
    ...
```

Results committed to a platform subdirectory are considered published.
Do not overwrite published results — create a new subdirectory if the hardware
or software configuration changes (e.g. `linux_2024_nvme/`).

---

## After Collecting Results

1. Update the top-level README.md with a summary table if publishing a
   comparison.
2. Verify that every result JSON contains the `environment.flush_mechanism`
   field so platform differences are self-documenting.
3. Verify that every result JSON's `fairness_axis` matches the config used:
   - Axis A: `sqlite-synchronous=full`, `durability-bytes=1048576`
   - Axis B: `sqlite-synchronous=normal`, `durability-bytes=10485760`
