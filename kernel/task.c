#include "task.h"
#include "kheap.h"
#include "paging.h"
#include "pmm.h"
#include "panic.h"
#include "klog.h"
#include "elf.h"
#include "vfs/vfs.h"
#include <stdint.h>
#include <stddef.h>

static uint32_t next_tid = 0;

// ---- stack frame layout ----------------------------------------------------
//
// All tasks — both kernel and user — are first entered via the IRQ preemption
// path in irq_stub.asm.  That stub unwinds:
//
//   pop ds
//   popa                  ; edi esi ebp (skip esp) ebx edx ecx eax
//   add esp, 8            ; discard int_no + err_code
//   iret                  ; ring-0->ring-0: pops eip, cs, eflags (3 dwords)
//                         ; ring-0->ring-3: pops eip, cs, eflags, esp, ss (5 dwords)
//
// Kernel task initial frame (ring-0 iret — 3-dword iret block):
//
//   [ eflags  ]   IF=1
//   [ cs      ]   0x08 kernel code
//   [ eip     ]   entry point
//   [ 0       ]   err_code  (discarded)
//   [ 0       ]   int_no    (discarded)
//   [ 0..0    ]   popa frame (8 dwords)
//   [ 0x10    ]   ds
//   ^ task->esp
//
// User task initial frame (ring-3 iret — 5-dword iret block):
//
//   [ ss      ]   0x2B  (USER_DATA | 3)
//   [ esp3    ]   USER_STACK_TOP
//   [ eflags  ]   IF=1
//   [ cs      ]   0x23  (USER_CODE | 3)
//   [ eip     ]   entry point
//   [ 0       ]   err_code  (discarded)
//   [ 0       ]   int_no    (discarded)
//   [ 0..0    ]   popa frame (8 dwords)
//   [ 0x10    ]   ds        (kernel ds — stubs reload ds on every entry)
//   ^ task->esp

#define EFLAGS_IF    0x00000202u  // IF=1 + reserved bit 1
#define USER_CODE_SEL 0x23u       // 0x20 | RPL=3
#define USER_DATA_SEL 0x2Bu       // 0x28 | RPL=3

// ---- shared stack-seeding helper -------------------------------------------

// Seeds the common lower part of the initial IRQ frame onto *sp:
//   ds | edi..eax (8 dwords) | int_no=0 | err_code=0
// Returns the updated stack pointer.
static uint32_t* seed_common_frame(uint32_t* sp)
{
    // popa frame: pushed in pusha order (eax ecx edx ebx esp ebp esi edi)
    // popa restores in reverse: edi esi ebp skip-esp ebx edx ecx eax
    *--sp = 0;      // eax
    *--sp = 0;      // ecx
    *--sp = 0;      // edx
    *--sp = 0;      // ebx
    *--sp = 0;      // esp (dummy — popa skips this slot)
    *--sp = 0;      // ebp
    *--sp = 0;      // esi
    *--sp = 0;      // edi

    // discarded by "add esp, 8" in irq_stub
    *--sp = 0;      // err_code
    *--sp = 0;      // int_no

    return sp;
}

// ---- public API ------------------------------------------------------------

