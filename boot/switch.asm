bits 32

; void switch_to(task_t* prev, task_t* next);
;
; Calling convention (cdecl):
;   [esp+4]  = prev  (task_t*)
;   [esp+8]  = next  (task_t*)
;
; task_t layout (must match task.h exactly):
;   offset 0  — esp        (uint32_t)
;   offset 4  — stack_top  (uint32_t)
;   offset 8  — stack_base (uint8_t*)
;   offset 12 — tid        (uint32_t)
;   offset 16 — state      (task_state_t / uint32_t)
;   offset 20 — page_dir   (uint32_t*)
;   offset 24 — next       (struct task*)
;
; Only task_t.esp (offset 0) is read/written here.

global switch_to
extern tss_set_kernel_stack

section .text

switch_to:
    ; --- save caller-saved state onto prev's kernel stack ---
    pushf                       ; save EFLAGS (preserves IF state of prev task)
    pusha                       ; save EAX ECX EDX EBX ESP EBP ESI EDI

    ; prev is at [esp + 36]: 8 dwords (pusha) + 1 dword (pushf) = 9 pushes = 36 bytes,
    ; plus the return address already on the stack when switch_to was called = +4,
    ; so prev is at [esp + 40] and next at [esp + 44].
    mov eax, [esp + 40]         ; eax = prev
    mov [eax], esp              ; prev->esp = esp

    ; --- update TSS so the CPU knows next task's kernel stack ---
    mov ebx, [esp + 44]         ; ebx = next
    push dword [ebx + 4]        ; push next->stack_top
    call tss_set_kernel_stack
    add esp, 4

    ; --- switch to next task's stack and restore its state ---
    mov ebx, [esp + 44]         ; reload next (tss call may have clobbered ebx via cdecl)
    mov esp, [ebx]              ; esp = next->esp

    popa                        ; restore EDI ESI EBP (skip ESP) EBX EDX ECX EAX
    popf                        ; restore EFLAGS (restores IF for next task)
    ret                         ; jump to return address on next's stack (entry_point on first run)

; Mark stack non-executable.
section .note.GNU-stack noalloc noexec nowrite progbits