#!/usr/bin/env bash
set -euo pipefail
exec 2>&1

ROOT=".."
VARIANTS=(libc v0 v1)

NUM_ALLOCS=100000
ITERS_LIST=(1 5 10 50)

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
