#!/usr/bin/env bash
set -euo pipefail
exec 2>&1

ROOT=".."
VARIANTS=(libc v0 v1)

NUM_THREADS_LIST=(1 2 4 8 16)
NUM_ALLOCS=100000
NUM_ITERS=10

echo "BenchmarkB multi-thread churn mixed-sizes no-remote-frees"
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
