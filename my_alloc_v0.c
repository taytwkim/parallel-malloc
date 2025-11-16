// #define _DARWIN_C_SOURCE
#include <sys/mman.h>
#include <stdint.h>
#include <stddef.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include <omp.h>

/* My Alloc V0
 * Minimal memory allocator with one global arena and a free list
 */

const int DEBUG = 0;
const int VERBOSE = 0;

// MMAP 1 GiB
#ifndef MYALLOC_REGION_SIZE
#define MYALLOC_REGION_SIZE (1ULL * 1024 * 1024 * 1024)
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

// ===== Global arena & free list =====
static uint8_t      *g_base = NULL;         // start of mmapped region
static uint8_t      *g_bump = NULL;         // unexplored region to carve out from
static uint8_t      *g_end  = NULL;         // one past end of the mmaped region
static free_chunk_t *g_free_list = NULL;    // head of doubly-linked free list
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

#define OFF(P) ((int)((uintptr_t)(P) - (uintptr_t)g_base))    // offset from g_base - for debugging

static void myalloc_init(void) {
    // mmap a large memory region and initialize the arena, called only once at the beginning
    
    if (DEBUG) printf("[alloc_init] entered\n");

    if (g_base) return; // initialize only once

    size_t req = MYALLOC_REGION_SIZE;
    size_t ps  = pagesize();

    if (req % ps) req += ps - (req % ps); // round the requested size up to OS page size (mmap maps whole pages)

    void *mem = mmap(NULL, req, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    
    if (mem == MAP_FAILED) return;

    g_base = (uint8_t*)mem;
    g_bump = g_base;
    g_end  = g_base + req;
    g_free_list = NULL;   // start with empty freelist; start carving on demand

    if (DEBUG) printf("[alloc_init] initialized arena: base=%d, end=%d, bump=%d\n", OFF(g_base), OFF(g_end), OFF(g_bump));
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

static inline void set_next_chunk_hdr_prev(void *hdr, int prev_in_use) {
    void *nxt = get_next_chunk_hdr(hdr);
    if ((uint8_t*)nxt < g_bump) set_prev_bit_in_hdr(nxt, prev_in_use);
}

static inline size_t get_free_chunk_min_size(void) {
    // return min size of a free chunk (need enough space for free chunk + footer)
    size_t need = align16(sizeof(free_chunk_t)) + sizeof(size_t);
    return align16(need);
}

// ===== Free List Operations =====
static void remove_from_free_list(free_chunk_t *fc) {
    free_chunk_t *fd = fc->links.fd, *bk = fc->links.bk;
    if (bk) bk->links.fd = fd;
    if (fd) fd->links.bk = bk;
    if (g_free_list == fc) g_free_list = fd;
    fc->links.fd = fc->links.bk = NULL;
}

static void push_front_to_free_list(free_chunk_t *fc) {
    fc->links.bk = NULL;
    fc->links.fd = g_free_list;
    if (g_free_list) g_free_list->links.bk = fc;
    g_free_list = fc;
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
static void* split_free_chunk(free_chunk_t *fc, size_t need_total) {
    size_t csz = get_chunk_size(fc);
    const size_t MIN_FREE = get_free_chunk_min_size();

    // Split only if the leftover chunk is big enough to be a valid free chunk
    if (csz >= need_total + MIN_FREE) {
        remove_from_free_list(fc);

        uint8_t *base = (uint8_t*)fc;
        set_hdr_keep_prev(base, need_total, 0);
        set_next_chunk_hdr_prev(base, 1);

        uint8_t *rem = base + need_total;
        size_t rem_sz = csz - need_total;
        
        set_hdr_keep_prev(rem, rem_sz, 1);
        set_ftr(rem, rem_sz);
        
        ((free_chunk_t*)rem)->links.fd = ((free_chunk_t*)rem)->links.bk = NULL;
        push_front_to_free_list((free_chunk_t*)rem);

        return base;
    }
    else {
        // Return the whole chunk
        remove_from_free_list(fc);
        set_hdr_keep_prev(fc, csz, 0);
        set_next_chunk_hdr_prev(fc, 1);
        return fc;
    }
}

static void* try_free_list(size_t need_total) {
    // Find a free chunk that is large enough. If not, return null
    for (free_chunk_t *p = g_free_list; p; p = p->links.fd) {
        if (!chunk_is_free(p)) continue;
        
        // need_total is header + payload
        if (get_chunk_size(p) >= need_total) {
            return split_free_chunk(p, need_total);
        }
    }
    return NULL;
}

static void* carve_from_top(size_t need_total) {
    uintptr_t start = (uintptr_t)g_bump;

    // Find a 16B-aligned payload address, then step back by header size.
    uintptr_t payload = (start + sizeof(size_t) + 15u) & ~((uintptr_t)15u);
    uint8_t *hdr = (uint8_t*)(payload - sizeof(size_t));

    if ((size_t)(g_end - hdr) < need_total) return NULL;

    set_hdr_keep_prev(hdr, need_total, 0);

    /* This looks suspicious but is actually correct.
     * Note that when we carve, the prev chunk will always be in-use. 
     * Because we always merge after free list, no chunk in the free list can be adjacent to the unexplored region
     * 
     * Also, when we carve at base==bump, we do want to set prev_bit as 1, in order to prevent left merge later
     */
    set_prev_bit_in_hdr(hdr, 1);

    g_bump = hdr + need_total;

    return hdr;
}


static void* coalesce(void *hdr) {
    size_t csz = get_chunk_size(hdr);

    // merge RIGHT if free
    void *nxt = get_next_chunk_hdr(hdr);
    if ((uint8_t*)nxt < g_bump && chunk_is_free(nxt)) {
        if (DEBUG && VERBOSE) printf("[coalesce] right chunk is free, merge with right chunk\n");

        size_t nxt_sz = get_chunk_size(nxt);
        remove_from_free_list((free_chunk_t*)nxt);
        csz += nxt_sz;
        set_hdr_keep_prev(hdr, csz, 1);
        set_ftr(hdr, csz);
    }

    // merge LEFT if free, but never look left of the first header (offset 8)
    if (prev_chunk_is_free(hdr)) {
        if (DEBUG && VERBOSE) printf("[coalesce] left chunk is free, merge with left chunk\n");

        uint8_t *p = (uint8_t*)hdr;
        size_t prev_footer = *(size_t*)(p - sizeof(size_t));

        if (get_free_bit_from_hdr(prev_footer)) {    // double check
            size_t prev_sz = get_size_from_hdr(prev_footer);
            void *prv = p - prev_sz;
            remove_from_free_list((free_chunk_t*)prv);
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
    // if (DEBUG) printf("[malloc] entered: req=%zu\n", size);
    
    if (size == 0) return NULL;

    pthread_mutex_lock(&g_lock);
    
    if (DEBUG) printf("[malloc] entered: req=%zu [tid=%d]\n", size, omp_get_thread_num());

    if (!g_base) myalloc_init();

    if (!g_base) {
        pthread_mutex_unlock(&g_lock);
        return NULL;
    }
    
    size_t payload = align16(size);
    size_t need = align16(sizeof(size_t) + payload); // header + payload

    if (DEBUG && VERBOSE) printf("[malloc] aligned: payload=%zu (from %zu), needed_size=%zu\n", payload, size, need);

    void *hdr = try_free_list(need);

    if (!hdr) {
        if (DEBUG && VERBOSE) printf("[malloc] freelist miss; carve from top; bump=%d\n", OFF(g_bump));

        hdr = carve_from_top(need); // if we weren't able to find a suitable chunk from the free list, carve chunk from top

        if (!hdr) {
            pthread_mutex_unlock(&g_lock);
            return NULL;  // out of arena
        }

        if (DEBUG && VERBOSE) { 
            uint8_t *payload_ptr = get_payload_from_hdr(hdr);
            uint8_t *chunk_end = (uint8_t*)hdr + get_chunk_size(hdr);
            printf("[malloc] from-top: hdr=%d  payload=%d  end=%d  size=%zu  aligned=%d\n", OFF(hdr), OFF(payload_ptr), OFF(chunk_end), get_chunk_size(hdr), ((uintptr_t)payload_ptr & 15u) == 0);
        }
    }
    else {
        if (DEBUG && VERBOSE) {
            uint8_t *payload_ptr = get_payload_from_hdr(hdr);
            uint8_t *chunk_end = (uint8_t*)hdr + get_chunk_size(hdr);
            printf("[malloc] from-free-list: hdr=%d  payload=%d  end=%d  size=%zu  aligned=%d\n", OFF(hdr), OFF(payload_ptr), OFF(chunk_end), get_chunk_size(hdr), ((uintptr_t)payload_ptr & 15u) == 0);
        }
    }
    
    void *ret = get_payload_from_hdr(hdr);

    if (DEBUG) printf("[malloc] exit: [tid=%d]\n", omp_get_thread_num());
    pthread_mutex_unlock(&g_lock);
    return ret;
}

void my_free(void *ptr) {
    // if (DEBUG) printf("[free] entered: ptr=%d\n", OFF(ptr));

    if (!ptr) return;

    pthread_mutex_lock(&g_lock);
    
    if (DEBUG) printf("[free] entered: ptr=%d [tid=%d]\n", OFF(ptr), omp_get_thread_num());

    uint8_t *hdr = (uint8_t*)get_hdr_from_payload(ptr);
    size_t csz = get_chunk_size(hdr);

    if (DEBUG && VERBOSE) printf("[free] header=%d, size=%zu\n", OFF(hdr), csz);

    set_hdr_keep_prev(hdr, csz, 1);
    set_ftr(hdr, csz);

    void *merged = coalesce(hdr);

    size_t msz = get_chunk_size(merged);
    uint8_t *merged_end = (uint8_t*)merged + msz;

    set_next_chunk_hdr_prev(merged, 0);

    // if the freed chunk touches the top, don't add to free list, shrink the unexplored region
    if (merged_end == g_bump) {
        g_bump = (uint8_t*)merged;
        if (DEBUG && VERBOSE) {
            printf("[free] touches top; shrink: new g_bump=%d\n", OFF(g_bump));
            // dump_free_list();
        }

        if (DEBUG) printf("[free] exit: [tid=%d]\n", omp_get_thread_num());
        pthread_mutex_unlock(&g_lock);
        return;
    }

    ((free_chunk_t*)merged)->links.fd = ((free_chunk_t*)merged)->links.bk = NULL;
    push_front_to_free_list((free_chunk_t*)merged);

    if (DEBUG && VERBOSE) {
        printf("[free] pushed to freelist: %d size=%zu\n", OFF(merged), msz);
        // dump_free_list();
    }

    if (DEBUG) printf("[free] exit: [tid=%d]\n", omp_get_thread_num());
    pthread_mutex_unlock(&g_lock);
}
