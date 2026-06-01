#include <stdint.h>
#include "config.h"
#include "kernel.h"
#include "idt.h"
#include "klog.h"
#include "mmap.h"
#include "pmm.h"
#include "paging.h"
#include "kheap.h"
#include "tss.h"
#include "irq.h"
#include "pic.h"
#include "task.h"
#include "scheduler.h"
#include "syscall.h"
#include "vfs/vfs.h"
#include "../drivers/ata/ata.h"
#include "../drivers/fat32/fat32.h"
#include "../drivers/timer/pit.h"
#include "../drivers/keyboard/keyboard.h"
#include "shell.h"
#include "panic.h"
#include "../drivers/serial/serial.h"
#include "../drivers/vga/terminal.h"

// ---- kernel entry point ----------------------------------------------------

extern uint8_t stack_top;  // top of the boot stack, defined in kernel_entry.asm

void kernel_main(uint32_t mb2_info)
{
    serial_init();
    terminal_init();

    klog_info("MyOs %s", "Kernel booting...");
    klog_info("Kernel version %d.%d", KERNEL_VERSION_MAJOR, KERNEL_VERSION_MINOR);

    idt_init();
    klog_info("IDT initialized.");

    mmap_init(mb2_info);
    mmap_dump();
    klog_info("Memory map initialized.");

    pmm_init();
    pmm_dump();
    klog_info("Physical memory manager initialized.");

    paging_init();
    klog_info("Paging initialized.");

    kheap_init();
    klog_info("Kernel heap initialized.");

    tss_init((uint32_t)&stack_top);
    klog_info("TSS initialized.");

    irq_install();
    klog_info("IRQ handlers installed.");

    pit_init(1000);
    keyboard_init();
    klog_info("PIT and keyboard initialized.");

    syscall_init();
    klog_info("Syscall gate installed.");

    ata_init();
    klog_info("ATA driver initialized.");

    // Attempt to mount FAT32 on primary master (drive 0).
    // On QEMU, attach a disk image with: -drive file=disk.img,format=raw
    static vfs_ops_t fat32_ops;
    vfs_init();
    if (fat32_init(ATA_PRIMARY_MASTER, &fat32_ops) == 0) {
        if (vfs_mount(&fat32_ops) == 0)
            klog_info("VFS: FAT32 mounted on drive 0.");
        else
            klog_warn("VFS: mount failed.");
    } else {
        klog_warn("VFS: no FAT32 volume found on drive 0: - filesystem unavailable.");
    }

    // Scheduler must be initialised before sti so that the first PIT tick
    // doesn't call scheduler_tick() with current == NULL.
    scheduler_init();
    klog_info("Scheduler initialized.");

    task_t* tsh = task_create(shell_task);
    scheduler_add(tsh);
    klog_info("Shell task created.");

#ifdef DEBUG_EXCEPTIONS
    __asm__ volatile (
        "mov $0, %ecx\n\t"
        "mov $1, %eax\n\t"
        "div %ecx\n\t"
    );
#endif

    klog_info("Kernel booted successfully. Entering idle loop.");

    __asm__ volatile ("sti");
    while (1) {
        __asm__ volatile ("hlt");
    }
}