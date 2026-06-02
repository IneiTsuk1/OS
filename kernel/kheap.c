#include "kheap.h"
#include "paging.h"
#include "pmm.h"
#include "klog.h"
#include "spinlock.h"
#include "panic.h"
#include <stdint.h>
#include <stddef.h>

extern uint8_t kernel_end;

#define KHEAP_START  (((uint32_t)&kernel_end + PMM_PAGE_SIZE - 1) \
                       & ~(PMM_PAGE_SIZE - 1))
#define KHEAP_MAX    (KHEAP_START + 16 * 1024 * 1024)  // 16 MiB ceiling
#define MIN_SPLIT    32  // don't split if remainder would be smaller than this

typedef struct block_header {
    uint32_t             size;  // usable bytes after this header
    uint32_t             free;  // 1 = free, 0 = used
    struct block_header* next;  // next block (NULL = last)
    struct block_header* prev;  // previous block (NULL = first)
} block_header_t;

static block_header_t* heap_head = NULL;
static uint32_t        heap_top  = 0;

static spinlock_t heap_lock = SPINLOCK_INIT;

// ---- internal helpers ------------------------------------------------------

// Extend the heap by at least `bytes`, mapping fresh pages.
// Must be called with heap_lock held.
// Returns 0 on success, -1 on failure.
static int heap_grow(uint32_t bytes)
{
    uint32_t pages = (bytes + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE;

    for (uint32_t i = 0; i < pages; i++) {
        if (heap_top >= KHEAP_MAX)
            return -1;

        uint32_t frame = pmm_alloc_frame();
        if (!frame)
            return -1;

        paging_map(heap_top, frame, PAGE_PRESENT | PAGE_WRITABLE);
        heap_top += PMM_PAGE_SIZE;
    }

    return 0;
}

// Split `blk` so that its usable region is exactly `size` bytes, creating a
// new free block from the remainder if the remainder is large enough.
// Must be called with heap_lock held and blk->free == 0.
static void maybe_split(block_header_t* blk, size_t size)
{
    if (blk->size < size + sizeof(block_header_t) + MIN_SPLIT)
        return;

    block_header_t* split = (block_header_t*)
        ((uint8_t*)blk + sizeof(block_header_t) + size);

    split->size = blk->size - size - sizeof(block_header_t);
    split->free = 1;
    split->next = blk->next;
    split->prev = blk;

    if (split->next)
        split->next->prev = split;

    blk->size = size;
    blk->next = split;
}

// ---- public API ------------------------------------------------------------

void kheap_init(void)
{
    heap_top = KHEAP_START;

    if (heap_grow(PMM_PAGE_SIZE) != 0)
        panic("kheap_init: failed to map initial heap page");

    heap_head        = (block_header_t*)KHEAP_START;
    heap_head->size  = PMM_PAGE_SIZE - sizeof(block_header_t);
    heap_head->free  = 1;
    heap_head->next  = NULL;
    heap_head->prev  = NULL;

    klog_info("Heap: start=%x size=%u KiB ceiling=%x",
        KHEAP_START, (KHEAP_MAX - KHEAP_START) / 1024, KHEAP_MAX);
}

void* kmalloc(size_t size)
{
    if (size == 0)
        return NULL;

    size = (size + 7) & ~7u;  // align to 8 bytes

    spinlock_acquire(&heap_lock);

    block_header_t* blk = heap_head;

    while (blk) {
        if (blk->free && blk->size >= size) {
            maybe_split(blk, size);
            blk->free = 0;
            spinlock_release(&heap_lock);
            return (void*)((uint8_t*)blk + sizeof(block_header_t));
        }

        if (!blk->next) {
            uint32_t needed = sizeof(block_header_t) + size;
            if (heap_grow(needed) != 0) {
                spinlock_release(&heap_lock);
                return NULL;
            }

            block_header_t* newblk = (block_header_t*)
                ((uint8_t*)blk + sizeof(block_header_t) + blk->size);

            if (blk->free) {
                blk->size += heap_top - (uint32_t)newblk;
            } else {
                newblk->size = heap_top
                               - (uint32_t)newblk
                               - sizeof(block_header_t);
                newblk->free = 1;
                newblk->next = NULL;
                newblk->prev = blk;
                blk->next    = newblk;
            }
            continue;
        }

        blk = blk->next;
    }

    spinlock_release(&heap_lock);
    return NULL;
}

void kfree(void* ptr)
{
    if (!ptr)
        return;

    block_header_t* blk = (block_header_t*)
        ((uint8_t*)ptr - sizeof(block_header_t));

    spinlock_acquire(&heap_lock);

    if (blk->free)
        panic("kfree: double free at %x", ptr);

    blk->free = 1;

    // Coalesce forward
    if (blk->next && blk->next->free) {
        block_header_t* next = blk->next;
        blk->size += sizeof(block_header_t) + next->size;
        blk->next  = next->next;
        if (blk->next)
            blk->next->prev = blk;
    }

    // Coalesce backward
    if (blk->prev && blk->prev->free) {
        block_header_t* prev = blk->prev;
        prev->size += sizeof(block_header_t) + blk->size;
        prev->next  = blk->next;
        if (prev->next)
            prev->next->prev = prev;
    }

    spinlock_release(&heap_lock);
}

void* krealloc(void* ptr, size_t new_size)
{
    // krealloc(NULL, size) == kmalloc(size)
    if (!ptr)
        return kmalloc(new_size);

    // krealloc(ptr, 0) == kfree(ptr)
    if (new_size == 0) {
        kfree(ptr);
        return NULL;
    }

    new_size = (new_size + 7) & ~7u;  // align to 8 bytes

    block_header_t* blk = (block_header_t*)
        ((uint8_t*)ptr - sizeof(block_header_t));

    spinlock_acquire(&heap_lock);

    if (blk->free)
        panic("krealloc: called on free block at %x", ptr);

    // Case 1: block is already large enough — split off excess and return.
    if (blk->size >= new_size) {
        maybe_split(blk, new_size);
        spinlock_release(&heap_lock);
        return ptr;
    }

    // Case 2: next block is free and combined size is sufficient — absorb it
    // in-place, no copy needed.
    if (blk->next && blk->next->free) {
        uint32_t combined = blk->size
                          + sizeof(block_header_t)
                          + blk->next->size;

        if (combined >= new_size) {
            block_header_t* next = blk->next;
            blk->size = combined;
            blk->next = next->next;
            if (blk->next)
                blk->next->prev = blk;

            maybe_split(blk, new_size);
            spinlock_release(&heap_lock);
            return ptr;
        }
    }

    // Case 3: need a new allocation — alloc, copy, free.
    // Release the lock before calling kmalloc (which acquires it internally).
    uint32_t old_size = blk->size;
    spinlock_release(&heap_lock);

    void* new_ptr = kmalloc(new_size);
    if (!new_ptr)
        return NULL;  // original block unchanged

    // Copy old contents
    uint8_t* src = (uint8_t*)ptr;
    uint8_t* dst = (uint8_t*)new_ptr;
    uint32_t copy_len = old_size < new_size ? old_size : new_size;
    for (uint32_t i = 0; i < copy_len; i++)
        dst[i] = src[i];

    kfree(ptr);
    return new_ptr;
}

void kheap_dump(void)
{
#define KHEAP_DUMP_MAX 256
    struct { uint32_t addr; uint32_t size; uint32_t is_free; } snap[KHEAP_DUMP_MAX];
    uint32_t nblocks = 0, used = 0, free_bytes = 0;

    spinlock_acquire(&heap_lock);
    block_header_t* blk = heap_head;
    while (blk && nblocks < KHEAP_DUMP_MAX) {
        snap[nblocks].addr    = (uint32_t)blk + sizeof(block_header_t);
        snap[nblocks].size    = blk->size;
        snap[nblocks].is_free = blk->free;
        if (blk->free) free_bytes += blk->size;
        else           used       += blk->size;
        nblocks++;
        blk = blk->next;
    }
    spinlock_release(&heap_lock);

    klog_info("Heap dump:");
    for (uint32_t i = 0; i < nblocks; i++) {
        klog_info("  [%u] addr=%x size=%u %s",
            i, snap[i].addr, snap[i].size,
            snap[i].is_free ? "FREE" : "USED");
    }
    klog_info("Heap: %u blocks, %u used, %u free (bytes)", nblocks, used, free_bytes);
}