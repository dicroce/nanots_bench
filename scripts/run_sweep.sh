#!/usr/bin/env bash
# run_sweep.sh — Sweep durability_bytes across a log-spaced range for
#                sustained_write to reveal the crossover point where nanots
#                write throughput surpasses SQLite.
#
# Usage:
#   ./scripts/run_sweep.sh [--bench <path>] [--backends <comma-list>]
#                          [--num-frames <N>] [--out-dir <path>]
#
# Outputs one JSON file per (backend, block_size) to --out-dir (default:
# results/sweep/).  Feed the whole directory to plot_results.py to generate
# the crossover chart:
#
#   python3 scripts/plot_results.py results/sweep/*.json

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

BENCH="${BENCH:-${REPO_ROOT}/build/bench}"
OUT_DIR="${REPO_ROOT}/results/sweep"
DATA_DIR="${REPO_ROOT}/bench_data/sweep"
BACKENDS=""
NUM_FRAMES=50000
FRAME_SIZE=4096

while [[ $# -gt 0 ]]; do
    case "$1" in
        --bench)      BENCH="$2";      shift 2 ;;
        --backends)   BACKENDS="$2";   shift 2 ;;
        --num-frames) NUM_FRAMES="$2"; shift 2 ;;
        --out-dir)    OUT_DIR="$2";    shift 2 ;;
        *) echo "Unknown arg: $1"; exit 1 ;;
    esac
done

if [[ ! -x "${BENCH}" ]]; then
    echo "ERROR: bench binary not found at ${BENCH}"
    echo "  Build first: cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j"
    exit 1
fi

# Block sizes to sweep (bytes) — log-spaced from 512 KB to 256 MB.
# 512KB is below the crossover; 256MB is well above it.
BLOCK_SIZES=(524288 1048576 2097152 4194304 8388608 16777216 33554432 67108864 134217728 268435456)

if [[ -z "${BACKENDS}" ]]; then
    BACKENDS=$("${BENCH}" --list-backends | grep -v '^Available' | awk '{print $1}' | tr '\n' ',')
    BACKENDS="${BACKENDS%,}"
fi
IFS=',' read -ra BACKEND_LIST <<< "${BACKENDS}"

mkdir -p "${OUT_DIR}" "${DATA_DIR}"

echo "========================================================"
echo " nanots_bench block-size sweep"
echo " Bench:    ${BENCH}"
echo " Backends: ${BACKENDS}"
echo " Frames:   ${NUM_FRAMES} x ${FRAME_SIZE} B"
echo " Results:  ${OUT_DIR}"
echo "========================================================"
echo
printf "  %-10s  %-10s  %s\n" "BLOCK_SIZE" "BACKEND" "OPS/SEC"
printf "  %-10s  %-10s  %s\n" "----------" "-------" "-------"

FAILED=0
for DUR in "${BLOCK_SIZES[@]}"; do
    MB=$(python3 -c "import sys; b=int(sys.argv[1]); print(f'{b/1048576:.3g} MB')" "${DUR}")
    for BACKEND in "${BACKEND_LIST[@]}"; do
        DB_DIR="${DATA_DIR}/${BACKEND}_${DUR}"
        OUTFILE="${OUT_DIR}/${BACKEND}_sweep_${DUR}.json"
        rm -rf "${DB_DIR}"

        # SQLite uses synchronous=full throughout the sweep so the durability
        # contract is honoured at every block size (same as run_all.sh axis A).
        set +e
        "${BENCH}" \
            --backend            "${BACKEND}" \
            --workload           sustained_write \
            --num-frames         "${NUM_FRAMES}" \
            --frame-size         "${FRAME_SIZE}" \
            --durability-bytes   "${DUR}" \
            --sqlite-synchronous full \
            --data-dir           "${DB_DIR}" \
            --output             "${OUTFILE}" \
            2>/dev/null
        RC=$?
        set -e

        if [[ $RC -eq 0 && -f "${OUTFILE}" ]]; then
            # Read via stdin so bash (not Python) resolves the MSYS2 path.
            OPS=$(python3 -c "import sys,json; print(f\"{json.load(sys.stdin)['metrics']['ops_per_sec']:,.0f}\")" < "${OUTFILE}")
            printf "  %-10s  %-10s  %s\n" "${MB}" "${BACKEND}" "${OPS}"
        else
            printf "  %-10s  %-10s  FAILED\n" "${MB}" "${BACKEND}"
            ((FAILED++)) || true
        fi
    done
done

echo
echo "========================================================"
[[ $FAILED -eq 0 ]] && echo " Sweep complete. Plot with:" || echo " Sweep finished with ${FAILED} failure(s). Plot with:"
echo "   python3 ${SCRIPT_DIR}/plot_results.py ${OUT_DIR}/*.json"
echo "========================================================"
exit $FAILED
