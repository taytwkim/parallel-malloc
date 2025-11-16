#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <omp.h>
#include "../my_alloc.h"

#ifdef USE_LIBC
    #define BENCH_ALLOC(sz)  malloc(sz)
    #define BENCH_FREE(p)    free(p)
#else
    #define BENCH_ALLOC(sz)  my_malloc(sz)
    #define BENCH_FREE(p)    my_free(p)
#endif

// Size classes for mixed pattern
static const size_t size_classes[] = {16, 32, 64, 128, 256, 512, 1024};
#define NUM_CLASSES (sizeof(size_classes) / sizeof(size_classes[0]))

int main(int argc, char **argv) {
    int num_threads = 1;          // default threads
    size_t num_allocs = 100000;   // allocations per thread per iteration
    size_t alloc_size = 64;       // used in uniform mode
    size_t num_iters = 10;        // iterations
    int pattern = 1;              // 0 = uniform, 1 = mixed (default for B)

    // Args: num_threads num_allocs alloc_size num_iters pattern
    if (argc >= 2) num_threads = atoi(argv[1]);
    if (argc >= 3) num_allocs = strtoull(argv[2], NULL, 10);
    if (argc >= 4) alloc_size = strtoull(argv[3], NULL, 10);
    if (argc >= 5) num_iters = strtoull(argv[4], NULL, 10);
    if (argc >= 6) pattern = atoi(argv[5]);

    if (num_threads <= 0) {
        fprintf(stderr, "num_threads must be >= 1 (got %d)\n", num_threads);
        return 1;
    }

    // Tell OpenMP how many threads we want
    omp_set_num_threads(num_threads);

    // Verify how many threads we actually got
    int actual_threads = 0;
    #pragma omp parallel
    {
        #pragma omp master
        {
            actual_threads = omp_get_num_threads();
        }
    }

    printf("# Benchmark B: multi-thread alloc/free, no remote frees\n");
    printf("# num_threads_requested=%d\n", num_threads);
    printf("# num_threads_actual=%d\n", actual_threads);
    printf("# num_allocs_per_thread=%zu alloc_size=%zu num_iters=%zu pattern=%d\n", num_allocs, alloc_size, num_iters, pattern);

    size_t total_per_thread = num_allocs * num_iters;
    size_t total_global = total_per_thread * (size_t)actual_threads;

    printf("# total_allocs_per_thread=%zu\n", total_per_thread);
    printf("# total_allocs_global=%zu\n", total_global);

    // Parallel region: each thread allocs/frees its own blocks.
    #pragma omp parallel
    {
        int tid = omp_get_thread_num();

        void **ptrs = malloc(num_allocs * sizeof(void*));
        
        if (!ptrs) {
            fprintf(stderr, "thread %d: failed to malloc ptr array\n", tid);
            abort();
        }

        for (size_t it = 0; it < num_iters; ++it) {
            // allocate num_allocs blocks
            for (size_t i = 0; i < num_allocs; ++i) {
                size_t sz;
                if (pattern == 1) {
                    // Mixed: cycle through size classes
                    sz = size_classes[i % NUM_CLASSES];
                } 
                else {
                    // Uniform
                    sz = alloc_size;
                }

                void *p = BENCH_ALLOC(sz);
                if (!p) {
                    fprintf(stderr, "thread %d: BENCH_ALLOC failed at iter=%zu i=%zu (size=%zu)\n", tid, it, i, sz);
                    abort();
                }

                ptrs[i] = p;
                // Touch memory so pages are actually allocated.
                memset(p, 0, sz);
            }

            // free them (same thread → no remote frees)
            for (size_t i = 0; i < num_allocs; ++i) {
                BENCH_FREE(ptrs[i]);
            }
        }

        free(ptrs);
    }

    return 0;
}