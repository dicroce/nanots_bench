#!/usr/bin/env python3
"""
plot_results.py — Generate charts from bench JSON result files.

Usage:
    python3 scripts/plot_results.py results/*.json

Produces:
    results/plot_throughput.png      — bar chart: ops/sec per (backend, workload, axis)
    results/plot_latency.png         — grouped bar chart: p50/p95/p99 latencies
    results/plot_reader_scaling.png  — line chart: writer throughput vs reader count
                                       (concurrent_readers workload only)

Requires: matplotlib, numpy  (pip install matplotlib numpy)
"""

import argparse
import json
import os
import sys
from pathlib import Path

try:
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    import numpy as np
except ImportError:
    print("ERROR: matplotlib and numpy are required.  Install with:")
    print("  pip install matplotlib numpy")
    sys.exit(1)


# ---------------------------------------------------------------------------
# Load results
# ---------------------------------------------------------------------------

def load_results(paths):
    results = []
    for p in paths:
        try:
            with open(p) as f:
                data = json.load(f)
            if "error" in data and data["error"]:
                print(f"Skipping {p}: contains error: {data['error']}")
                continue
            results.append(data)
        except Exception as e:
            print(f"Skipping {p}: {e}")
    return results


# ---------------------------------------------------------------------------
# Throughput chart
# ---------------------------------------------------------------------------

def plot_throughput(results, out_path):
    # Group by workload, then backend+axis
    workloads = sorted({r["workload"]["name"] for r in results})
    groups = [(r["backend"]["name"], r.get("fairness_axis", "?")) for r in results]
    labels = sorted(set(f"{b} (Axis {a})" for b, a in groups))

    fig, axes = plt.subplots(1, len(workloads), figsize=(6 * len(workloads), 5), squeeze=False)
    fig.suptitle("Throughput (ops/sec) — higher is better", fontsize=13)

    for col, wl in enumerate(workloads):
        ax = axes[0][col]
        sub = [r for r in results if r["workload"]["name"] == wl]
        names  = [f"{r['backend']['name']} Axis {r.get('fairness_axis','?')}" for r in sub]
        values = [r["metrics"]["ops_per_sec"] for r in sub]

        x = np.arange(len(sub))
        bars = ax.bar(x, values, color=["#4C72B0", "#DD8452", "#55A868", "#C44E52",
                                         "#8172B3", "#937860"][:len(sub)])
        ax.set_title(wl)
        ax.set_ylabel("ops / sec")
        ax.set_xticks(x)
        ax.set_xticklabels(names, rotation=25, ha="right", fontsize=8)
        ax.yaxis.set_major_formatter(
            matplotlib.ticker.FuncFormatter(lambda v, _: f"{v:,.0f}"))
        for bar, val in zip(bars, values):
            ax.text(bar.get_x() + bar.get_width() / 2, bar.get_height() * 1.01,
                    f"{val:,.0f}", ha="center", va="bottom", fontsize=7)

    plt.tight_layout()
    plt.savefig(out_path, dpi=150)
    print(f"Saved: {out_path}")
    plt.close()


# ---------------------------------------------------------------------------
# Latency chart
# ---------------------------------------------------------------------------

def plot_latency(results, out_path):
    workloads = sorted({r["workload"]["name"] for r in results})

    fig, axes = plt.subplots(1, len(workloads), figsize=(6 * len(workloads), 5), squeeze=False)
    fig.suptitle("Write latency (µs) — lower is better", fontsize=13)

    for col, wl in enumerate(workloads):
        ax = axes[0][col]
        sub = [r for r in results if r["workload"]["name"] == wl]
        names   = [f"{r['backend']['name']} Axis {r.get('fairness_axis','?')}" for r in sub]
        p50s    = [r["metrics"]["latency_us"]["p50"]  for r in sub]
        p95s    = [r["metrics"]["latency_us"]["p95"]  for r in sub]
        p99s    = [r["metrics"]["latency_us"]["p99"]  for r in sub]

        x    = np.arange(len(sub))
        w    = 0.25
        ax.bar(x - w,   p50s, w, label="p50",  color="#4C72B0")
        ax.bar(x,       p95s, w, label="p95",  color="#DD8452")
        ax.bar(x + w,   p99s, w, label="p99",  color="#C44E52")

        ax.set_title(wl)
        ax.set_ylabel("latency (µs)")
        ax.set_xticks(x)
        ax.set_xticklabels(names, rotation=25, ha="right", fontsize=8)
        ax.legend(fontsize=8)

    plt.tight_layout()
    plt.savefig(out_path, dpi=150)
    print(f"Saved: {out_path}")
    plt.close()


