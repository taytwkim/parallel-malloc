#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <omp.h>
#include "../my_alloc.h"

// Benchmark C: Producer-Consumer Cross-Thread Frees
// Usage: ./bench_c_<variant> [num_consumers] [num_allocs] [num_iters]

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
    int num_consumers = 1;        // default: 1 producer + 1 consumer
    size_t num_allocs = 50000;   // per iteration
    size_t num_iters = 10;

    if (argc >= 2) num_consumers = atoi(argv[1]);
    if (argc >= 3) num_allocs    = strtoull(argv[2], NULL, 10);
    if (argc >= 4) num_iters     = strtoull(argv[3], NULL, 10);

    if (num_consumers < 1) {
        fprintf(stderr, "num_consumers must be >= 1 (got %d)\n", num_consumers);
        return 1;
    }

    int num_threads = num_consumers + 1;  // 1 producer + N consumers
    omp_set_num_threads(num_threads);

    printf("# Benchmark C: 1 producer + %d consumers, remote frees (mixed sizes)\n", num_consumers);
    printf("# num_threads=%d (producer=0, consumers=1..%d)\n", num_threads, num_threads - 1);
    printf("# num_allocs=%zu num_iters=%zu\n", num_allocs, num_iters);
    printf("# size_classes={16,32,64,128,256,512,1024}\n");
    printf("# total_allocs=%zu\n", num_allocs * num_iters); // all by producer

    // Shared array of pointers for this iteration
    void **ptrs = malloc(num_allocs * sizeof(void*));
    
    if (!ptrs) {
        fprintf(stderr, "failed to malloc ptrs array (num_allocs=%zu)\n", num_allocs);
        return 1;
    }

    #pragma omp parallel
    {
        int tid = omp_get_thread_num();
        int threads = omp_get_num_threads();
        int consumers = threads - 1;   // should equal num_consumers

        for (size_t it = 0; it < num_iters; ++it) {
            // Producer: allocate all blocks
            if (tid == 0) {
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

            // Consumers: free the producer's blocks (remote frees)
            if (tid > 0) {
                int cid = tid - 1;  // consumer index: 0..consumers-1

                for (size_t i = cid; i < num_allocs; i += consumers) {
                    BENCH_FREE(ptrs[i]);
                }
            }

            // Wait until all frees are done before next iteration
            #pragma omp barrier
        }
    }

    free(ptrs);
    return 0;
}