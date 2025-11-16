// #define _DARWIN_C_SOURCE
#include <sys/mman.h>
#include <stdint.h>
#include <stddef.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include <omp.h>

/* My Alloc V1
 * Update from V0: Added multiple arenas and tcaches 
 */

const int DEBUG = 0;
const int VERBOSE = 0;

// 67,108,864 bytes (64 MiB)
#ifndef MYALLOC_REGION_SIZE
#define MYALLOC_REGION_SIZE (64ULL * 1024ULL * 1024ULL)
#endif

// ===== Helpers =====

/* Round up n to the next multiple of 16 (bytes)
 *
 * Why do we align to 16 bytes?
 * 
 * The hardware (or the compiler) expects addresses to be multiples of certain size.
 *  - char    → 1-byte alignment (can start anywhere)
 *  - int     → 4-byte alignment (address multiple of 4)
 *  - double  → 8-byte alignment (address multiple of 8) ... and so on.
 * 
 * So aligning by 16 is a good default.
 */
static inline size_t align16(size_t n) { return (n + 15u) & ~((size_t)15u); } 

static inline size_t pagesize(void) { return (size_t)getpagesize(); }           // return the OS page size

// ===== Chunk =====

/* In-use:    [ header (size | flags) ]       8 bytes (in a 64 bit machine), the last four bits are flags
 *            [ payload ... ]
 * 
 * Free:      [ header (size | flags) ]       8 bytes
 *            [ fd ]                          8 bytes, forward pointer to the next free chunk
 *            [ bk ]                          8 bytes, backward pointer to the prev free chunk
 *            ... 
 *            [ footer (size | flags) ]       8 bytes, same as the header, but CHUNK_PREV_IN_USE_BIT is not actively updated
 * 
 * flags: 
 *    - bit 0: CHUNK_FREE_BIT
 *    - bit 1: CHUNK_PREV_IN_USE_BIT
 */

typedef struct free_links { 
    struct free_chunk *fd, *bk; 
} free_links_t;

typedef struct free_chunk {
    size_t        size_and_flags;           // total size (incl header; footer exists only when free)
    free_links_t  links;                    // valid only when free (lives at start of payload)
} free_chunk_t;

typedef struct arena {
    uint8_t      *base;           // start of mmapped region
    uint8_t      *bump;           // unexplored region
    uint8_t      *end;            // one past end
    free_chunk_t *free_list;      // head of free list
    pthread_mutex_t lock;         // lock protecting this arena
} arena_t;

#define MAX_ARENAS 64

static arena_t g_arenas[MAX_ARENAS];
static int g_narenas = 0;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static _Thread_local arena_t *t_arena = NULL;       // per-thread pointer to its assgined arena

static inline int OFF(arena_t *a, void *p) {
    return (int)((uintptr_t)p - (uintptr_t)a->base);
}

