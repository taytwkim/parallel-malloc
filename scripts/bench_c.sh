#!/usr/bin/env bash
set -euo pipefail
exec 2>&1

ROOT=".."
VARIANTS=(libc v0)

HOARD_LIB="/Users/taykim/Desktop/Hoard/build/libhoard.dylib"

# 1 producer + N consumers (we vary N)
NUM_CONSUMERS_LIST=(1 2 4 8)

NUM_ALLOCS=100000   # per iteration (all done by producer)
NUM_ITERS=10        # iterations

echo "BenchmarkC 1-producer + N-consumers remote-frees mixed-sizes"
echo "num_allocs=${NUM_ALLOCS} num_iters=${NUM_ITERS}"
echo

for variant in "${VARIANTS[@]}"; do
    exe="${ROOT}/bench_c_${variant}"
    if [[ ! -x "$exe" ]]; then
        echo "WARNING missing executable ${exe}"
        continue
    fi

    echo "variant=${variant}"
    for c in "${NUM_CONSUMERS_LIST[@]}"; do
        threads=$((c + 1))                # 1 producer + c consumers
        total_allocs=$((NUM_ALLOCS * NUM_ITERS))
        echo "run consumers=${c} threads=${threads} total_allocs=${total_allocs}"
        time "$exe" "${c}" "${NUM_ALLOCS}" "${NUM_ITERS}"
        echo
    done
    echo
done

# Hoard runs using the libc binary with DYLD_INSERT_LIBRARIES (macOS)
if [[ -f "${HOARD_LIB}" ]]; then
    exe="${ROOT}/bench_c_libc"
    if [[ -x "$exe" ]]; then
        echo "variant=hoard DYLD_INSERT_LIBRARIES=${HOARD_LIB}"
        for c in "${NUM_CONSUMERS_LIST[@]}"; do
            threads=$((c + 1))
            total_allocs=$((NUM_ALLOCS * NUM_ITERS))
            echo "run consumers=${c} threads=${threads} total_allocs=${total_allocs}"
            DYLD_INSERT_LIBRARIES="${HOARD_LIB}" \
            DYLD_FORCE_FLAT_NAMESPACE=1 \
            time "$exe" "${c}" "${NUM_ALLOCS}" "${NUM_ITERS}"
            echo
        done
        echo
    else
        echo "WARNING bench_c_libc not found/executable; skipping hoard runs"
    fi
else
    echo "WARNING HOARD_LIB=${HOARD_LIB} not found; skipping hoard runs"
fi
