#include "paging.h"
#include "pmm.h"
#include "klog.h"
#include "spinlock.h"
#include "panic.h"
#include <stdint.h>
#include <stddef.h>

extern uint8_t kernel_end;

// The kernel page directory — 4 KiB aligned, one entry per 4 MiB region.
static uint32_t page_dir[1024] __attribute__((aligned(4096)));

static spinlock_t paging_lock = SPINLOCK_INIT;

// ---- physical ↔ virtual translation ----------------------------------------

uint32_t paging_get_phys(uint32_t virt)
{
    uint32_t pd_idx = virt >> 22;
    uint32_t pt_idx = (virt >> 12) & 0x3FF;

    if (!(page_dir[pd_idx] & PAGE_PRESENT))
        panic("paging_get_phys: PDE not present for virt=%x", virt);

    uint32_t* pt = (uint32_t*)paging_phys_to_virt(page_dir[pd_idx] & ~0xFFFu);

    if (!(pt[pt_idx] & PAGE_PRESENT))
        panic("paging_get_phys: PTE not present for virt=%x", virt);

    return (pt[pt_idx] & ~0xFFFu) | (virt & 0xFFFu);
}

uint32_t paging_phys_to_virt(uint32_t phys)
{
    return phys;
}

uint32_t* paging_get_kernel_page_dir(void)
{
    return page_dir;
}

uint32_t paging_get_kernel_page_dir_phys(void)
{
    // Under identity mapping the virtual address of page_dir equals its
    // physical address. paging_get_phys() would work too but calling it on
    // a static array address before paging is fully up is fragile — under
    // identity mapping this is always correct.
    return (uint32_t)page_dir;
}

// ---- helpers ---------------------------------------------------------------

static void paging_enable(void)
{
    uint32_t cr0;
    __asm__ volatile ("mov %0, %%cr3" :: "r"((uint32_t)page_dir));
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= (1u << 31);
    __asm__ volatile ("mov %0, %%cr0" :: "r"(cr0));
}

// ---- public API ------------------------------------------------------------

void paging_flush_tlb(uint32_t virt)
{
    __asm__ volatile ("invlpg (%0)" :: "r"(virt) : "memory");
}

void paging_map(uint32_t virt, uint32_t phys, uint32_t flags)
{
    spinlock_acquire(&paging_lock);

    uint32_t pd_idx = virt >> 22;
    uint32_t pt_idx = (virt >> 12) & 0x3FF;

    if (!(page_dir[pd_idx] & PAGE_PRESENT)) {
        uint32_t pt_phys = pmm_alloc_frame();
        if (!pt_phys)
            panic("paging_map: OOM allocating page table for virt=%x", virt);

        uint32_t* pt_virt = (uint32_t*)paging_phys_to_virt(pt_phys);
        for (int i = 0; i < 1024; i++)
            pt_virt[i] = 0;

        page_dir[pd_idx] = pt_phys | PAGE_PRESENT | PAGE_WRITABLE;
    }

    // The CPU enforces: a user-mode access is permitted only when PAGE_USER is
    // set in BOTH the PDE and the PTE.  If the caller requests PAGE_USER, OR it
    // into the existing PDE so that previously-mapped kernel pages in the same
    // 4 MiB region don't accidentally block user access to this new PTE.
    if (flags & PAGE_USER)
        page_dir[pd_idx] |= PAGE_USER;

    uint32_t  pt_phys = page_dir[pd_idx] & ~0xFFFu;
    uint32_t* pt_virt = (uint32_t*)paging_phys_to_virt(pt_phys);
    pt_virt[pt_idx] = (phys & ~0xFFFu) | (flags | PAGE_PRESENT);

    spinlock_release(&paging_lock);
}

// Map a single page into an arbitrary page directory (by physical address).
// Used by the ELF loader before the new task is scheduled.
// No spinlock — caller must ensure exclusive access to dir_phys.
// Returns 0 on success, -1 on OOM.
int paging_map_into(uint32_t dir_phys, uint32_t virt,
                    uint32_t phys, uint32_t flags)
{
    uint32_t* dir_virt = (uint32_t*)paging_phys_to_virt(dir_phys);

    uint32_t pd_idx = virt >> 22;
    uint32_t pt_idx = (virt >> 12) & 0x3FF;

    if (!(dir_virt[pd_idx] & PAGE_PRESENT)) {
        uint32_t pt_phys = pmm_alloc_frame();
        if (!pt_phys)
            return -1;

        uint32_t* pt_virt = (uint32_t*)paging_phys_to_virt(pt_phys);
        for (int i = 0; i < 1024; i++)
            pt_virt[i] = 0;

        dir_virt[pd_idx] = pt_phys | PAGE_PRESENT | PAGE_WRITABLE;
    }

    // Propagate PAGE_USER into the PDE so the CPU permits ring-3 access.
    if (flags & PAGE_USER)
        dir_virt[pd_idx] |= PAGE_USER;

    uint32_t  pt_phys = dir_virt[pd_idx] & ~0xFFFu;
    uint32_t* pt_virt = (uint32_t*)paging_phys_to_virt(pt_phys);
    pt_virt[pt_idx] = (phys & ~0xFFFu) | (flags | PAGE_PRESENT);

    return 0;
}

