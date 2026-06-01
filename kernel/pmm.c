#include "pmm.h"
#include "mmap.h"
#include "klog.h"
#include "panic.h"
#include "spinlock.h"
#include <stdint.h>

#define PMM_MAX_FRAMES  (0x100000000ULL / PMM_PAGE_SIZE)  // 1M frames for 4 GiB
#define BITMAP_SIZE     (PMM_MAX_FRAMES / 32)             // uint32_t words

// 1 = used/reserved, 0 = free
static uint32_t bitmap[BITMAP_SIZE];  // 128 KiB in .bss

static uint32_t total_frames = 0;
static uint32_t free_frames  = 0;

// Protects bitmap, free_frames during concurrent alloc/free.
// pmm_init() runs before interrupts and SMP, so no lock needed there.
static spinlock_t pmm_lock = SPINLOCK_INIT;

// Kernel image bounds — provided by the linker
extern uint8_t kernel_end;

// ---- bit helpers (called with pmm_lock held) --------------------------------

static inline void bitmap_set(uint32_t frame)
{
    bitmap[frame / 32] |= (1u << (frame % 32));
}

static inline void bitmap_clear(uint32_t frame)
{
    bitmap[frame / 32] &= ~(1u << (frame % 32));
}

static inline int bitmap_test(uint32_t frame)
{
    return (bitmap[frame / 32] >> (frame % 32)) & 1u;
}

// ---- public API ------------------------------------------------------------

void pmm_init(void)
{
    // Runs single-threaded before interrupts — no lock needed.

    // Step 1: mark every frame as used
    for (uint32_t i = 0; i < BITMAP_SIZE; i++)
        bitmap[i] = 0xFFFFFFFF;

    // Step 2: free all MMAP_TYPE_AVAILABLE regions
    for (int i = 0; i < mmap_entry_count; i++) {
        mmap_entry_t* e = &mmap_entries[i];

        if (e->type != MMAP_TYPE_AVAILABLE)
            continue;

        uint64_t end = e->base + e->length;
        if (end > 0x100000000ULL) end = 0x100000000ULL;
        if (e->base >= 0x100000000ULL) continue;

        uint32_t first_frame = (uint32_t)((e->base + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE);
        uint32_t last_frame  = (uint32_t)(end / PMM_PAGE_SIZE);

        for (uint32_t f = first_frame; f < last_frame; f++) {
            bitmap_clear(f);
            free_frames++;
            total_frames++;
        }
    }

    // Step 3: re-mark frame 0 (null pointer guard)
    if (!bitmap_test(0)) {
        bitmap_set(0);
        free_frames--;
    }

    // Step 4: re-mark kernel image frames
    uint32_t k_start_frame = 0x100000 / PMM_PAGE_SIZE;
    uint32_t k_end_frame   = ((uint32_t)&kernel_end + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE;

    for (uint32_t f = k_start_frame; f < k_end_frame; f++) {
        if (!bitmap_test(f)) {
            bitmap_set(f);
            free_frames--;
        }
    }

    // Step 5: re-mark the bitmap itself
    uint32_t bm_start = (uint32_t)bitmap;
    uint32_t bm_end   = bm_start + sizeof(bitmap);
    uint32_t bm_first = bm_start / PMM_PAGE_SIZE;
    uint32_t bm_last  = (bm_end + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE;

    for (uint32_t f = bm_first; f < bm_last; f++) {
        if (!bitmap_test(f)) {
            bitmap_set(f);
            free_frames--;
        }
    }

    klog_info("PMM: %d KiB free (%d frames)", (free_frames * PMM_PAGE_SIZE) / 1024, free_frames);
}

uint32_t pmm_alloc_frame(void)
{
    spinlock_acquire(&pmm_lock);

    for (uint32_t i = 0; i < BITMAP_SIZE; i++) {
        if (bitmap[i] == 0xFFFFFFFF)
            continue;

        for (uint32_t bit = 0; bit < 32; bit++) {
            uint32_t frame = i * 32 + bit;
            if (!bitmap_test(frame)) {
                bitmap_set(frame);
                free_frames--;
                spinlock_release(&pmm_lock);
                return frame * PMM_PAGE_SIZE;
            }
        }
    }

    spinlock_release(&pmm_lock);
    return 0;  // out of memory
}

void pmm_free_frame(uint32_t addr)
{
    if (addr == 0)
        panic("pmm_free_frame: attempted to free null frame");

    uint32_t frame = addr / PMM_PAGE_SIZE;

    if (frame >= PMM_MAX_FRAMES)
        panic("pmm_free_frame: address out of range: %x", addr);

    spinlock_acquire(&pmm_lock);

    if (bitmap_test(frame) == 0) {
        spinlock_release(&pmm_lock);
        panic("pmm_free_frame: double free at %x", addr);
    }

    bitmap_clear(frame);
    free_frames++;

    spinlock_release(&pmm_lock);
}

void pmm_dump(void)
{
    spinlock_acquire(&pmm_lock);
    uint32_t used = total_frames - free_frames;
    uint32_t free = free_frames;
    spinlock_release(&pmm_lock);

    klog_info("PMM: total=%d free=%d used=%d (%d KiB used)",
        total_frames, free, used, (used * PMM_PAGE_SIZE) / 1024);
}