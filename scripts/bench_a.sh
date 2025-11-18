#!/usr/bin/env bash
set -euo pipefail
exec 2>&1

ROOT=".."
VARIANTS=(libc v0 v1)

# ===== Hoard paths =====
HOARD_LIB_LINUX="/path/to/libhoard.so"

# macOS
# HOARD_LIB_MAC="/Users/taykim/Desktop/Hoard/build/libhoard.dylib"

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
        # 2 * NUM_ALLOCS * iters because each iteration:
        #   - allocate N mixed-size blocks
        #   - free every 3rd → allocate/free more churn blocks
        total_allocs=$((2 * NUM_ALLOCS * iters))
        echo "run num_iters=${iters} total_allocs=${total_allocs}"
        time "$exe" "${NUM_ALLOCS}" "${iters}"
        echo
    done
    echo
done

# ===== Hoard on Linux =====
if [[ -f "${HOARD_LIB_LINUX}" ]]; then
    exe="${ROOT}/bench_a_libc"
    if [[ -x "$exe" ]]; then
        echo "variant=hoard LD_PRELOAD=${HOARD_LIB_LINUX}"
        for iters in "${ITERS_LIST[@]}"; do
            total_allocs=$((2 * NUM_ALLOCS * iters))
            echo "run num_iters=${iters} total_allocs=${total_allocs}"
            LD_PRELOAD="${HOARD_LIB_LINUX}" \
            time "$exe" "${NUM_ALLOCS}" "${iters}"
            echo
        done
        echo
    else
        echo "WARNING bench_a_libc not found/executable; skipping hoard runs"
    fi
else
    echo "WARNING HOARD_LIB_LINUX=${HOARD_LIB_LINUX} not found; skipping hoard runs"
fi

# ===== Hoard on Mac =====
# if [[ -f "${HOARD_LIB_MAC}" ]]; then
#     exe="${ROOT}/bench_a_libc"
#     if [[ -x "$exe" ]]; then
#         echo "variant=hoard DYLD_INSERT_LIBRARIES=${HOARD_LIB_MAC}"
#         for iters in "${ITERS_LIST[@]}"; do
#             total_allocs=$((2 * NUM_ALLOCS * iters))
#             echo "run num_iters=${iters} total_allocs=${total_allocs}"
#             DYLD_INSERT_LIBRARIES="${HOARD_LIB_MAC}" \
#             DYLD_FORCE_FLAT_NAMESPACE=1 \
#             time "$exe" "${NUM_ALLOCS}" "${iters}"
#             echo
#         done
#         echo
#     else
#         echo "WARNING bench_a_libc not found/executable; skipping hoard runs"
#     fi
# else
#     echo "WARNING HOARD_LIB_MAC=${HOARD_LIB_MAC} not found; skipping hoard runs"
# fi
