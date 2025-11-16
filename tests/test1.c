// test/test1.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <omp.h>
#include "../my_alloc.h"

int main(void) {
    const int nthreads = 4;
    const size_t iters = 10000;     // iterations per thread

    printf("test1: multithreaded alloc/free test\n");
    printf("  threads = %d, iters per thread = %zu\n", nthreads, iters);

    int errors = 0;

    #pragma omp parallel num_threads(nthreads) reduction(+:errors)
    {
        int tid = omp_get_thread_num();

        for (size_t i = 0; i < iters; i++) {
            size_t sz = 16 + ((i + tid) % 256);   // vary size a bit
            
            unsigned char *p = (unsigned char*)my_malloc(sz);
            
            if (!p) {
                printf("Thread %d: my_malloc returned NULL at iter %zu\n", tid, i);
                errors++;
                break;  // this thread stops; others may continue
            }

            // Fill with a thread-specific pattern
            unsigned char pattern = (unsigned char)(tid + 1);
            memset(p, pattern, sz);

            // Verify the pattern (basic corruption check)
            for (size_t j = 0; j < sz; j++) {
                if (p[j] != pattern) {
                    printf("Thread %d: data corrupted at iter %zu, offset %zu\n",
                           tid, i, j);
                    errors++;
                    break;
                }
            }

            my_free(p);

            // If this thread saw an error, no need to keep going
            if (errors > 0) {
                break;
            }
        }
    }

    if (errors > 0) {
        printf("test1: FAILED (errors = %d)\n", errors);
        return 1;
    } 
    else {
        printf("test1: PASSED ✅\n");
        return 0;
    }
}