# ---------------------------------------------------------------------------
# Block-size sweep chart  (throughput vs durability_bytes)
# ---------------------------------------------------------------------------

def plot_block_size_sweep(results, out_path):
    """Plot ops/sec vs durability_bytes for each backend.

    Triggered automatically when results span >= 3 distinct durability_bytes
    values for the sustained_write workload.  Highlights the crossover point
    where nanots write throughput surpasses SQLite.
    """
    sw = [r for r in results if r["workload"]["name"] == "sustained_write"]
    if not sw:
        return

    # Group by backend: {backend_name: [(dur_bytes, ops_per_sec), ...]}
    by_backend = {}
    for r in sw:
        name = r["backend"]["name"]
        try:
            dur = int(r["workload"]["config"]["durability_bytes"])
        except (KeyError, ValueError):
            continue
        ops = r["metrics"]["ops_per_sec"]
        by_backend.setdefault(name, []).append((dur, ops))

    # Need at least one backend with >= 3 distinct block sizes to be useful.
    if not any(len(v) >= 3 for v in by_backend.values()):
        return

    # Sort each backend's points by block size.
    for name in by_backend:
        by_backend[name].sort()

    fig, ax = plt.subplots(figsize=(9, 5))
    ax.set_title("Write throughput vs durability window\n"
                 "(sustained_write, synchronous=full for SQLite)",
                 fontsize=11)
    ax.set_xlabel("Durability window / block size")
    ax.set_ylabel("ops / sec  (higher is better)")
    ax.set_xscale("log", base=2)

    colors = {"nanots": "#4C72B0", "sqlite": "#DD8452"}
    default_colors = ["#55A868", "#C44E52", "#8172B3", "#937860"]
    color_idx = 0

    lines = {}
    for name, points in sorted(by_backend.items()):
        xs = [p[0] for p in points]
        ys = [p[1] for p in points]
        color = colors.get(name) or default_colors[color_idx % len(default_colors)]
        color_idx += 1
        line, = ax.plot(xs, ys, marker="o", linewidth=2, markersize=6,
                        label=name, color=color)
        lines[name] = (xs, ys, color)

    # Annotate crossover between nanots and sqlite if both are present.
    if "nanots" in lines and "sqlite" in lines:
        nxs, nys, _ = lines["nanots"]
        sxs, sys_, _ = lines["sqlite"]
        # Find adjacent pairs where nanots crosses above sqlite.
        # Use the common x-values only.
        common = sorted(set(nxs) & set(sxs))
        n_map = dict(zip(nxs, nys))
        s_map = dict(zip(sxs, sys_))
        for i in range(len(common) - 1):
            x0, x1 = common[i], common[i + 1]
            n0, n1 = n_map[x0], n_map[x1]
            s0, s1 = s_map[x0], s_map[x1]
            # Crossed if sign of (nanots - sqlite) changes.
            if (n0 - s0) * (n1 - s1) < 0:
                # Linear interpolation in log space to find exact crossover.
                import math
                lx0, lx1 = math.log2(x0), math.log2(x1)
                diff0, diff1 = n0 - s0, n1 - s1
                t = diff0 / (diff0 - diff1)
                cross_log = lx0 + t * (lx1 - lx0)
                cross_x = 2 ** cross_log
                cross_y = n0 + t * (n1 - n0)
                ax.axvline(cross_x, color="gray", linestyle="--", linewidth=1)
                ax.annotate(
                    f"crossover\n~{cross_x / 1048576:.1f} MB",
                    xy=(cross_x, cross_y),
                    xytext=(cross_x * 1.4, cross_y * 0.75),
                    fontsize=8,
                    color="gray",
                    arrowprops=dict(arrowstyle="->", color="gray", lw=0.8),
                )
                break

    # X-axis ticks: one per tested block size, labelled in MB.
    all_xs = sorted({p for pts in by_backend.values() for p, _ in pts})
    ax.set_xticks(all_xs)
    ax.set_xticklabels(
        [f"{x / 1048576:.3g} MB" for x in all_xs],
        rotation=35, ha="right", fontsize=8,
    )
    ax.yaxis.set_major_formatter(
        matplotlib.ticker.FuncFormatter(lambda v, _: f"{v:,.0f}"))
    ax.legend(fontsize=9)
    ax.grid(axis="y", linestyle=":", alpha=0.5)

    plt.tight_layout()
    plt.savefig(out_path, dpi=150)
    print(f"Saved: {out_path}")
    plt.close()


# ---------------------------------------------------------------------------
# Reader-scaling chart
# ---------------------------------------------------------------------------

