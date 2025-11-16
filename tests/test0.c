#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "../my_alloc.h"

static int aligned16(void *p){ 
    return ((uintptr_t)p & 15u) == 0; 
}

static void test_alignment(void){
    void *a = my_malloc(1);
    void *b = my_malloc(17);
    void *c = my_malloc(4096);

    assert(a && b && c);
    assert(aligned16(a));
    assert(aligned16(b));
    assert(aligned16(c));

    my_free(a);
    my_free(b);
    my_free(c);
}

static void test_coalesce_reuse(void){
    // Sizes chosen so A need=48, B need=56 (64-bit), A+B=104

    void *a = my_malloc(24);   // need = align16(8 + align16(24=32)) = 48
    void *b = my_malloc(40);   // need = align16(8 + align16(40=48)) = 56
    void *c = my_malloc(64);   // keep something after B to avoid touching top
    assert(a && b && c);

    // Free middle then left → should coalesce into one 104-byte free block at A’s spot
    my_free(b);
    my_free(a);

    // Request 56 bytes → allocator needs 80 total (8 hdr + 64 payload)
    // Since remainder (104-80=24) < MIN_FREE (~48), it should take the whole coalesced block.
    void *d = my_malloc(56);
    
    assert(d == a);  // payload pointer reused at same address
    my_free(d);
    my_free(c);
}

static void test_top_shrink_reuse(void){
    // Allocate two blocks; 'y' is the last (top/wilderness).
    void *x = my_malloc(128);
    void *y = my_malloc(32);
    assert(x && y);

    // Free the top block. In a freelist-first allocator this typically
    // shrinks the wilderness but *doesn't* guarantee the next alloc
    // will reuse the same payload address.
    my_free(y);

    // Next alloc is allowed to come from the freelist; just require correctness.
    void *z = my_malloc(16);
    assert(z && aligned16(z));      // don't require z == y

    my_free(z);
    my_free(x);
}

static void test_null_and_zero(void){
    // free(NULL) should be a no-op
    my_free(NULL);

    // malloc(0) → we expect NULL in this allocator
    void *p = my_malloc(0);
    assert(p == NULL);
}

static void test_churn(void){
    enum { N = 96 };              // smaller, still exercises patterns well
    void *arr[N] = {0};

    // Allocate a mix of sizes (wrap at 64 to keep blocks small)
    for (int i = 0; i < N; i++){
        size_t sz = 1 + (i % 64);    // 1..64 bytes
        arr[i] = my_malloc(sz);
        assert(arr[i]);
        assert(aligned16(arr[i]));
    }

    // Free every third block to fragment the freelist
    for (int i = 0; i < N; i += 3){
        my_free(arr[i]);
        arr[i] = NULL;
    }

    // Reuse pressure: allocate & free a bunch of 64B payloads
    for (int i = 0; i < N; i++){
        void *p = my_malloc(64);
        assert(p && aligned16(p));
        my_free(p);
    }

    // Free the rest
    for (int i = 0; i < N; i++){
        if (arr[i]) my_free(arr[i]);
    }
}

int main(void){
    printf("[*] test_alignment...\n");
    test_alignment();
    
    printf("[*] test_coalesce_reuse...\n");
    test_coalesce_reuse();
    
    printf("[*] test_top_shrink_reuse...\n");
    test_top_shrink_reuse();
    
    printf("[*] test_null_and_zero...\n");
    test_null_and_zero();

    printf("[*] test_churn...\n");
    test_churn();

    printf("OK: all tests passed ✅\n");
    
    return 0;
}