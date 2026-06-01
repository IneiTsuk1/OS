bits 32

global isr0
global isr1
global isr2
global isr3
global isr4
global isr5
global isr6
global isr7
global isr8
global isr9
global isr10
global isr11
global isr12
global isr13
global isr14
global isr15
global isr16
global isr17
global isr18
global isr19
global isr20
global isr21
global isr22
global isr23
global isr24
global isr25
global isr26
global isr27
global isr28
global isr29
global isr30
global isr31
global isr128

extern isr_handler
extern scheduler_need_reschedule
extern scheduler_switch_from_irq
extern scheduler_current_page_dir_phys

; CPU exception error-code matrix (Intel SDM Vol.3 Table 6-1):
;   Pushes error code: 8, 10, 11, 12, 13, 14, 17, 21, 29, 30
;   No error code:     all others
; For no-error-code exceptions we push dummy 0 first so the stack
; frame is always uniform (matching regs_t in isr.h).

isr0:   cli
        push dword 0 ; dummy err_code
        push dword 0
        jmp isr_common

isr1:   cli
        push dword 0
        push dword 1
        jmp isr_common

isr2:   cli
        push dword 0
        push dword 2
        jmp isr_common

isr3:   cli
        push dword 0
        push dword 3
        jmp isr_common

isr4:   cli
        push dword 0
        push dword 4
        jmp isr_common

isr5:   cli
        push dword 0
        push dword 5
        jmp isr_common

isr6:   cli
        push dword 0
        push dword 6
        jmp isr_common

isr7:   cli
        push dword 0
        push dword 7
        jmp isr_common

isr8:   cli
        ; CPU already pushed error code
        push dword 8
        jmp isr_common

isr9:   cli
        push dword 0
        push dword 9
        jmp isr_common

isr10:  cli
        push dword 10
        jmp isr_common

isr11:  cli
        push dword 11
        jmp isr_common

isr12:  cli
        push dword 12
        jmp isr_common

isr13:  cli
        push dword 13
        jmp isr_common

isr14:  cli
        push dword 14
        jmp isr_common

isr15:  cli
        push dword 0
        push dword 15
        jmp isr_common

; Vector 16 — x87 FP Exception (no error code)
isr16:  cli
        push dword 0
        push dword 16
        jmp isr_common

; Vector 17 — Alignment Check (error code)
isr17:  cli
        push dword 17
        jmp isr_common

; Vector 18 — Machine Check (no error code)
isr18:  cli
        push dword 0
        push dword 18
        jmp isr_common

; Vector 19 — SIMD FP Exception (no error code)
isr19:  cli
        push dword 0
        push dword 19
        jmp isr_common

isr20:  cli
        push dword 0
        push dword 20
        jmp isr_common

isr21:  cli
        ; Vector 21 (Control Protection) pushes an error code on newer CPUs.
        push dword 21
        jmp isr_common

isr22:  cli
        push dword 0
        push dword 22
        jmp isr_common

isr23:  cli
        push dword 0
        push dword 23
        jmp isr_common

isr24:  cli
        push dword 0
        push dword 24
        jmp isr_common

isr25:  cli
        push dword 0
        push dword 25
        jmp isr_common

isr26:  cli
        push dword 0
        push dword 26
        jmp isr_common

isr27:  cli
        push dword 0
        push dword 27
        jmp isr_common

isr28:  cli
        push dword 0
        push dword 28
        jmp isr_common

; Vector 29 — VMM Communication Exception (error code)
isr29:  cli
        push dword 29
        jmp isr_common

; Vector 30 — Security Exception (error code)
isr30:  cli
        push dword 30
        jmp isr_common

; Vector 31 — Reserved (no error code)
isr31:  cli
        push dword 0
        push dword 31
        jmp isr_common

; Vector 128 (0x80) — syscall entry point (no error code)
isr128:
    cli
    push dword 0        ; no error code
    push dword 128      ; int_no = 0x80
    jmp isr_common

; ---------------------------------------------------------------------------
; isr_common — shared exception/syscall entry path
;
; Stack on entry (after stubs push int_no and err_code):
;   [cpu iret frame]  eip / cs / eflags  (+ esp3/ss3 if from ring-3)
;   err_code
;   int_no
;                     <- ESP here
;
; After pusha + push ds:
;   esp+0   ds
;   esp+4   edi esi ebp esp_dummy ebx edx ecx eax   (pusha, 8 dwords = 32 bytes)
;   esp+36  int_no
;   esp+40  err_code
;   esp+44  eip
;   esp+48  cs
;   esp+52  eflags
;   esp+56  user_esp  (only when coming from ring-3)
;   esp+60  user_ss   (only when coming from ring-3)
;
; This layout is IDENTICAL to irq_common's frame.  Both paths push ds last
; before calling the C handler, so both leave ds on the stack at esp+0 and
; the saved CS at esp+48 throughout the handler call and the reschedule hook.
;
; After the reschedule hook, esp may point at a DIFFERENT task's frame (one
; that was saved by irq_common on a PIT tick).  That frame has the same layout
; — ds at esp+0, CS at esp+48 — so the exit sequence is identical regardless
; of whether a switch occurred.  We therefore do NOT pop ds before the
; reschedule hook; we pop it as part of the unified exit path below.
; ---------------------------------------------------------------------------

isr_common:
    pusha
    push ds

    mov ax, 0x10    ; kernel data segment
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    ; Call the C handler.  esp+0 = ds (the saved ds we just pushed), which
    ; means the regs_t* passed to isr_handler points one slot ABOVE that —
    ; we push esp so the handler receives a pointer to the full frame.
    push esp
    call isr_handler
    add esp, 4

    ; --- reschedule hook ------------------------------------------------
    ; SYS_EXIT (and future syscalls) may set need_reschedule.  If so, pass
    ; the current ESP — which still points at our frame with ds at esp+0 —
    ; to scheduler_switch_from_irq.  It saves this as prev->esp and returns
    ; next->esp.  We load next->esp into esp; from this point we are
    ; unwinding next's frame, which has the same layout (built by irq_common
    ; or isr_common, both identical).
    call scheduler_need_reschedule
    test eax, eax
    jz .no_switch

    mov eax, esp
    push eax
    call scheduler_switch_from_irq
    add esp, 4
    mov esp, eax        ; switch to next task's frame (ds still at esp+0)

.no_switch:

    ; --- CR3 restore ----------------------------------------------------
    call scheduler_current_page_dir_phys
    mov cr3, eax

    ; --- segment register fixup -----------------------------------------
    ; ds is still on the stack at esp+0.  CS is at esp+48.
    ; If returning to ring-3 (CS RPL=3), load USER_DATA_SEL into es/fs/gs
    ; BEFORE popping ds, so the CPU does not null them on the privilege
    ; change.  We read CS first, then pop ds, then branch.
    mov eax, [esp + 48]     ; saved CS (ds still on stack, so +48 not +44)
    and eax, 3              ; isolate RPL
    pop ds                  ; restore ds now (frame compacted by 4 bytes;
                            ; offsets above are no longer valid after this)
    jz .kernel_segs

    mov ax, 0x2B            ; USER_DATA_SEL (0x28 | RPL=3)
    mov es, ax
    mov fs, ax
    mov gs, ax
    jmp .segs_done

.kernel_segs:
    mov ax, 0x10
    mov es, ax
    mov fs, ax
    mov gs, ax

.segs_done:
    popa
    add esp, 8              ; discard int_no + err_code
    iret

; Mark the stack as non-executable.
section .note.GNU-stack noalloc noexec nowrite progbits