def plot_reader_scaling(results, out_path):
    cr_results = [r for r in results if r["workload"]["name"] == "concurrent_readers"]
    if not cr_results:
        print("No concurrent_readers results found; skipping scaling chart.")
        return

    fig, ax = plt.subplots(figsize=(8, 5))
    ax.set_title("Writer throughput vs reader count\n(concurrent_readers workload)")
    ax.set_xlabel("Number of concurrent reader threads (K)")
    ax.set_ylabel("Writer writes / sec")

    colors = ["#4C72B0", "#DD8452", "#55A868", "#C44E52", "#8172B3"]
    for idx, r in enumerate(cr_results):
        label = f"{r['backend']['name']} Axis {r.get('fairness_axis','?')}"
        scaling_str = r["workload"]["config"].get("reader_scaling", "")
        if not scaling_str:
            continue
        # Parse "k=1: 82345 w/s | k=2: 81902 w/s | ..."
        ks, ops = [], []
        for token in scaling_str.split(" | "):
            try:
                k_part, ops_part = token.split(":")
                k = int(k_part.strip().split("=")[1])
                o = float(ops_part.strip().split(" ")[0])
                ks.append(k)
                ops.append(o)
            except Exception:
                pass
        if ks:
            color = colors[idx % len(colors)]
            ax.plot(ks, ops, marker="o", label=label, color=color)

    ax.legend()
    ax.yaxis.set_major_formatter(
        matplotlib.ticker.FuncFormatter(lambda v, _: f"{v:,.0f}"))
    plt.tight_layout()
    plt.savefig(out_path, dpi=150)
    print(f"Saved: {out_path}")
    plt.close()


# ---------------------------------------------------------------------------
# Writer-scaling chart  (total throughput vs writer count)
# ---------------------------------------------------------------------------

def plot_writer_scaling(results, out_path):
    msw_results = [r for r in results if r["workload"]["name"] == "multi_stream_write"]
    if not msw_results:
        return

    fig, axes = plt.subplots(1, 2, figsize=(13, 5))
    fig.suptitle("Multi-stream write scaling\n(multi_stream_write workload)", fontsize=12)

    colors = {"nanots": "#4C72B0", "sqlite": "#DD8452"}
    default_colors = ["#55A868", "#C44E52", "#8172B3", "#937860"]
    color_idx = 0

    # Left: total throughput — nanots rises, SQLite falls
    ax_total = axes[0]
    ax_total.set_title("Total throughput (all writers)")
    ax_total.set_xlabel("Number of concurrent writer threads (K)")
    ax_total.set_ylabel("Total writes / sec  (higher = better scaling)")

    # Right: per-writer throughput — shows WAL serialization clearly
    ax_per = axes[1]
    ax_per.set_title("Per-writer throughput")
    ax_per.set_xlabel("Number of concurrent writer threads (K)")
    ax_per.set_ylabel("Writes / sec per writer")

    for r in msw_results:
        label = f"{r['backend']['name']} Axis {r.get('fairness_axis','?')}"
        scaling_str = r["workload"]["config"].get("writer_scaling", "")
        if not scaling_str:
            continue
        ks, totals = [], []
        for token in scaling_str.split(" | "):
            try:
                k_part, ops_part = token.split(":")
                k = int(k_part.strip().split("=")[1])
                o = float(ops_part.strip().split(" ")[0])
                ks.append(k)
                totals.append(o)
            except Exception:
                pass
        if not ks:
            continue
        per_writer = [t / k for t, k in zip(totals, ks)]
        color = colors.get(r["backend"]["name"]) or default_colors[color_idx % len(default_colors)]
        color_idx += 1
        ax_total.plot(ks, totals,     marker="o", linewidth=2, label=label, color=color)
        ax_per.plot(  ks, per_writer, marker="o", linewidth=2, label=label, color=color)

    # Ideal linear scaling reference on total chart
    if msw_results:
        ref_k1 = None
        for r in msw_results:
            sc = r["workload"]["config"].get("writer_scaling", "")
            if sc:
                first_tok = sc.split(" | ")[0]
                try:
                    ref_k1 = float(first_tok.split(":")[1].strip().split(" ")[0])
                    break
                except Exception:
                    pass
        if ref_k1:
            ideal_ks = [1, 2, 4, 8]
            ax_total.plot(ideal_ks, [ref_k1 * k for k in ideal_ks],
                          linestyle="--", color="gray", linewidth=1, label="ideal linear")

    for ax in axes:
        ax.set_xticks([1, 2, 4, 8])
        ax.legend(fontsize=8)
        ax.yaxis.set_major_formatter(
            matplotlib.ticker.FuncFormatter(lambda v, _: f"{v:,.0f}"))
        ax.grid(axis="y", linestyle=":", alpha=0.4)

    plt.tight_layout()
    plt.savefig(out_path, dpi=150)
    print(f"Saved: {out_path}")
    plt.close()


