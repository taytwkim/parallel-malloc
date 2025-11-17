#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <omp.h>
#include "../my_alloc.h"

// Benchmark C: Producer-Consumer Cross-Thread Frees
// Usage: ./bench_c_<variant> [num_allocs] [num_iters]

#ifdef USE_LIBC
    #define BENCH_ALLOC(sz)  malloc(sz)
    #define BENCH_FREE(p)    free(p)
#else
    #define BENCH_ALLOC(sz)  my_malloc(sz)
    #define BENCH_FREE(p)    my_free(p)
#endif

// Mixed size classes
static const size_t size_classes[] = {16, 32, 64, 128, 256, 512, 1024};
#define NUM_CLASSES (sizeof(size_classes) / sizeof(size_classes[0]))

int main(int argc, char **argv) {
    size_t num_allocs = 100000;  // per iteration
    size_t num_iters = 10;       // rounds

    if (argc >= 2) num_allocs = strtoull(argv[1], NULL, 10);
    if (argc >= 3) num_iters  = strtoull(argv[2], NULL, 10);

    omp_set_num_threads(2);      // 2 threads: producer + consumer

    printf("# Benchmark C: 2-thread producer/consumer with remote frees (mixed sizes)\n");
    printf("# num_threads_fixed=2\n");
    printf("# num_allocs=%zu num_iters=%zu\n", num_allocs, num_iters);
    printf("# size_classes={16,32,64,128,256,512,1024}\n");
    printf("# total_allocs=%zu\n", num_allocs * num_iters);

    // One shared array of pointers for this iteration
    void **ptrs = malloc(num_allocs * sizeof(void*));
    
    if (!ptrs) {
        fprintf(stderr, "failed to malloc ptrs array (num_allocs=%zu)\n", num_allocs);
        return 1;
    }

    #pragma omp parallel
    {
        int tid = omp_get_thread_num(); // 0 = producer, 1 = consumer

        for (size_t it = 0; it < num_iters; ++it) {
            if (tid == 0) {
                // Producer: allocate
                for (size_t i = 0; i < num_allocs; ++i) {
                    size_t sz = size_classes[i % NUM_CLASSES];

                    void *p = BENCH_ALLOC(sz);
                    
                    if (!p) {
                        fprintf(stderr, "producer: BENCH_ALLOC failed at iter=%zu i=%zu (size=%zu)\n", it, i, sz);
                        abort();
                    }

                    ptrs[i] = p;
                    memset(p, 0, sz); // touch memory
                }
            }
            // Wait until producer has filled ptrs[]
            #pragma omp barrier

            if (tid == 1) {
                // Consumer: free what producer allocated (remote frees)
                for (size_t i = 0; i < num_allocs; ++i) {
                    BENCH_FREE(ptrs[i]);
                }
            }
            // Wait until consumer has freed before next iteration
            #pragma omp barrier
        }
    }
    free(ptrs);
    return 0;
}