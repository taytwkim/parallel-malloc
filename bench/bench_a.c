#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "../my_alloc.h"

// Benchmark A: Sequential mallocs and frees
// Usage: ./bench_a_<variant> [num_allocs] [num_iters]

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
    size_t num_allocs = 50000;
    size_t num_iters = 10;

    if (argc >= 2) num_allocs = strtoull(argv[1], NULL, 10);
    if (argc >= 3) num_iters = strtoull(argv[2], NULL, 10);

    printf("# Benchmark A: single-thread churn (mixed sizes)\n");
    printf("# num_allocs=%zu num_iters=%zu\n", num_allocs, num_iters);

    void **ptrs = malloc(num_allocs * sizeof(void*));
    
    if (!ptrs) {
        fprintf(stderr, "failed to malloc ptr array\n");
        return 1;
    }

    size_t total_allocs = 2 * num_allocs * num_iters;
    printf("# total_allocs=%zu\n", total_allocs);

    for (size_t it = 0; it < num_iters; ++it) {
        // 1) Allocate a mix of sizes into ptrs[]
        for (size_t i = 0; i < num_allocs; ++i) {
            size_t sz = size_classes[i % NUM_CLASSES];   // mixed sizes
            void *p = BENCH_ALLOC(sz);
            
            if (!p) {
                fprintf(stderr, "BENCH_ALLOC failed in mixed phase at iter=%zu i=%zu (size=%zu)\n", it, i, sz);
                free(ptrs);
                return 1;
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
                fprintf(stderr, "BENCH_ALLOC failed in transient 64B phase at iter=%zu i=%zu\n", it, i);
                free(ptrs);
                return 1;
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
    return 0;
}
