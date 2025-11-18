#!/usr/bin/env bash
set -euo pipefail
exec 2>&1

ROOT=".."
VARIANTS=(libc v0 v1)

# ----- Hoard paths -----
# Linux
# HOARD_LIB_LINUX="/path/to/libhoard.so"

# macOS
HOARD_LIB_MAC="/Users/taykim/Desktop/Hoard/build/libhoard.dylib"

NUM_THREADS_LIST=(1 2 4 8 16)
NUM_ALLOCS=50000
NUM_ITERS=10

echo "BenchmarkB_1 multi-thread churn mixed-sizes no-remote-frees"
echo "num_allocs_per_thread=${NUM_ALLOCS} num_iters=${NUM_ITERS}"
echo

for variant in "${VARIANTS[@]}"; do
    exe="${ROOT}/bench_b_${variant}"
    if [[ ! -x "$exe" ]]; then
        echo "WARNING missing executable ${exe}"
        continue
    fi

    echo "variant=${variant}"
    for t in "${NUM_THREADS_LIST[@]}"; do
        per_thread=$((2 * NUM_ALLOCS * NUM_ITERS))
        total=$((t * per_thread))
        echo "run threads=${t} total_allocs_per_thread=${per_thread} total_allocs_global=${total}"
        time "$exe" "${t}" "${NUM_ALLOCS}" "${NUM_ITERS}"
        echo
    done
    echo
done

: '
# ===== Hoard on Linux =====
if [[ -f "${HOARD_LIB_LINUX}" ]]; then
    exe="${ROOT}/bench_b_libc"
    if [[ -x "$exe" ]]; then
        echo "variant=hoard LD_PRELOAD=${HOARD_LIB_LINUX}"
        for t in "${NUM_THREADS_LIST[@]}"; do
            per_thread=$((2 * NUM_ALLOCS * NUM_ITERS))
            total=$((t * per_thread))
            echo "run threads=${t} total_allocs_per_thread=${per_thread} total_allocs_global=${total}"
            LD_PRELOAD="${HOARD_LIB_LINUX}" \
            time "$exe" "${t}" "${NUM_ALLOCS}" "${NUM_ITERS}"
            echo
        done
        echo
    else
        echo "WARNING bench_b_libc not found/executable; skipping hoard runs"
    fi
else
    echo "WARNING HOARD_LIB_LINUX=${HOARD_LIB_LINUX} not found; skipping hoard runs"
fi
'

# ===== Hoard on Mac =====
if [[ -f "${HOARD_LIB_MAC}" ]]; then
    exe="${ROOT}/bench_b_libc"
    if [[ -x "$exe" ]]; then
        echo "variant=hoard DYLD_INSERT_LIBRARIES=${HOARD_LIB_MAC}"
        for t in "${NUM_THREADS_LIST[@]}"; do
            per_thread=$((2 * NUM_ALLOCS * NUM_ITERS))
            total=$((t * per_thread))
            echo "run threads=${t} total_allocs_per_thread=${per_thread} total_allocs_global=${total}"
            DYLD_INSERT_LIBRARIES="${HOARD_LIB_MAC}" \
            DYLD_FORCE_FLAT_NAMESPACE=1 \
            time "$exe" "${t}" "${NUM_ALLOCS}" "${NUM_ITERS}"
            echo
        done
        echo
    else
        echo "WARNING bench_b_libc not found/executable; skipping hoard runs"
    fi
else
    echo "WARNING HOARD_LIB_MAC=${HOARD_LIB_MAC} not found; skipping hoard runs"
fi