static void arena_init(arena_t *a) {
    size_t req = MYALLOC_REGION_SIZE;
    size_t ps  = pagesize();

    if (req % ps) req += ps - (req % ps);

    void *mem = mmap(NULL, req, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    
    if (mem == MAP_FAILED) {
        a->base = a->bump = a->end = NULL;
        a->free_list = NULL;
        return;
    }

    a->base = (uint8_t*)mem;
    a->bump = a->base;
    a->end  = a->base + req;
    a->free_list = NULL;

    pthread_mutex_init(&a->lock, NULL);

    if (DEBUG) printf("[arena_init] base=%d end=%d bump=%d\n", OFF(a, a->base), OFF(a, a->end), OFF(a, a->bump));
}

static void global_init(void) {
    int ncores = omp_get_max_threads();

    if (ncores < 1) ncores = 1;

    g_narenas = ncores;

    if (g_narenas > MAX_ARENAS) g_narenas = MAX_ARENAS;

    for (int i = 0; i < g_narenas; ++i) {
        memset(&g_arenas[i], 0, sizeof(arena_t));
        arena_init(&g_arenas[i]);
    }
}

static arena_t *get_my_arena(void) {
    pthread_once(&g_once, global_init); // check that global init has run

    if (t_arena) return t_arena;

    int tid = omp_get_thread_num();   // outside parallel region this is 0
    
    if (tid < 0) tid = 0;

    int idx = tid % g_narenas;
    t_arena = &g_arenas[idx];
    
    return t_arena;
}

// ===== Tcache =====

#define TCACHE_MAX_BINS   64
#define TCACHE_MAX_COUNT  32

typedef struct tcache_bin {
    free_chunk_t *head;   // head of the linked list
    int count;
} tcache_bin_t;

static _Thread_local tcache_bin_t g_tcache[TCACHE_MAX_BINS];  // per-thread tcache

static inline int size_to_tcache_bin(size_t usable) {
    /* Size classes are 16, 32, ...
     * 
     * Bin 0: usable in [16 .. 31]
     * Bin 1: usable in [32 .. 47]
     * ...
     */

    size_t idx = usable / 16;
    if (idx == 0) return -1;               // too small
    if (idx > TCACHE_MAX_BINS) return -1;  // too big for tcache, try free list
    return (int)(idx - 1);                 // index is 0-based
}

// ===== Chunk Flags and Masks =====

// CHUNK_SIZE_MASK clears the flag bits (…FFF0)
//    - header & CHUNK_SIZE_MASK → size
#define CHUNK_SIZE_MASK (~(size_t)0xFUL)

// CHUNK_FREE_BIT = mask for bit 0 (…0001)
//   - Set  : header |=  CHUNK_FREE_BIT → mark chunk FREE
//   - Clear: header &= ~CHUNK_FREE_BIT → mark chunk IN-USE
#define CHUNK_FREE_BIT ((size_t)1)

/* CHUNK_PREV_IN_USE_BIT = mask for bit 1 (…0010)
 *   - Set  : header |=  CHUNK_PREV_IN_USE_BIT → previous chunk is IN-USE
 *   - Clear: header &= ~CHUNK_PREV_IN_USE_BIT → previous chunk is FREE
 * 
 * Why do we need this flag?
 * 
 * We need this flag when merging two chunks. When a chunk is freed, we look at its left neighbor and try to merge.
 * But we want to first make sure that the left chunk is actually free. If we naively read from the left chunk's footer without checking, 
 * we might be reading from the payload of an in-use chunk.
 * This is not really a problem when merging with the right chunk, because both free and in-use chunks have the header
 */
#define CHUNK_PREV_IN_USE_BIT ((size_t)2)

// ===== Header & Footer Operations =====
static inline size_t get_size_from_hdr(size_t hdr) { return hdr & CHUNK_SIZE_MASK; }
static inline int get_free_bit_from_hdr(size_t hdr) { return (int)(hdr & CHUNK_FREE_BIT); }
static inline int get_prev_from_hdr(size_t hdr_word) { return (int)(hdr_word & CHUNK_PREV_IN_USE_BIT); }

static inline void set_prev_bit_in_hdr(void *hdr, int on) {
    size_t h = *(size_t*)hdr;
    if (on)  h |=  CHUNK_PREV_IN_USE_BIT;
    else     h &= ~CHUNK_PREV_IN_USE_BIT;
    *(size_t*)hdr = h;
}

static inline size_t build_hdr_with_free_bit(size_t size_aligned, int is_free) {
    // return header and set free bit
    size_t s = size_aligned & CHUNK_SIZE_MASK;   // keep high bits only
    return is_free ? (s | CHUNK_FREE_BIT) : (s & ~CHUNK_FREE_BIT);
}

static inline void set_hdr_keep_prev(void *hdr, size_t size_aligned, int is_free) {
    // set header, but don't update the prev_in_use_bit
    size_t old   = *(size_t*)hdr;
    size_t prevb = old & CHUNK_PREV_IN_USE_BIT;
    *(size_t*)hdr = build_hdr_with_free_bit(size_aligned, is_free) | prevb;
}

static inline void set_ftr(void *hdr, size_t size_aligned) {
    uint8_t *base = (uint8_t*)hdr;
    *(size_t*)(base + size_aligned - sizeof(size_t)) = build_hdr_with_free_bit(size_aligned, 1);
}

// ===== Pointer Conversion Btwn Header and Payload =====
static inline uint8_t* get_payload_from_hdr(void *hdr) { return (uint8_t*)hdr + sizeof(size_t); }
static inline void* get_hdr_from_payload(void *ptr) { return (uint8_t*)ptr - sizeof(size_t); }

// ===== Chunk Operations =====
static inline size_t get_chunk_size(void *hdr) { return get_size_from_hdr(*(size_t*)hdr); }
static inline int chunk_is_free(void *hdr) { return get_free_bit_from_hdr(*(size_t*)hdr); }
static inline int prev_chunk_is_free(void *hdr) { return !get_prev_from_hdr(*(size_t*)hdr); }
static inline void* get_next_chunk_hdr(void *hdr) { return (uint8_t*)hdr + get_chunk_size(hdr); }

static inline void set_next_chunk_hdr_prev(arena_t *a, void *hdr, int prev_in_use) {
    void *nxt = get_next_chunk_hdr(hdr);
    if ((uint8_t*)nxt < a->bump) set_prev_bit_in_hdr(nxt, prev_in_use);
}

static inline size_t get_free_chunk_min_size(void) {
    // return min size of a free chunk (need enough space for free chunk + footer)
    size_t need = align16(sizeof(free_chunk_t)) + sizeof(size_t);
    return align16(need);
}

// ===== Free List Operations =====
static void remove_from_free_list(arena_t *a, free_chunk_t *fc) {
    free_chunk_t *fd = fc->links.fd, *bk = fc->links.bk;
    if (bk) bk->links.fd = fd;
    if (fd) fd->links.bk = bk;
    if (a->free_list == fc) a->free_list = fd;
    fc->links.fd = fc->links.bk = NULL;
}

static void push_front_to_free_list(arena_t *a, free_chunk_t *fc) {
    fc->links.bk = NULL;
    fc->links.fd = a->free_list;
    if (a->free_list) a->free_list->links.bk = fc;
    a->free_list = fc;
}

/*
static void dump_free_list(void) {
    printf("free list:\n");
    free_chunk_t *p = g_free_list;

    if (!p) { printf("  (empty)\n"); return; }

    size_t i = 0;
    while (p) {
        uint8_t *end = (uint8_t*)p + get_chunk_size(p);
        printf("  #%zu hdr=%d end=%d\n", i, OFF(p), OFF(end));

        free_chunk_t *next = p->links.fd;
        p = next;
        i += 1;
    }
}
*/

// ===== Core helpers =====
static void* split_free_chunk(arena_t *a, free_chunk_t *fc, size_t need_total) {
    size_t csz = get_chunk_size(fc);
    const size_t MIN_FREE = get_free_chunk_min_size();

    if (csz >= need_total + MIN_FREE) {
        remove_from_free_list(a, fc);

        uint8_t *base = (uint8_t*)fc;
        set_hdr_keep_prev(base, need_total, 0);
        set_next_chunk_hdr_prev(a, base, 1);

        uint8_t *rem = base + need_total;
        size_t rem_sz = csz - need_total;

        set_hdr_keep_prev(rem, rem_sz, 1);
        set_ftr(rem, rem_sz);

        ((free_chunk_t*)rem)->links.fd = ((free_chunk_t*)rem)->links.bk = NULL;
        push_front_to_free_list(a, (free_chunk_t*)rem);

        return base;
    } else {
        remove_from_free_list(a, fc);
        set_hdr_keep_prev(fc, csz, 0);
        set_next_chunk_hdr_prev(a, fc, 1);
        return fc;
    }
}

static void* try_free_list(arena_t *a, size_t need_total) {
    for (free_chunk_t *p = a->free_list; p; p = p->links.fd) {
        if (!chunk_is_free(p)) continue;

        if (get_chunk_size(p) >= need_total) {
            return split_free_chunk(a, p, need_total);
        }
    }
    return NULL;
}

static void* carve_from_top(arena_t *a, size_t need_total) {
    uintptr_t start = (uintptr_t)a->bump;

    uintptr_t payload = (start + sizeof(size_t) + 15u) & ~((uintptr_t)15u);
    uint8_t *hdr = (uint8_t*)(payload - sizeof(size_t));

    if ((size_t)(a->end - hdr) < need_total) return NULL;

    set_hdr_keep_prev(hdr, need_total, 0);
    set_prev_bit_in_hdr(hdr, 1);

    a->bump = hdr + need_total;

    return hdr;
}

static void* coalesce(arena_t *a, void *hdr) {
    size_t csz = get_chunk_size(hdr);

    void *nxt = get_next_chunk_hdr(hdr);
    if ((uint8_t*)nxt < a->bump && chunk_is_free(nxt)) {
        if (DEBUG && VERBOSE) printf("[coalesce] right chunk is free, merge with right chunk\n");

        size_t nxt_sz = get_chunk_size(nxt);
        remove_from_free_list(a, (free_chunk_t*)nxt);
        csz += nxt_sz;
        set_hdr_keep_prev(hdr, csz, 1);
        set_ftr(hdr, csz);
    }

    if (prev_chunk_is_free(hdr)) {
        if (DEBUG && VERBOSE) printf("[coalesce] left chunk is free, merge with left chunk\n");

        uint8_t *p = (uint8_t*)hdr;
        size_t prev_footer = *(size_t*)(p - sizeof(size_t));

        if (get_free_bit_from_hdr(prev_footer)) {
            size_t prev_sz = get_size_from_hdr(prev_footer);
            void *prv = p - prev_sz;
            remove_from_free_list(a, (free_chunk_t*)prv);
            csz += prev_sz;
            set_hdr_keep_prev(prv, csz, 1);
            set_ftr(prv, csz);
            hdr = prv;
        }
    }
    return hdr;
}

// ===== Malloc API =====
void *my_malloc(size_t size) {
    if (size == 0) return NULL;

    arena_t *a = get_my_arena();
    if (!a || !a->base) return NULL;

    pthread_mutex_lock(&a->lock);

    if (DEBUG) printf("[malloc] entered: req=%zu [tid=%d]\n", size, omp_get_thread_num());

    size_t payload = align16(size);
    size_t need = align16(sizeof(size_t) + payload);  // header + payload
    size_t usable = need - sizeof(size_t);            // payload + padding for alignment

    int bin = size_to_tcache_bin(usable);

    if (DEBUG && VERBOSE) printf("[malloc] aligned: payload=%zu (from %zu), need=%zu usable=%zu bin=%d\n", payload, size, need, usable, bin);

    // 1) Try per-thread tcache first
    void *hdr = NULL;

    if (bin >= 0) {
        tcache_bin_t *b = &g_tcache[bin];
        if (b->head != NULL) {
            free_chunk_t *fc = b->head;
            b->head = fc->links.fd;
            b->count--;

            hdr = (void*)fc;

            if (DEBUG && VERBOSE) {
                uint8_t *payload_ptr = get_payload_from_hdr(hdr);
                uint8_t *chunk_end   = (uint8_t*)hdr + get_chunk_size(hdr);           
                printf("[malloc] from-tcache: hdr=%d payload=%d end=%d size=%zu\n", OFF(a, hdr), OFF(a, payload_ptr), OFF(a, chunk_end), get_chunk_size(hdr));
            }
        }
    }

    // 2) If tcache miss, fall back to arena freelist / bump
    if (!hdr) {
        hdr = try_free_list(a, need);

        if (!hdr) {
            if (DEBUG && VERBOSE) printf("[malloc] freelist miss; carve from top; bump=%d\n", OFF(a, a->bump));

            hdr = carve_from_top(a, need); // carve from top

            if (!hdr) {
                pthread_mutex_unlock(&a->lock);
                return NULL;  // out of arena
            }

            if (DEBUG && VERBOSE) { 
                uint8_t *payload_ptr = get_payload_from_hdr(hdr);
                uint8_t *chunk_end   = (uint8_t*)hdr + get_chunk_size(hdr);
                printf("[malloc] from-top: hdr=%d  payload=%d  end=%d  size=%zu  aligned=%d\n", OFF(a, hdr), OFF(a, payload_ptr), OFF(a, chunk_end), get_chunk_size(hdr), ((uintptr_t)payload_ptr & 15u) == 0);
            }
        } 
        else {
            if (DEBUG && VERBOSE) {
                uint8_t *payload_ptr = get_payload_from_hdr(hdr);
                uint8_t *chunk_end   = (uint8_t*)hdr + get_chunk_size(hdr);
                printf("[malloc] from-free-list: hdr=%d  payload=%d  end=%d  size=%zu  aligned=%d\n", OFF(a, hdr), OFF(a, payload_ptr), OFF(a, chunk_end), get_chunk_size(hdr), ((uintptr_t)payload_ptr & 15u) == 0);
            }
        }
    }

    void *ret = get_payload_from_hdr(hdr);

    if (DEBUG) printf("[malloc] exit: [tid=%d]\n", omp_get_thread_num());
    
    pthread_mutex_unlock(&a->lock);
    return ret;
}

void my_free(void *ptr) {
    if (!ptr) return;

    arena_t *a = get_my_arena();
    if (!a || !a->base) return;

    pthread_mutex_lock(&a->lock);

    if (DEBUG) printf("[free] entered: ptr=%d [tid=%d]\n", OFF(a, ptr), omp_get_thread_num());

    uint8_t *hdr = (uint8_t*)get_hdr_from_payload(ptr);
    size_t csz = get_chunk_size(hdr);
    size_t usable = csz - sizeof(size_t);
    int bin = size_to_tcache_bin(usable);

    if (DEBUG && VERBOSE) printf("[free] header=%d, size=%zu usable=%zu bin=%d\n", OFF(a, hdr), csz, usable, bin);

    // 1) Try to put small chunks into per-thread tcache
    if (bin >= 0) {
        tcache_bin_t *b = &g_tcache[bin];
        if (b->count < TCACHE_MAX_COUNT) {
            free_chunk_t *fc = (free_chunk_t*)hdr;

            // IMPORTANT: do NOT mark as free, do NOT set footer, do NOT coalesce.
            // Chunk stays "in-use" from the global allocator's point of view.
            
            fc->links.fd = b->head;
            b->head = fc;
            b->count++;

            if (DEBUG && VERBOSE) printf("[free] put into tcache bin=%d (count=%d)\n", bin, b->count);
            if (DEBUG) printf("[free] exit (tcache): [tid=%d]\n", omp_get_thread_num());
            
            pthread_mutex_unlock(&a->lock);
            return;
        }
    }

    // 2) Otherwise fall back to the old free path: mark free, coalesce, push into arena freelist.

    set_hdr_keep_prev(hdr, csz, 1);  // mark free (sets FREE bit, keeps prev bit)
    set_ftr(hdr, csz);

    void *merged = coalesce(a, hdr);

    size_t msz = get_chunk_size(merged);
    uint8_t *merged_end = (uint8_t*)merged + msz;

    set_next_chunk_hdr_prev(a, merged, 0);

    // if the freed chunk touches the top, don't add to free list, shrink the unexplored region
    if (merged_end == a->bump) {
        a->bump = (uint8_t*)merged;

        if (DEBUG && VERBOSE) printf("[free] touches top; shrink: new bump=%d\n", OFF(a, a->bump));
        if (DEBUG) printf("[free] exit (shrink): [tid=%d]\n", omp_get_thread_num());

        pthread_mutex_unlock(&a->lock);
        return;
    }

    ((free_chunk_t*)merged)->links.fd = ((free_chunk_t*)merged)->links.bk = NULL;
    push_front_to_free_list(a, (free_chunk_t*)merged);

    if (DEBUG && VERBOSE) printf("[free] pushed to freelist: %d size=%zu\n", OFF(a, merged), msz);
    if (DEBUG) printf("[free] exit: [tid=%d]\n", omp_get_thread_num());

    pthread_mutex_unlock(&a->lock);
}
