#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <omp.h>
#include "../my_alloc.h"

// Benchmark B: Multi-threaded mallocs and frees
// Usage: ./bench_b_<variant> [num_threads] [num_allocs] [num_iters]

#ifdef USE_LIBC
    #define BENCH_ALLOC(sz)  malloc(sz)
    #define BENCH_FREE(p)    free(p)
#else
    #define BENCH_ALLOC(sz)  my_malloc(sz)
    #define BENCH_FREE(p)    my_free(p)
#endif

static const size_t size_classes[] = {16, 32, 64, 128, 256, 512, 1024};

#define NUM_CLASSES (sizeof(size_classes) / sizeof(size_classes[0]))

int main(int argc, char **argv) {
    int num_threads = 1;          // default threads
    size_t num_allocs = 50000;    // per-thread churn array length
    size_t num_iters = 10;        // churn rounds

    if (argc >= 2) num_threads = atoi(argv[1]);
    if (argc >= 3) num_allocs = strtoull(argv[2], NULL, 10);
    if (argc >= 4) num_iters = strtoull(argv[3], NULL, 10);

    if (num_threads <= 0) {
        fprintf(stderr, "num_threads must be >= 1 (got %d)\n", num_threads);
        return 1;
    }

    omp_set_num_threads(num_threads);

    printf("# Benchmark B: multi-thread churn (mixed sizes), no remote frees\n");
    printf("# num_threads=%d\n", num_threads);
    printf("# num_allocs_per_thread=%zu num_iters=%zu\n", num_allocs, num_iters);

    size_t total_per_thread = 2 * num_allocs * num_iters;
    size_t total_global = total_per_thread * (size_t)num_threads;
    
    printf("# total_allocs_per_thread=%zu\n", total_per_thread);
    printf("# total_allocs_global=%zu\n", total_global);

    #pragma omp parallel
    {
        int tid = omp_get_thread_num();

        void **ptrs = malloc(num_allocs * sizeof(void*));
        if (!ptrs) {
            fprintf(stderr, "thread %d: failed to malloc ptr array\n", tid);
            abort();
        }

        for (size_t it = 0; it < num_iters; ++it) {
            // 1) Allocate a mix of sizes into ptrs[]
            for (size_t i = 0; i < num_allocs; ++i) {
                size_t sz = size_classes[i % NUM_CLASSES];
                void *p = BENCH_ALLOC(sz);
                
                if (!p) {
                    fprintf(stderr, "thread %d: BENCH_ALLOC failed in mixed phase at iter=%zu i=%zu (size=%zu)\n", tid, it, i, sz);
                    free(ptrs);
                    abort();
                }
                
                ptrs[i] = p;
                memset(p, 0, sz);
            }

            // 2) Free every third block to fragment the freelist
            for (size_t i = 0; i < num_allocs; i += 3) {
                BENCH_FREE(ptrs[i]);
                ptrs[i] = NULL;
            }

            // 3) Reuse: allocate & free a bunch of 64B payloads
            for (size_t i = 0; i < num_allocs; ++i) {
                size_t sz = 64;
                void *p = BENCH_ALLOC(sz);
                if (!p) {
                    fprintf(stderr, "thread %d: BENCH_ALLOC failed in transient 64B phase at iter=%zu i=%zu\n", tid, it, i);
                    free(ptrs);
                    abort();
                }
                memset(p, 0, sz);
                BENCH_FREE(p);
            }

            // 4) Free any remaining long-lived blocks
            for (size_t i = 0; i < num_allocs; ++i) {
                if (ptrs[i]) {
                    BENCH_FREE(ptrs[i]);
                    ptrs[i] = NULL;
                }
            }
        }
        free(ptrs);
    }

    return 0;
}