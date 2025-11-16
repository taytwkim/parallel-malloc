#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "../my_alloc.h"

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
    // Default parameters
    size_t num_allocs = 100000;   // how many blocks per iteration
    size_t alloc_size = 64;       // used only in uniform mode
    size_t num_iters = 50;        // how many alloc/free rounds
    int pattern = 1;              // 0 = uniform size classes, 1 = mixed size classes

    if (argc >= 2) num_allocs = strtoull(argv[1], NULL, 10);
    if (argc >= 3) alloc_size = strtoull(argv[2], NULL, 10);
    if (argc >= 4) num_iters  = strtoull(argv[3], NULL, 10);
    if (argc >= 5) pattern    = atoi(argv[4]);

    printf("# Benchmark A: single-thread alloc/free\n");
    printf("# num_allocs=%zu alloc_size=%zu num_iters=%zu pattern=%d\n", num_allocs, alloc_size, num_iters, pattern);

    void **ptrs = malloc(num_allocs * sizeof(void*));   // Array to store pointers so that we can free them later

    if (!ptrs) {
        fprintf(stderr, "failed to malloc ptr array\n");
        return 1;
    }

    size_t total_allocs = num_allocs * num_iters;
    printf("# total_allocs=%zu\n", total_allocs);

    for (size_t it = 0; it < num_iters; ++it) {
        // allocate num_allocs blocks
        for (size_t i = 0; i < num_allocs; ++i) {
            size_t sz;
            if (pattern == 1) {
                sz = size_classes[i % NUM_CLASSES];   // Mixed: cycle through size classes
            } 
            else {
                sz = alloc_size;    // Uniform
            }

            void *p = BENCH_ALLOC(sz);
            if (!p) {
                fprintf(stderr, "BENCH_ALLOC failed at iter=%zu i=%zu (size=%zu)\n", it, i, sz);
                free(ptrs);
                return 1;
            }
            ptrs[i] = p;        // Remember pointer so we can free later

            memset(p, 0, sz);   // Touch memory so pages are actually allocated.
        }

        // free them
        for (size_t i = 0; i < num_allocs; ++i) {
            BENCH_FREE(ptrs[i]);
        }
    }

    free(ptrs);
    return 0;
}
