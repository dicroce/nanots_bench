#!/usr/bin/env bash
# run_all.sh — Run all workloads against all registered backends under both
#              fairness axes and save results to ../results/.
#
# Usage:
#   ./scripts/run_all.sh [--bench <path>] [--backends <comma-list>]
#
# The script runs:
#   Axis A (matched durability):   --durability-bytes 1048576  --sqlite-synchronous full
#   Axis B (best-reasonable):      --durability-bytes 10485760 --sqlite-synchronous normal
#
# Why synchronous=full for axis A?
#   nanots calls FlushFileBuffers (Windows) / msync(MS_SYNC) (Linux) at every
#   block boundary — a synchronous flush to durable storage.  SQLite with
#   synchronous=normal does NOT fsync on every WAL commit; it defers fsyncs to
#   checkpoint time.  To give both systems the same durability contract at axis A
#   (at-most ~1 MB lost on crash), SQLite must use synchronous=full so that each
#   1 MB transaction commit is accompanied by an fsync.
#
# Why only these two axes?  See README.md § "Methodology: Fairness".

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

# ---- Defaults ---------------------------------------------------------------
BENCH="${BENCH:-${REPO_ROOT}/build/bench}"
RESULTS_DIR="${REPO_ROOT}/results"
DATA_DIR="${REPO_ROOT}/bench_data"
BACKENDS=""          # empty = all registered backends
NUM_FRAMES=100000
FRAME_SIZE=4096

# ---- Argument parsing -------------------------------------------------------
while [[ $# -gt 0 ]]; do
    case "$1" in
        --bench)      BENCH="$2";      shift 2 ;;
        --backends)   BACKENDS="$2";   shift 2 ;;
        --num-frames) NUM_FRAMES="$2"; shift 2 ;;
        --frame-size) FRAME_SIZE="$2"; shift 2 ;;
        *) echo "Unknown arg: $1"; exit 1 ;;
    esac
done

# ---- Sanity checks ----------------------------------------------------------
if [[ ! -x "${BENCH}" ]]; then
    echo "ERROR: bench binary not found at ${BENCH}"
    echo "  Build first:  cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j"
    exit 1
fi

mkdir -p "${RESULTS_DIR}" "${DATA_DIR}"

# ---- Backend list -----------------------------------------------------------
if [[ -z "${BACKENDS}" ]]; then
    BACKENDS=$("${BENCH}" --list-backends | grep -v '^Available' | awk '{print $1}' | tr '\n' ',')
    BACKENDS="${BACKENDS%,}"   # strip trailing comma
fi
IFS=',' read -ra BACKEND_LIST <<< "${BACKENDS}"

# ---- Workload list ----------------------------------------------------------
# multi_stream_write and write_read_contention require --nanots-auto-reclaim
# because they write continuously and exhaust the pre-allocated block pool.
WORKLOADS=(sustained_write random_seek concurrent_readers multi_stream_write write_read_contention)
declare -A NEEDS_AUTO_RECLAIM=(
    ["concurrent_readers"]="1"
    ["multi_stream_write"]="1"
    ["write_read_contention"]="1"
)

# ---- Fairness axes ----------------------------------------------------------
# Axis A: matched durability — both systems lose at most ~1 MB on crash.
# Axis B: best-reasonable   — tuned for realistic video/IoT deployment.
declare -A AXIS_DUR=(
    ["A"]=1048576
    ["B"]=10485760
)
declare -A AXIS_SYNC=(
    ["A"]="full"
    ["B"]="normal"
)

# ---- Run matrix -------------------------------------------------------------
echo "========================================================"
echo " nanots_bench run_all"
echo " Bench:    ${BENCH}"
echo " Backends: ${BACKENDS}"
echo " Results:  ${RESULTS_DIR}"
echo "========================================================"
echo

FAILED=0
SKIPPED=0
PASSED=0

for BACKEND in "${BACKEND_LIST[@]}"; do
    for WORKLOAD in "${WORKLOADS[@]}"; do
        for AXIS in A B; do
            DUR="${AXIS_DUR[$AXIS]}"
            SYNC="${AXIS_SYNC[$AXIS]}"
            OUTFILE="${RESULTS_DIR}/${BACKEND}_${WORKLOAD}_axis${AXIS}.json"
            DB_DIR="${DATA_DIR}/${BACKEND}_${WORKLOAD}_axis${AXIS}"

            echo -n "  [Axis ${AXIS}] ${BACKEND} / ${WORKLOAD} ... "

            # Clean up any previous run's data directory so results are fresh.
            rm -rf "${DB_DIR}"
            mkdir -p "${DB_DIR}"

            # Build optional flags
            EXTRA_FLAGS=""
            if [[ -n "${NEEDS_AUTO_RECLAIM[$WORKLOAD]+x}" ]]; then
                EXTRA_FLAGS="--nanots-auto-reclaim"
            fi

            set +e
            "${BENCH}" \
                --backend            "${BACKEND}" \
                --workload           "${WORKLOAD}" \
                --num-frames         "${NUM_FRAMES}" \
                --frame-size         "${FRAME_SIZE}" \
                --durability-bytes   "${DUR}" \
                --sqlite-synchronous "${SYNC}" \
                --fairness-axis      "${AXIS}" \
                --data-dir           "${DB_DIR}" \
                --output             "${OUTFILE}" \
                ${EXTRA_FLAGS} \
                2>&1
            RC=$?
            set -e

            if [[ $RC -eq 0 && -f "${OUTFILE}" ]]; then
                # Check if the JSON contains an error field
                if grep -q '"error"' "${OUTFILE}" 2>/dev/null; then
                    echo "SKIPPED (backend not implemented)"
                    ((SKIPPED++)) || true
                else
                    echo "OK -> $(basename "${OUTFILE}")"
                    ((PASSED++)) || true
                fi
            else
                echo "FAILED (exit ${RC})"
                ((FAILED++)) || true
            fi
        done
    done
done

echo
echo "========================================================"
echo " Results: ${PASSED} passed, ${SKIPPED} skipped (stub backends), ${FAILED} failed"
echo "========================================================"

# Emit a quick summary table of writer throughput if jq is available.
if command -v jq &>/dev/null; then
    echo
    echo "Quick throughput summary (ops/sec):"
    printf "  %-12s  %-20s  %-8s  %s\n" "BACKEND" "WORKLOAD" "AXIS" "OPS/SEC"
    for f in "${RESULTS_DIR}"/*.json; do
        [[ -f "$f" ]] || continue
        jq -r '
            if .error then empty
            else [.backend.name, .workload.name, .fairness_axis,
                  (.metrics.ops_per_sec | tostring)] | @tsv
            end
        ' "$f" 2>/dev/null | \
        awk '{printf "  %-12s  %-20s  %-8s  %s\n", $1, $2, $3, $4}'
    done
fi

exit $FAILED