void paging_unmap(uint32_t virt)
{
    spinlock_acquire(&paging_lock);

    uint32_t pd_idx = virt >> 22;
    uint32_t pt_idx = (virt >> 12) & 0x3FF;

    if (!(page_dir[pd_idx] & PAGE_PRESENT))
        panic("paging_unmap: PDE not present for virt=%x", virt);

    uint32_t  pt_phys = page_dir[pd_idx] & ~0xFFFu;
    uint32_t* pt_virt = (uint32_t*)paging_phys_to_virt(pt_phys);

    if (!(pt_virt[pt_idx] & PAGE_PRESENT))
        panic("paging_unmap: PTE not present for virt=%x", virt);

    pt_virt[pt_idx] = 0;
    paging_flush_tlb(virt);

    int empty = 1;
    for (int i = 0; i < 1024; i++) {
        if (pt_virt[i] & PAGE_PRESENT) {
            empty = 0;
            break;
        }
    }

    if (empty) {
        pmm_free_frame(pt_phys);
        page_dir[pd_idx] = 0;
        paging_flush_tlb(virt);
    }

    spinlock_release(&paging_lock);
}

// ---- user address space ----------------------------------------------------

// The kernel owns PDEs 0 .. KERNEL_PDE_COUNT-1 (the identity-mapped region).
// Everything from that PDE index upward in a user directory is user-space.
// We compute this from kernel_end rounded up to the nearest 4 MiB boundary
// so we copy exactly the PDEs the kernel actually uses and no more.
//
// Each PDE covers 4 MiB (1024 pages * 4 KiB).
#define PDE_KERNEL_END  (((uint32_t)&kernel_end + (4*1024*1024) - 1) / (4*1024*1024))

uint32_t paging_create_user_dir(void)
{
    // Allocate one physical frame for the new page directory.
    uint32_t dir_phys = pmm_alloc_frame();
    if (!dir_phys)
        return 0;

    uint32_t* dir_virt = (uint32_t*)paging_phys_to_virt(dir_phys);

    // Zero the entire directory first.
    for (int i = 0; i < 1024; i++)
        dir_virt[i] = 0;

    // Copy kernel PDEs so the kernel's identity-mapped region is visible from
    // this address space. User tasks need to reach kernel code and data during
    // syscalls without switching CR3.
    // We copy the PDEs by value — they point to the kernel's page tables which
    // are permanently mapped; no reference counting needed for M5.
    spinlock_acquire(&paging_lock);
    for (uint32_t i = 0; i < PDE_KERNEL_END; i++)
        dir_virt[i] = page_dir[i];
    spinlock_release(&paging_lock);

    return dir_phys;
}

void paging_destroy_user_dir(uint32_t dir_phys)
{
    if (!dir_phys)
        return;

    uint32_t* dir_virt = (uint32_t*)paging_phys_to_virt(dir_phys);

    // Walk every PDE above the kernel region — those are user-space entries.
    // Free each present page table and all present pages within it.
    // Kernel PDEs (below PDE_KERNEL_END) are shared with the kernel directory
    // and must NOT be freed here.
    for (uint32_t pd_idx = PDE_KERNEL_END; pd_idx < 1024; pd_idx++) {
        if (!(dir_virt[pd_idx] & PAGE_PRESENT))
            continue;

        uint32_t  pt_phys = dir_virt[pd_idx] & ~0xFFFu;
        uint32_t* pt_virt = (uint32_t*)paging_phys_to_virt(pt_phys);

        // Free each mapped user page frame.
        for (int pt_idx = 0; pt_idx < 1024; pt_idx++) {
            if (pt_virt[pt_idx] & PAGE_PRESENT) {
                uint32_t frame = pt_virt[pt_idx] & ~0xFFFu;
                pmm_free_frame(frame);
                pt_virt[pt_idx] = 0;
            }
        }

        // Free the page table frame itself.
        pmm_free_frame(pt_phys);
        dir_virt[pd_idx] = 0;
    }

    // Free the directory frame itself.
    pmm_free_frame(dir_phys);

    // Flush the entire TLB by reloading CR3 with the kernel page directory.
    // The destroyed user directory may have shared PDE entries with the kernel
    // directory; stale TLB entries from freed page tables must not be served.
    uint32_t cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile ("mov %0, %%cr3" :: "r"(cr3));
}

