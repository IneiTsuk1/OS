#include "stdlib.h"
#include "string.h"

/* ---- malloc / free --------------------------------------------------------
 *
 * Simple first-fit heap inside a static 64 KiB arena in .bss.
 * No SYS_BRK needed — the arena is part of the ELF's BSS segment which the
 * kernel maps at load time.  64 KiB is enough for typical user programs; the
 * ceiling can be raised by changing HEAP_SIZE.
 *
 * Block header layout (8 bytes):
 *   uint32_t size   — usable bytes after this header
 *   uint32_t free   — 1 = free, 0 = used
 *
 * Blocks are always 8-byte aligned.  Forward coalescing on free.
 * No backward coalescing (keeps the implementation small).
 */

#define HEAP_SIZE   (64 * 1024)   /* 64 KiB static arena */
#define HDR_SIZE    8             /* size of block_hdr_t */
#define MIN_SPLIT   16            /* don't split if remainder < this */
#define ALIGN8(n)   (((n) + 7) & ~7u)

typedef struct block_hdr {
    uint32_t size;
    uint32_t free;
} block_hdr_t;

/* The arena lives in .bss — zero-initialised by the ELF loader. */
static unsigned char heap_arena[HEAP_SIZE];
static int heap_ready = 0;

static void heap_init(void)
{
    block_hdr_t* hdr = (block_hdr_t*)heap_arena;
    hdr->size = HEAP_SIZE - HDR_SIZE;
    hdr->free = 1;
    heap_ready = 1;
}

void* malloc(size_t size)
{
    if (!size) return 0;
    if (!heap_ready) heap_init();

    size = ALIGN8(size);

    block_hdr_t* blk = (block_hdr_t*)heap_arena;

    while ((unsigned char*)blk < heap_arena + HEAP_SIZE) {
        if (blk->free && blk->size >= size) {
            /* Split if the remainder is large enough to be useful. */
            if (blk->size >= size + HDR_SIZE + MIN_SPLIT) {
                block_hdr_t* next = (block_hdr_t*)((unsigned char*)blk + HDR_SIZE + size);
                next->size = blk->size - size - HDR_SIZE;
                next->free = 1;
                blk->size  = size;
            }
            blk->free = 0;
            return (unsigned char*)blk + HDR_SIZE;
        }
        blk = (block_hdr_t*)((unsigned char*)blk + HDR_SIZE + blk->size);
    }

    return 0;  /* out of memory */
}

void* calloc(size_t nmemb, size_t size)
{
    size_t total = nmemb * size;
    void* ptr = malloc(total);
    if (ptr) memset(ptr, 0, total);
    return ptr;
}

void free(void* ptr)
{
    if (!ptr) return;

    block_hdr_t* blk = (block_hdr_t*)((unsigned char*)ptr - HDR_SIZE);
    blk->free = 1;

    /* Forward coalesce: merge with the immediately following free block. */
    block_hdr_t* next = (block_hdr_t*)((unsigned char*)blk + HDR_SIZE + blk->size);
    while ((unsigned char*)next < heap_arena + HEAP_SIZE && next->free) {
        blk->size += HDR_SIZE + next->size;
        next = (block_hdr_t*)((unsigned char*)blk + HDR_SIZE + blk->size);
    }
}

/* ---- atoi ----------------------------------------------------------------- */

int atoi(const char* s)
{
    int n = 0, neg = 0;
    while (*s == ' ') s++;
    if (*s == '-') { neg = 1; s++; }
    else if (*s == '+') s++;
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