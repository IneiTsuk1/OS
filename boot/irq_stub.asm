bits 32

global irq0
global irq1
global irq2
global irq3
global irq4
global irq5
global irq6
global irq7
global irq8
global irq9
global irq10
global irq11
global irq12
global irq13
global irq14
global irq15

extern irq_handler
extern scheduler_need_reschedule
extern scheduler_switch_from_irq
extern scheduler_current_page_dir_phys

; Physical address of the kernel page directory.
; Needed to switch CR3 on IRQ entry from a user task.
; Exposed by paging.c.
extern paging_get_kernel_page_dir_phys

irq0:  cli
    push dword 0
    push dword 32
    jmp irq_common

irq1:  cli
    push dword 0
    push dword 33
    jmp irq_common

irq2:  cli
    push dword 0
    push dword 34
    jmp irq_common

irq3:  cli
    push dword 0
    push dword 35
    jmp irq_common

irq4:  cli
    push dword 0
    push dword 36
    jmp irq_common

irq5:  cli
    push dword 0
    push dword 37
    jmp irq_common

irq6:  cli
    push dword 0
    push dword 38
    jmp irq_common

irq7:  cli
    push dword 0
    push dword 39
    jmp irq_common

irq8:  cli
    push dword 0
    push dword 40
    jmp irq_common

irq9:  cli
    push dword 0
    push dword 41
    jmp irq_common

irq10: cli
    push dword 0
    push dword 42
    jmp irq_common

irq11: cli
    push dword 0
    push dword 43
    jmp irq_common

irq12: cli
    push dword 0
    push dword 44
    jmp irq_common

irq13: cli
    push dword 0
    push dword 45
    jmp irq_common

irq14: cli
    push dword 0
    push dword 46
    jmp irq_common

irq15: cli
    push dword 0
    push dword 47
    jmp irq_common

irq_common:
    pusha
    push ds

    ; Switch to kernel segments
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    ; --- CR3 switch on entry ------------------------------------------------
    ; If we interrupted a user task (CS on the iret frame has RPL=3) the CPU
    ; is already running with the user page directory in CR3.  Switch to the
    ; kernel page directory so all kernel code and data is accessible.
    ;
    ; The saved CS is at a fixed offset above esp:
    ;   esp+0  = ds  (just pushed)
    ;   esp+4  = edi esi ebp esp_dummy ebx edx ecx eax  (pusha = 8 dwords = 32 bytes)
    ;   esp+36 = int_no
    ;   esp+40 = err_code  (always 0 for IRQs)
    ;   esp+44 = eip       (pushed by CPU)
    ;   esp+48 = cs        <-- here
    ;   esp+52 = eflags
    ;   (esp+56 = user esp, esp+60 = user ss — only present on ring-3 -> ring-0)
    ;
    mov eax, [esp + 48]         ; load saved CS
    and eax, 3                  ; isolate RPL bits
    jz .no_entry_cr3_switch     ; RPL=0 means we came from kernel — no switch needed

    call paging_get_kernel_page_dir_phys
    mov cr3, eax                ; load kernel page directory

.no_entry_cr3_switch:

    ; --- call C IRQ handler -------------------------------------------------
    push esp
    call irq_handler
    add esp, 4

    ; --- preemptive reschedule hook -----------------------------------------
    call scheduler_need_reschedule
    test eax, eax
    jz .no_switch

    mov eax, esp
    push eax
    call scheduler_switch_from_irq
    add esp, 4
    mov esp, eax                ; may now be a different task's stack

.no_switch:

    ; --- CR3 switch on exit -------------------------------------------------
    ; Load the current task's page directory before iret.
    ; If the task we're returning to is a kernel task this is a no-op
    ; (kernel tasks have page_dir_phys == kernel page dir phys).
    ; If it's a user task this switches to its address space.
    call scheduler_current_page_dir_phys
    mov cr3, eax

    ; Restore segment registers.
    ; If returning to ring-3 (saved CS RPL=3), load USER_DATA_SEL (0x2B) into
    ; es/fs/gs.  Forcing 0x10 (DPL=0) here and then iret-ing to CPL=3 causes
    ; the CPU to null those registers, producing a #GP on the first user memory access.
    ;
    ; Stack layout at this point (ds still on stack, before pop):
    ;   esp+0      ds
    ;   esp+4..35  pusha frame (8 dwords)
    ;   esp+36     int_no
    ;   esp+40     err_code
    ;   esp+44     eip
    ;   esp+48     cs        <- check RPL here (same offset as entry CR3 check)
    ;   esp+52     eflags
    ;   esp+56     user_esp  (ring-3 -> ring-0 transition only)
    ;   esp+60     user_ss   (ring-3 -> ring-0 transition only)
    mov eax, [esp + 48]         ; saved CS
    and eax, 3                  ; isolate RPL
    pop ds
    jz .irq_kernel_segs

    mov ax, 0x2B                ; USER_DATA_SEL (0x28 | RPL=3)
    mov es, ax
    mov fs, ax
    mov gs, ax
    jmp .irq_segs_done

.irq_kernel_segs:
    mov ax, 0x10
    mov es, ax
    mov fs, ax
    mov gs, ax

.irq_segs_done:
    popa
    add esp, 8                  ; discard int_no + err_code
    iret

section .note.GNU-stack noalloc noexec nowrite progbits