# ---------------------------------------------------------------------------
# Write-read contention chart  (writer w/s and reader seeks/s vs K)
# ---------------------------------------------------------------------------

def plot_write_read_contention(results, out_path):
    wrc = [r for r in results if r["workload"]["name"] == "write_read_contention"]
    if not wrc:
        return

    fig, axes = plt.subplots(1, 2, figsize=(13, 5))
    fig.suptitle("Write-read contention scaling\n"
                 "(K writers + K readers simultaneously — nanots vs SQLite)",
                 fontsize=12)

    colors = {"nanots": "#4C72B0", "sqlite": "#DD8452"}
    default_colors = ["#55A868", "#C44E52", "#8172B3", "#937860"]
    color_idx = 0

    ax_w = axes[0]
    ax_w.set_title("Writer throughput under K concurrent readers")
    ax_w.set_xlabel("K  (simultaneous writers AND readers)")
    ax_w.set_ylabel("Total writer writes / sec  (higher is better)")

    ax_r = axes[1]
    ax_r.set_title("Reader throughput under K concurrent writers")
    ax_r.set_xlabel("K  (simultaneous writers AND readers)")
    ax_r.set_ylabel("Total reader seeks / sec  (higher is better)")

    def parse_scaling(s, unit):
        ks, vals = [], []
        for token in s.split(" | "):
            try:
                k_part, v_part = token.split(":")
                k = int(k_part.strip().split("=")[1])
                v = float(v_part.strip().split(" ")[0])
                ks.append(k)
                vals.append(v)
            except Exception:
                pass
        return ks, vals

    for r in wrc:
        label = f"{r['backend']['name']} Axis {r.get('fairness_axis','?')}"
        color = colors.get(r["backend"]["name"]) or default_colors[color_idx % len(default_colors)]
        color_idx += 1

        ws = r["workload"]["config"].get("writer_scaling", "")
        rs = r["workload"]["config"].get("reader_scaling", "")

        wks, wvals = parse_scaling(ws, "w/s")
        rks, rvals = parse_scaling(rs, "seeks/s")

        if wks:
            ax_w.plot(wks, wvals, marker="o", linewidth=2, label=label, color=color)
        if rks:
            ax_r.plot(rks, rvals, marker="o", linewidth=2, label=label, color=color)

    for ax in (ax_w, ax_r):
        ax.set_xticks([1, 2, 4, 8])
        ax.legend(fontsize=8)
        ax.yaxis.set_major_formatter(
            matplotlib.ticker.FuncFormatter(lambda v, _: f"{v:,.0f}"))
        ax.grid(axis="y", linestyle=":", alpha=0.4)

    plt.tight_layout()
    plt.savefig(out_path, dpi=150)
    print(f"Saved: {out_path}")
    plt.close()


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(description="Plot bench JSON results")
    parser.add_argument("files", nargs="+", help="JSON result files")
    parser.add_argument("--out-dir", default=None,
                        help="Output directory (defaults to directory of first input file)")
    args = parser.parse_args()

    results = load_results(args.files)
    if not results:
        print("No valid results found.")
        sys.exit(1)

    out_dir = args.out_dir or str(Path(args.files[0]).parent)
    os.makedirs(out_dir, exist_ok=True)

    # Detect sweep data: all results share one workload with >= 3 distinct
    # durability_bytes values.  The generic throughput/latency bar charts are
    # meaningless for sweep data (block size is the variable, not workload or
    # axis), so skip them and only produce the sweep chart.
    sw_durs = {
        r["workload"]["config"].get("durability_bytes")
        for r in results
        if r["workload"]["name"] == "sustained_write"
    }
    is_sweep = len(sw_durs) >= 3

    if not is_sweep:
        plot_throughput(results,     os.path.join(out_dir, "plot_throughput.png"))
        plot_latency(results,        os.path.join(out_dir, "plot_latency.png"))
    plot_reader_scaling(results,          os.path.join(out_dir, "plot_reader_scaling.png"))
    plot_writer_scaling(results,          os.path.join(out_dir, "plot_writer_scaling.png"))
    plot_write_read_contention(results,   os.path.join(out_dir, "plot_write_read_contention.png"))
    plot_block_size_sweep(results,        os.path.join(out_dir, "plot_sweep.png"))


if __name__ == "__main__":
    main()
