#include "stdlib.h"
#include "string.h"

/* ---- malloc / free --------------------------------------------------------
 *
 * Simple first-fit heap inside a static 64 KiB arena in .bss.
 * No SYS_BRK needed — the arena is part of the ELF's BSS segment which the
 * kernel maps at load time.  64 KiB is enough for typical user programs; the
 * ceiling can be raised by changing HEAP_SIZE.
 *
 * Block header layout (16 bytes):
 *   uint32_t magic  — 0xCAFEF00D = free, 0xDEADBEEF = used  (corruption guard)
 *   uint32_t size   — usable bytes after this header
 *   uint32_t free   — 1 = free, 0 = used
 *   uint32_t _pad   — reserved, keeps header 16-byte aligned
 *
 * Blocks are always 8-byte aligned.
 * Forward + backward coalescing on free.
 */

#define HEAP_SIZE   (64 * 1024)     /* 64 KiB static arena              */
#define HDR_SIZE    ((uint32_t)sizeof(block_hdr_t))
#define MIN_SPLIT   16              /* don't split if remainder < this   */
#define ALIGN8(n)   (((n) + 7u) & ~7u)

#define MAGIC_FREE  0xCAFEF00Du
#define MAGIC_USED  0xDEADBEEFu

typedef struct block_hdr {
    uint32_t magic;
    uint32_t size;
    uint32_t free;
    uint32_t _pad;
} block_hdr_t;

/* The arena lives in .bss — zero-initialised by the ELF loader. */
static unsigned char heap_arena[HEAP_SIZE];
static int heap_ready = 0;

/* ---- internal helpers ---------------------------------------------------- */

static inline block_hdr_t* hdr_of(void* ptr)
{
    return (block_hdr_t*)((unsigned char*)ptr - HDR_SIZE);
}

static inline int in_arena(block_hdr_t* blk)
{
    return (unsigned char*)blk >= heap_arena &&
           (unsigned char*)blk <  heap_arena + HEAP_SIZE;
}

/* Walk forward to the next block header (or one-past-end of arena). */
static inline block_hdr_t* next_blk(block_hdr_t* blk)
{
    return (block_hdr_t*)((unsigned char*)blk + HDR_SIZE + blk->size);
}

/* ---- heap init ----------------------------------------------------------- */

static void heap_init(void)
{
    block_hdr_t* hdr = (block_hdr_t*)heap_arena;
    hdr->magic = MAGIC_FREE;
    hdr->size  = HEAP_SIZE - HDR_SIZE;
    hdr->free  = 1;
    hdr->_pad  = 0;
    heap_ready = 1;
}

/* ---- malloc -------------------------------------------------------------- */

void* malloc(size_t size)
{
    if (!size) return 0;
    if (!heap_ready) heap_init();

    size = ALIGN8(size);

    block_hdr_t* blk = (block_hdr_t*)heap_arena;

    while (in_arena(blk)) {
        if (blk->free && blk->size >= size) {
            /* Split if the remainder is large enough to be useful. */
            if (blk->size >= size + HDR_SIZE + MIN_SPLIT) {
                block_hdr_t* split = (block_hdr_t*)((unsigned char*)blk + HDR_SIZE + size);
                split->magic = MAGIC_FREE;
                split->size  = blk->size - size - HDR_SIZE;
                split->free  = 1;
                split->_pad  = 0;
                blk->size    = size;
            }
            blk->magic = MAGIC_USED;
            blk->free  = 0;
            return (unsigned char*)blk + HDR_SIZE;
        }
        blk = next_blk(blk);
    }

    return 0;  /* out of memory */
}

/* ---- calloc -------------------------------------------------------------- */

void* calloc(size_t nmemb, size_t size)
{
    /* Overflow check: nmemb * size must not wrap. */
    if (size && nmemb > (size_t)-1 / size)
        return 0;

    size_t total = nmemb * size;
    void* ptr = malloc(total);
    if (ptr) memset(ptr, 0, total);
    return ptr;
}

/* ---- free ---------------------------------------------------------------- */

void free(void* ptr)
{
    if (!ptr) return;

    block_hdr_t* blk = hdr_of(ptr);

    /* Bounds check — catch wild pointers. */
    if (!in_arena(blk))
        return;  /* silently ignore out-of-arena pointers */

    /* Magic check — detect double-free and heap corruption. */
    if (blk->magic == MAGIC_FREE)
        return;  /* double-free: already freed, ignore */
    if (blk->magic != MAGIC_USED)
        return;  /* corrupted header, ignore */

    blk->magic = MAGIC_FREE;
    blk->free  = 1;

    /* --- Forward coalesce: merge with all immediately following free blocks. */
    block_hdr_t* nxt = next_blk(blk);
    while (in_arena(nxt) && nxt->free && nxt->magic == MAGIC_FREE) {
        blk->size += HDR_SIZE + nxt->size;
        nxt = next_blk(blk);
    }

    /* --- Backward coalesce: walk from the arena start to find our predecessor.
     *
     * We have no prev pointer in the header (keeping the struct small), so we
     * do a linear walk.  This is O(n) in the number of blocks, but free() is
     * not called in hot loops in typical user programs and the arena is small
     * (64 KiB → at most ~4096 8-byte minimum blocks).
     */
    block_hdr_t* prev = 0;
    block_hdr_t* cur  = (block_hdr_t*)heap_arena;
    while (in_arena(cur) && cur != blk) {
        prev = cur;
        cur  = next_blk(cur);
    }
    if (prev && prev->free && prev->magic == MAGIC_FREE) {
        prev->size += HDR_SIZE + blk->size;
        /* blk is now absorbed into prev — nothing else to do. */
    }
}

/* ---- atoi ----------------------------------------------------------------- */

int atoi(const char* s)
{
    int n = 0, neg = 0;
    while (*s == ' ') s++;
    if      (*s == '-') { neg = 1; s++; }
    else if (*s == '+') { s++; }
    while (*s >= '0' && *s <= '9') n = n * 10 + (*s++ - '0');
    return neg ? -n : n;
}

/* ---- itoa ----------------------------------------------------------------- */

char* itoa(int n, char* buf, int base)
{
    static const char digits[] = "0123456789ABCDEF";
    char tmp[34];
    int  i = 0, neg = 0;

    if (base < 2 || base > 16) { buf[0] = '\0'; return buf; }

    if (n == 0) { buf[0] = '0'; buf[1] = '\0'; return buf; }

    if (n < 0 && base == 10) { neg = 1; n = -n; }

    unsigned int un = (unsigned int)n;
    while (un) {
        tmp[i++] = digits[un % (unsigned int)base];
        un /= (unsigned int)base;
    }
    if (neg) tmp[i++] = '-';

    int j = 0;
    while (i--) buf[j++] = tmp[i];
    buf[j] = '\0';
    return buf;
}