#!/usr/bin/env bash
set -euo pipefail
exec 2>&1

ROOT=".."
VARIANTS=(libc v0)   # add hoard later via LD_PRELOAD on libc if you want

NUM_ITERS=10
NUM_ALLOCS_LIST=(100000 500000 1000000)

echo "BenchmarkC producer-consumer remote-frees mixed-sizes"
echo "num_iters=${NUM_ITERS}"
echo

for variant in "${VARIANTS[@]}"; do
    exe="${ROOT}/bench_c_${variant}"
    if [[ ! -x "$exe" ]]; then
        echo "WARNING missing executable ${exe}"
        continue
    fi

    echo "variant=${variant}"
    for n in "${NUM_ALLOCS_LIST[@]}"; do
        total=$((n * NUM_ITERS))
        echo "run num_allocs=${n} total_allocs=${total}"
        time "$exe" "${n}" "${NUM_ITERS}"
        echo
    done
    echo
done