task_t* task_create(void (*entry)(void))
{
    task_t* task = (task_t*)kmalloc(sizeof(task_t));
    if (!task)
        panic("task_create: OOM allocating task struct");

    // Zero the entire struct so fds[] and all fields start clean.
    // kmalloc does not guarantee zero-initialisation for reused heap blocks.
    for (uint32_t i = 0; i < sizeof(task_t); i++)
        ((uint8_t*)task)[i] = 0;

    uint8_t* stack = (uint8_t*)kmalloc(TASK_STACK_SIZE);
    if (!stack)
        panic("task_create: OOM allocating kernel stack");

    task->stack_base      = stack;
    task->stack_top       = (uint32_t)(stack + TASK_STACK_SIZE);
    task->tid             = next_tid++;
    task->state           = TASK_READY;
    task->wake_tick       = 0;
    task->page_dir        = paging_get_kernel_page_dir();
    task->page_dir_phys   = paging_get_phys((uint32_t)paging_get_kernel_page_dir());
    task->is_user         = 0;
    task->user_stack_virt = 0;
    task->waiter          = NULL;
    task->next            = NULL;
    uint32_t* sp = (uint32_t*)task->stack_top;

    // iret frame (ring-0: eip/cs/eflags only)
    *--sp = EFLAGS_IF;
    *--sp = 0x08;               // kernel CS
    *--sp = (uint32_t)entry;    // eip

    sp = seed_common_frame(sp);

    // ds — kernel data segment
    *--sp = 0x10;

    task->esp = (uint32_t)sp;

    // Populate fds 0/1/2 with terminal pseudo-files (stdin/stdout/stderr).
    vfs_setup_stdio(task);

    klog_info("Task %u created (kernel): entry=%x stack=%x-%x",
        task->tid, (uint32_t)entry,
        task->stack_top - TASK_STACK_SIZE, task->stack_top);

    return task;
}

task_t* task_create_user(void (*entry)(void))
{
    task_t* task = (task_t*)kmalloc(sizeof(task_t));
    if (!task)
        panic("task_create_user: OOM allocating task struct");

    for (uint32_t i = 0; i < sizeof(task_t); i++)
        ((uint8_t*)task)[i] = 0;

    // Kernel stack — used whenever this task is in kernel mode (syscalls, IRQs)
    uint8_t* stack = (uint8_t*)kmalloc(TASK_STACK_SIZE);
    if (!stack)
        panic("task_create_user: OOM allocating kernel stack");

    // Per-process page directory
    uint32_t dir_phys = paging_create_user_dir();
    if (!dir_phys)
        panic("task_create_user: OOM allocating page directory");

    // User stack — one page mapped at USER_STACK_VIRT in the new directory.
    // We temporarily switch to the new directory to call paging_map, then
    // switch back.  Actually paging_map operates on the *kernel* page_dir
    // global — we need a version that takes an explicit directory.
    // For M5 simplicity: map the user stack into the KERNEL directory too,
    // then copy that PDE into the user directory.
    //
    // Cleaner approach: allocate the frame here and write the PTE directly
    // into the user directory without going through paging_map.
    uint32_t user_stack_frame = pmm_alloc_frame();
    if (!user_stack_frame)
        panic("task_create_user: OOM allocating user stack frame");

    // Write the PTE directly into the user page directory.
    // USER_STACK_VIRT = 0xBFFFF000 -> pd_idx = 0x2FF = 767, pt_idx = 0x3FF = 1023
    uint32_t  pd_idx   = USER_STACK_VIRT >> 22;
    uint32_t  pt_idx   = (USER_STACK_VIRT >> 12) & 0x3FF;
    uint32_t* dir_virt = (uint32_t*)paging_phys_to_virt(dir_phys);

    // Allocate a page table for the user stack region if not present
    if (!(dir_virt[pd_idx] & PAGE_PRESENT)) {
        uint32_t pt_phys = pmm_alloc_frame();
        if (!pt_phys)
            panic("task_create_user: OOM allocating user stack page table");

        uint32_t* pt_virt = (uint32_t*)paging_phys_to_virt(pt_phys);
        for (int i = 0; i < 1024; i++)
            pt_virt[i] = 0;

        dir_virt[pd_idx] = pt_phys | PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER;
    }

    uint32_t  pt_phys  = dir_virt[pd_idx] & ~0xFFFu;
    uint32_t* pt_virt  = (uint32_t*)paging_phys_to_virt(pt_phys);
    pt_virt[pt_idx]    = user_stack_frame | PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER;

    task->stack_base      = stack;
    task->stack_top       = (uint32_t)(stack + TASK_STACK_SIZE);
    task->tid             = next_tid++;
    task->state           = TASK_READY;
    task->wake_tick       = 0;
    task->page_dir        = (uint32_t*)dir_phys;  // physical — used for CR3
    task->page_dir_phys   = dir_phys;
    task->is_user         = 1;
    task->user_stack_virt = USER_STACK_VIRT;
    task->waiter          = NULL;
    task->next            = NULL;

    // Seed initial IRQ frame — ring-3 iret (5-dword iret block)
    uint32_t* sp = (uint32_t*)task->stack_top;

    // iret frame (ring-3: ss/esp/eflags/cs/eip — CPU pops all 5 on privilege change)
    *--sp = USER_DATA_SEL;          // ss3
    *--sp = USER_STACK_TOP;         // esp3  — top of the user stack page
    *--sp = EFLAGS_IF;              // eflags IF=1
    *--sp = USER_CODE_SEL;          // cs3
    *--sp = (uint32_t)entry;        // eip

    sp = seed_common_frame(sp);

    // ds — kernel data segment (irq_stub reloads ds=0x10 on every IRQ entry,
    // and the syscall handler will set ds=USER_DATA_SEL before iret to user)
    *--sp = 0x10;

    task->esp = (uint32_t)sp;

    // Populate fds 0/1/2 with terminal pseudo-files (stdin/stdout/stderr).
    vfs_setup_stdio(task);

    klog_info("Task %u created (user): entry=%x kstack=%x-%x ustack=%x",
        task->tid, (uint32_t)entry,
        task->stack_top - TASK_STACK_SIZE, task->stack_top,
        USER_STACK_VIRT);

    return task;
}

