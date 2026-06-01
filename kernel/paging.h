#pragma once
#include <stdint.h>

// Page flag bits (PTE/PDE)
#define PAGE_PRESENT    (1u << 0)
#define PAGE_WRITABLE   (1u << 1)
#define PAGE_USER       (1u << 2)

// Initialise paging and enable the MMU.
// Must be called after pmm_init().
void paging_init(void);

// Map a single virtual page to a physical frame in the kernel page directory.
// Allocates a page table via pmm_alloc_frame() if needed.
// Thread-safe.
void paging_map(uint32_t virt, uint32_t phys, uint32_t flags);

// Map a single virtual page into an *arbitrary* page directory (identified by
// its physical address).  Used by the ELF loader to populate a new user address
// space before the task is scheduled for the first time.
// Allocates page tables from the PMM as needed.
// NOT thread-safe with respect to concurrent modifications of the same dir —
// call only while the target directory is not yet loaded into CR3.
// Returns 0 on success, -1 on OOM.
int paging_map_into(uint32_t dir_phys, uint32_t virt,
                    uint32_t phys, uint32_t flags);

// Unmap a single virtual page.
// Clears the PTE, issues invlpg, reclaims empty page tables to PMM.
// Panics if the address was not mapped.
// Thread-safe.
void paging_unmap(uint32_t virt);

// Flush a single TLB entry for the given virtual address.
void paging_flush_tlb(uint32_t virt);

// Translate a virtual address to its backing physical address.
// Panics if the address is not mapped.
uint32_t paging_get_phys(uint32_t virt);

// Convert a physical address to the virtual address the kernel uses to
// access it. Under identity mapping: returns phys unchanged.
uint32_t paging_phys_to_virt(uint32_t phys);

// Return a pointer to the kernel's page directory.
uint32_t* paging_get_kernel_page_dir(void);

// Allocate a fresh page directory for a user process.
// Copies all kernel PDE entries (identity-mapped region) into it so that
// kernel code and syscall handlers work without a CR3 switch.
// Returns the PHYSICAL address of the new directory (what CR3 needs),
// or 0 on OOM.
uint32_t paging_create_user_dir(void);

// Returns the PHYSICAL address of the kernel page directory.
// Used by irq_stub.asm to restore CR3 on IRQ entry from a user task.
uint32_t paging_get_kernel_page_dir_phys(void);

// Destroy a user page directory created by paging_create_user_dir().
// Walks every non-kernel PDE, frees all user pages and their page tables
// back to the PMM, then frees the directory frame itself.
// The kernel PDE entries are NOT freed — they are shared with the kernel dir.
// dir_phys: physical address returned by paging_create_user_dir().
void paging_destroy_user_dir(uint32_t dir_phys);

// Returns 1 if `virt` is mapped present with PAGE_USER set in both the PDE
// and PTE of the page directory at physical address `dir_phys`; 0 otherwise.
// Use this to validate a single pointer page before dereferencing in the kernel.
int paging_user_ptr_ok(uint32_t dir_phys, uint32_t virt);

// Returns 1 if the null-terminated string at `virt` (up to `max_len` bytes
// including the terminator) lies entirely within user-accessible mapped pages
// in directory `dir_phys`; 0 otherwise.
// `max_len` caps the scan — pass 4096 for general syscall string arguments.
int paging_user_str_ok(uint32_t dir_phys, uint32_t virt, uint32_t max_len);