// ---- user pointer validation -----------------------------------------------

// Returns 1 if `virt` is mapped present and accessible to user mode (PAGE_USER
// set in both the PDE and PTE) in the page directory at physical address
// `dir_phys`.  Returns 0 otherwise.
//
// Used by syscall handlers to validate pointers passed from ring-3 before
// dereferencing them in kernel mode.  A string validation wrapper
// (paging_user_str_ok) builds on this to check every page a string spans.
//
// This is a read-only walk — no locks needed because the caller holds no
// page-directory lock, and we never modify any entry.  The directory is
// accessed via paging_phys_to_virt (identity mapping), so the virtual
// address of the directory frame equals its physical address.
int paging_user_ptr_ok(uint32_t dir_phys, uint32_t virt)
{
    if (!dir_phys)
        return 0;

    uint32_t* dir_virt = (uint32_t*)paging_phys_to_virt(dir_phys);

    uint32_t pd_idx = virt >> 22;
    uint32_t pt_idx = (virt >> 12) & 0x3FF;

    uint32_t pde = dir_virt[pd_idx];
    if (!(pde & PAGE_PRESENT) || !(pde & PAGE_USER))
        return 0;

    uint32_t* pt_virt = (uint32_t*)paging_phys_to_virt(pde & ~0xFFFu);
    uint32_t  pte     = pt_virt[pt_idx];

    return (pte & PAGE_PRESENT) && (pte & PAGE_USER);
}

// Returns 1 if every byte of the null-terminated string starting at `virt`
// (up to `max_len` bytes including the terminator) lies in a user-accessible
// mapped page in directory `dir_phys`.  Returns 0 if any page check fails or
// the string is not null-terminated within `max_len` bytes.
//
// `max_len` caps the scan so a malicious (or buggy) user task cannot make the
// kernel walk unbounded memory.  4096 bytes is a reasonable default for
// SYS_WRITE messages.
int paging_user_str_ok(uint32_t dir_phys, uint32_t virt, uint32_t max_len)
{
    for (uint32_t i = 0; i < max_len; i++) {
        uint32_t addr = virt + i;

        // Check the page on every page boundary and for the very first byte.
        if (i == 0 || (addr & 0xFFF) == 0) {
            if (!paging_user_ptr_ok(dir_phys, addr))
                return 0;
        }

        if (*(const char*)addr == '\0')
            return 1;
    }

    return 0;   // no null terminator found within max_len bytes
}

void paging_init(void)
{
    for (int i = 0; i < 1024; i++)
        page_dir[i] = 0;

    // kernel_text_end marks the boundary between the read-execute region
    // (.multiboot + .text + .rodata) and the read-write region (.data + .bss).
    // Declared in linker.ld.
    extern uint8_t kernel_text_end;

    uint32_t text_end = ((uint32_t)&kernel_text_end + PMM_PAGE_SIZE - 1)
                        & ~(PMM_PAGE_SIZE - 1);
    uint32_t map_end  = ((uint32_t)&kernel_end + PMM_PAGE_SIZE - 1)
                        & ~(PMM_PAGE_SIZE - 1);

    paging_map(0, 0, PAGE_PRESENT);  // read-only null guard (no PAGE_USER — stays kernel-only)

    uint32_t pages_mapped = 1;

    // .text and .rodata: map PAGE_USER so that ring-3 user_task (kernel-resident
    // code, M5 limitation) can fetch instructions.  No PAGE_WRITABLE — read/execute only.
    // PAGE_USER in the PDE is set automatically by paging_map when it sees PAGE_USER here.
    for (uint32_t addr = PMM_PAGE_SIZE; addr < text_end; addr += PMM_PAGE_SIZE) {
        paging_map(addr, addr, PAGE_PRESENT | PAGE_USER);
        pages_mapped++;
    }

    // .data, .bss, heap region: kernel-private, writable, no user access.
    for (uint32_t addr = text_end; addr < map_end; addr += PMM_PAGE_SIZE) {
        paging_map(addr, addr, PAGE_PRESENT | PAGE_WRITABLE);
        pages_mapped++;
    }

    klog_info("Paging: identity mapped %u pages (0x00000000 - %x, text_end=%x)",
              pages_mapped, map_end, text_end);

    paging_enable();

    klog_info("Paging: MMU enabled.");
}