// Create a user-mode task from an already-loaded ELF image.
// Takes ownership of elf->dir_phys — do NOT call paging_destroy_user_dir
// on it after this returns successfully.
//
// argc/argv are laid out on the user stack in standard i386 cdecl ABI so
// that _start(int argc, const char** argv) works with no special prologue:
//
//   USER_STACK_TOP (0xC0000000) — top of the mapped page
//     [ argv string data, packed downward from top ]
//     [ 4-byte alignment pad if needed             ]
//     [ NULL                                       ]  argv[argc] sentinel
//     [ ptr to argv[argc-1]                        ]
//     ...
//     [ ptr to argv[0]                             ]  <- argv_array_uptr
//     [ NULL                                       ]  envp (empty)
//     [ argv_array_uptr                            ]  argv argument to _start
//     [ argc                                       ]  argc argument to _start
//     [ 0                                          ]  fake return address
//   <- esp3 points here
//
// The CPU irets to _start with esp3 as the user stack pointer.  _start's
// C prologue then reads [esp+4]=argc, [esp+8]=argv exactly as expected.
task_t* task_create_user_from_elf(elf_load_result_t* elf,
                                   int argc, const char** argv)
{
    task_t* task = (task_t*)kmalloc(sizeof(task_t));
    if (!task)
        return NULL;

    for (uint32_t i = 0; i < sizeof(task_t); i++)
        ((uint8_t*)task)[i] = 0;

    uint8_t* stack = (uint8_t*)kmalloc(TASK_STACK_SIZE);
    if (!stack) {
        kfree(task);
        return NULL;
    }

    // Map the user stack into the ELF's page directory.
    uint32_t user_stack_frame = pmm_alloc_frame();
    if (!user_stack_frame) {
        kfree(stack);
        kfree(task);
        return NULL;
    }

    if (paging_map_into(elf->dir_phys, USER_STACK_VIRT, user_stack_frame,
                        PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER) != 0) {
        pmm_free_frame(user_stack_frame);
        kfree(stack);
        kfree(task);
        return NULL;
    }

    // ---- Build argc/argv on the user stack page ----------------------------
    //
    // kbase: kernel-accessible base of the user stack frame (identity mapped,
    //        so physical addr == kernel virtual addr).
    // ubase: USER_STACK_VIRT — the virtual base the user task sees.
    // All byte offsets into the page are identical in both views.

    uint8_t*  kbase = (uint8_t*)user_stack_frame;
    uint32_t  ubase = USER_STACK_VIRT;

    if (argc < 0) argc = 0;
    if (argc > 64) argc = 64;

    // Pass 1 — copy strings into the page from the top downward.
    // Record the user virtual address of each string.
    uint32_t str_offset = PMM_PAGE_SIZE;   // byte offset from page base, grows down
    uint32_t str_ptrs[64];

    for (int i = 0; i < argc; i++) {
        const char* src = argv[i] ? argv[i] : "";
        uint32_t len = 0;
        while (src[len]) len++;
        len++;  // include null terminator

        if (len > str_offset) len = 1;
        str_offset -= len;

        uint8_t* dst = kbase + str_offset;
        for (uint32_t j = 0; j < len; j++)
            dst[j] = (uint8_t)src[j];

        str_ptrs[i] = ubase + str_offset;
    }

    // Align down to 4 bytes.
    str_offset &= ~3u;

    // Pass 2 — write the argv pointer array (argv[0]..argv[argc-1], NULL).
    // We need (argc + 1) slots for the array itself.
    uint32_t array_slots = (uint32_t)(argc + 1);   // ptrs + NULL sentinel
    str_offset -= array_slots * 4;
    uint32_t* argv_array = (uint32_t*)(kbase + str_offset);
    for (int i = 0; i < argc; i++)
        argv_array[i] = str_ptrs[i];
    argv_array[argc] = 0;   // NULL sentinel

    // User virtual address of the argv[] array.
    uint32_t argv_array_uptr = ubase + str_offset;

    // Pass 3 — write the cdecl call frame:
    //   [0] fake return address
    //   [1] argc
    //   [2] argv  (pointer to the array above)
    //   [3] envp  (NULL — empty environment)
    str_offset -= 4 * 4;
    uint32_t* frame = (uint32_t*)(kbase + str_offset);
    frame[0] = 0;                   // fake return address
    frame[1] = (uint32_t)argc;      // argc
    frame[2] = argv_array_uptr;     // argv
    frame[3] = 0;                   // envp = NULL

    uint32_t user_esp3 = ubase + str_offset;

    // ---- Fill task struct --------------------------------------------------

    task->stack_base      = stack;
    task->stack_top       = (uint32_t)(stack + TASK_STACK_SIZE);
    task->tid             = next_tid++;
    task->state           = TASK_READY;
    task->wake_tick       = 0;
    task->page_dir        = (uint32_t*)elf->dir_phys;
    task->page_dir_phys   = elf->dir_phys;
    task->is_user         = 1;
    task->user_stack_virt = USER_STACK_VIRT;
    task->waiter          = NULL;
    task->next            = NULL;

    // Seed the initial IRQ frame — ring-3 iret (5-dword iret block).
    uint32_t* sp = (uint32_t*)task->stack_top;

    *--sp = USER_DATA_SEL;          // ss3
    *--sp = user_esp3;              // esp3 — points at argc on the user stack
    *--sp = EFLAGS_IF;              // eflags IF=1
    *--sp = USER_CODE_SEL;          // cs3
    *--sp = elf->entry;             // eip — ELF entry point

    sp = seed_common_frame(sp);
    *--sp = 0x10;                   // ds (kernel segment; reloaded on every IRQ entry)

    task->esp = (uint32_t)sp;

    vfs_setup_stdio(task);

    klog_info("Task %u created (ELF user): entry=%x kstack=%x-%x ustack=%x esp3=%x argc=%d",
        task->tid, elf->entry,
        task->stack_top - TASK_STACK_SIZE, task->stack_top,
        USER_STACK_VIRT, user_esp3, argc);

    return task;
}

void task_free(task_t* task)
{
    // Close all open file descriptors before tearing down the address space.
    // Flushes pending writes and frees vfs_file_t heap allocations.
    vfs_task_close_all(task);

    if (task->is_user && task->page_dir_phys) {
        //klog_info("task_free: destroying page dir %x",
        //          task->page_dir_phys);

        paging_destroy_user_dir(task->page_dir_phys);

        //klog_info("task_free: page dir destroyed");
    }

    //klog_info("task_free: freeing kstack %x",
    //          (uint32_t)task->stack_base);

    kfree(task->stack_base);

    //klog_info("task_free: freeing task struct %x",
    //          (uint32_t)task);

    kfree(task);

    //klog_info("task_free: done");
}