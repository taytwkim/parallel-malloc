#!/usr/bin/env bash
set -euo pipefail
exec 2>&1

ROOT=".."
VARIANTS=(libc v0 v1)

# Hoard on macOS: update this path to your actual libhoard.dylib
HOARD_LIB="/Users/taykim/Desktop/Hoard/build/libhoard.dylib"

# Churn parameters
NUM_ALLOCS=50000           # per iteration
ITERS_LIST=(1 10 100)      # gives 100k, 1M, 10M total allocs

echo "BenchmarkA single-thread churn mixed-sizes"
echo "num_allocs=${NUM_ALLOCS}"
echo

for variant in "${VARIANTS[@]}"; do
    exe="${ROOT}/bench_a_${variant}"
    if [[ ! -x "$exe" ]]; then
        echo "WARNING missing executable ${exe}"
        continue
    fi

    echo "variant=${variant}"
    for iters in "${ITERS_LIST[@]}"; do
        total_allocs=$((2 * NUM_ALLOCS * iters))
        echo "run num_iters=${iters} total_allocs=${total_allocs}"
        time "$exe" "${NUM_ALLOCS}" "${iters}"
        echo
    done
    echo
done

# Hoard run using the libc binary with DYLD_INSERT_LIBRARIES (macOS)
if [[ -f "${HOARD_LIB}" ]]; then
    exe="${ROOT}/bench_a_libc"
    if [[ -x "$exe" ]]; then
        echo "variant=hoard DYLD_INSERT_LIBRARIES=${HOARD_LIB}"
        for iters in "${ITERS_LIST[@]}"; do
            total_allocs=$((2 * NUM_ALLOCS * iters))
            echo "run num_iters=${iters} total_allocs=${total_allocs}"
            DYLD_INSERT_LIBRARIES="${HOARD_LIB}" \
            DYLD_FORCE_FLAT_NAMESPACE=1 \
            time "$exe" "${NUM_ALLOCS}" "${iters}"
            echo
        done
        echo
    else
        echo "WARNING bench_a_libc not found/executable; skipping hoard runs"
    fi
else
    echo "WARNING HOARD_LIB=${HOARD_LIB} not found; skipping hoard runs"
fi
