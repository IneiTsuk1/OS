#include "scheduler.h"
#include "task.h"
#include "kheap.h"
#include "tss.h"
#include "klog.h"
#include "panic.h"
#include "paging.h"
#include <stddef.h>

#define SCHEDULER_TICKS  20     // 20 ms time slice at 1000 Hz

// Declared in switch.asm — used only for voluntary yield (non-IRQ context).
extern void switch_to(task_t* prev, task_t* next);

// ---- run queue -------------------------------------------------------------
//
// Circular singly-linked list.
//
// `current`        — the task currently running on the CPU.
// `run_queue_head` — always points to some live node in the list.
//                    Used by pick_next() as the scan start point so that it
//                    never starts from current->next, which is NULL after
//                    scheduler_exit() dequeues current.
//
// The list is never empty after scheduler_init() — the idle task (tid 0) is
// always present and is never removed.

static task_t*   current         = NULL;
static task_t*   run_queue_head  = NULL;   // stable scan start for pick_next
static uint32_t  ticks_left      = SCHEDULER_TICKS;
static int       need_reschedule = 0;

static uint32_t  queue_len       = 0;

// ---- reap queue ------------------------------------------------------------
//
// Tasks that call SYS_EXIT cannot free their own kernel stack — we are still
// executing on it at the point scheduler_exit() is called.  They are placed
// here and freed by reap_drain().
//
// CRITICAL — when reap_drain() may safely be called:
//   reap_drain() calls task_free() which calls kfree(stack_base), writing heap
//   metadata into the freed stack pages.  This is only safe once the CPU's
//   physical ESP register has moved off that stack.
//
//   scheduler_switch_from_irq() returns next->esp as a *value* in EAX; the
//   stub executes "mov esp, eax" only AFTER the C function returns, so the
//   physical ESP still points at prev's stack for the entire duration of
//   scheduler_switch_from_irq().  Calling reap_drain() there corrupts the
//   live stack and causes the subsequent iret to fault.
//
//   The safe site is scheduler_tick(), called at the top of every PIT IRQ.
//   By the time any PIT tick fires, irq_stub has already loaded the new
//   task's ESP from the previous switch, so ESP always belongs to a live task.

#define REAP_QUEUE_SIZE 16
static task_t*  reap_queue[REAP_QUEUE_SIZE];
static uint32_t reap_head = 0;
static uint32_t reap_tail = 0;

static void reap_enqueue(task_t* task)
{
    uint32_t next = (reap_head + 1) % REAP_QUEUE_SIZE;
    if (next == reap_tail) {
        klog_warn("scheduler: reap queue full, leaking tid=%u", task->tid);
        return;
    }
    reap_queue[reap_head] = task;
    reap_head = next;
}

// Drain the reap queue.  ONLY call this when the physical ESP is known to
// belong to a live task — see block comment above.
static void reap_drain(void)
{
    while (reap_tail != reap_head) {
        task_t* dead = reap_queue[reap_tail];
        reap_tail = (reap_tail + 1) % REAP_QUEUE_SIZE;
        klog_info("Scheduler: reaping tid=%u", dead->tid);
        task_free(dead);
    }
}

// ---- run queue helpers -----------------------------------------------------

static void enqueue(task_t* task)
{
    if (!current) {
        task->next     = task;
        current        = task;
        run_queue_head = task;
        queue_len      = 1;
        return;
    }

    // Insert before current so run_queue_head stays valid.
    task_t* tail = current;
    while (tail->next != current)
        tail = tail->next;

    tail->next = task;
    task->next = current;
    queue_len++;
    // run_queue_head is unchanged — it still points to a valid list node.
}

// Remove `task` from the circular run queue.
// Must be called with interrupts disabled.
// Updates run_queue_head if it pointed at the removed node.
// MUST NOT be called when queue_len == 1 (idle task always stays).
static void dequeue(task_t* task)
{
    if (queue_len <= 1)
        panic("scheduler dequeue: attempted to remove last task");

    // Find the predecessor node.
    task_t* pred = task;
    while (pred->next != task)
        pred = pred->next;

    pred->next = task->next;
    task->next = NULL;
    queue_len--;

    // If run_queue_head was pointing at the removed node, advance it to the
    // predecessor (which is still in the list).
    if (run_queue_head == task)
        run_queue_head = pred;
}

// Pick the next runnable task other than current; return NULL if none.
// Starts scanning from run_queue_head so it never touches current->next,
// which is NULL when current has been dequeued by scheduler_exit().
// Called with interrupts disabled.
static task_t* pick_next(void)
{
    if (!run_queue_head)
        return NULL;

    task_t* next = run_queue_head;

    for (uint32_t checked = 0; checked < queue_len; checked++) {
        if (next != current &&
            (next->state == TASK_READY || next->state == TASK_RUNNING))
            return next;
        next = next->next;
    }

    return NULL;
}

static inline uint32_t eflags_read(void)
{
    uint32_t f;
    __asm__ volatile ("pushf; pop %0" : "=r"(f));
    return f;
}

// ---- public API ------------------------------------------------------------

void scheduler_init(void)
{
    extern uint8_t stack_top;

    task_t* idle = (task_t*)kmalloc(sizeof(task_t));
    if (!idle)
        panic("scheduler_init: OOM");

    idle->esp             = 0;
    idle->stack_top       = (uint32_t)&stack_top;
    idle->stack_base      = NULL;
    idle->tid             = 0;
    idle->state           = TASK_RUNNING;
    idle->wake_tick       = 0;
    idle->page_dir        = paging_get_kernel_page_dir();
    idle->page_dir_phys   = paging_get_kernel_page_dir_phys();
    idle->is_user         = 0;
    idle->user_stack_virt = 0;
    idle->next            = idle;

    current        = idle;
    run_queue_head = idle;
    queue_len      = 1;
    ticks_left     = SCHEDULER_TICKS;

    klog_info("Scheduler: initialised (idle tid=0, slice=%d ms)", SCHEDULER_TICKS);
}

// Called from SYS_EXIT (syscall context, interrupts disabled via isr_common).
// Unlinks the current task from the run queue, enqueues it for deferred freeing,
// wakes any task waiting on this one, and sets need_reschedule so isr_common
// switches away before iret.
// Must NOT be called on tid 0 (idle task).
void scheduler_exit(int exit_code)
{
    if (current->tid == 0)
        panic("scheduler_exit: idle task called exit");

    current->exit_code = exit_code;
    current->state     = TASK_DEAD;
    dequeue(current);           // unlink; sets current->next = NULL
                                // run_queue_head advanced off current by dequeue()

    // Wake any task blocked in scheduler_wait() on this tid.
    // Stash our exit code into the waiter's exit_code field so it can be
    // read by scheduler_wait() after waking (before we are reaped).
    if (current->waiter) {
        current->waiter->exit_code = exit_code;
        current->waiter->state     = TASK_READY;
        current->waiter            = NULL;
    }

    reap_enqueue(current);      // defer task_free to scheduler_tick (safe stack)
    need_reschedule = 1;        // isr_common will call scheduler_switch_from_irq
}

// Send signal `sig` to the task with `tid`.
// Sets the corresponding bit in pending_signals.
// If the target is TASK_BLOCKED (sleeping/waiting), wakes it so it can be
// killed promptly on the next scheduler_tick() rather than waiting for its
// sleep to expire.
// Returns 0 on success, -1 if no task with that tid exists.
// Must be called with interrupts disabled (called from syscall context).
int scheduler_kill(uint32_t tid, int sig)
{
    if (sig < 0 || sig > 31)
        return -1;

    task_t* t = run_queue_head;
    for (uint32_t i = 0; i < queue_len; i++) {
        if (t->tid == tid) {
            t->pending_signals |= (1u << sig);
            // Wake blocked tasks so they are scheduled and the signal is
            // delivered promptly in scheduler_tick().
            if (t->state == TASK_BLOCKED) {
                t->state     = TASK_READY;
                t->wake_tick = 0;
                need_reschedule = 1;
            }
            return 0;
        }
        t = t->next;
    }
    return -1;  // ESRCH
}


// Returns the child's exit_code (or 0 if the task was already gone).
// Must be called with interrupts enabled (we hlt while blocked).
int scheduler_wait(uint32_t tid)
{
    uint32_t flags = eflags_read();
    __asm__ volatile ("cli");

    // Walk the run queue looking for the target tid.
    task_t* target = NULL;
    if (run_queue_head) {
        task_t* t = run_queue_head;
        for (uint32_t i = 0; i < queue_len; i++) {
            if (t->tid == tid) {
                target = t;
                break;
            }
            t = t->next;
        }
    }

    if (!target) {
        // Already gone — nothing to wait for.
        if (flags & (1u << 9))
            __asm__ volatile ("sti");
        return 0;
    }

    // Register current as the waiter and block.
    target->waiter  = current;
    current->state  = TASK_BLOCKED;
    need_reschedule = 1;

    if (flags & (1u << 9))
        __asm__ volatile ("sti");

    // Spin-wait with hlt — scheduler_exit() will set us TASK_READY.
    while (current->state == TASK_BLOCKED)
        __asm__ volatile ("hlt");

    // target has been reaped by now; its exit_code was saved before it
    // was enqueued for freeing.  We stashed it in current before being woken.
    // Because the waiter pointer is cleared before wake, we use a local.
    // Actually: we need to read it before reap_drain frees target.
    // scheduler_exit() wakes us synchronously, and reap_drain only runs on
    // the next tick — so target is still valid here for one brief window.
    // We avoid the race by reading the exit code in scheduler_exit() itself
    // via the waiter's exit_code field set there.  Instead we use a simpler
    // approach: stash the child exit code into current->exit_code temporarily.
    int code = current->exit_code; // set by scheduler_exit before waking us
    current->exit_code = 0;        // restore for our own future exit
    return code;
}

void scheduler_add(task_t* task)
{
    if (!task)
        panic("scheduler_add: NULL task");

    uint32_t flags = eflags_read();
    __asm__ volatile ("cli");

    task->state     = TASK_READY;
    task->wake_tick = 0;
    enqueue(task);

    if (flags & (1u << 9))
        __asm__ volatile ("sti");

    klog_info("Scheduler: added task tid=%u", task->tid);
}

// Block the current task until the PIT reaches wake_tick.
// Must be called with interrupts enabled.
void scheduler_sleep(uint32_t wake_tick)
{
    uint32_t flags = eflags_read();
    __asm__ volatile ("cli");

    current->state     = TASK_BLOCKED;
    current->wake_tick = wake_tick;
    need_reschedule    = 1;

    if (flags & (1u << 9))
        __asm__ volatile ("sti");

    while (current->state == TASK_BLOCKED)
        __asm__ volatile ("hlt");
}

// Called from the PIT IRQ handler (interrupts disabled).
// Drains the reap queue first — safe here because by the time any PIT IRQ
// fires, irq_stub has already loaded a live task's ESP from any prior switch.
// Then wakes sleeping tasks and triggers preemption if the slice expired.
void scheduler_tick(void)
{
    if (!current)
        return;

    // --- reap dead tasks ----------------------------------------------------
    // Physical ESP belongs to the interrupted live task, not to anything in
    // the reap queue, so task_free() is safe to call.
    reap_drain();

    // --- wake sleeping tasks ------------------------------------------------
    extern uint32_t pit_get_ticks(void);
    uint32_t now = pit_get_ticks();

    task_t* t = run_queue_head;
    for (uint32_t i = 0; i < queue_len; i++) {
        if (t->state == TASK_BLOCKED && t->wake_tick <= now) {
            t->state     = TASK_READY;
            t->wake_tick = 0;
        }
        t = t->next;
    }

    // --- signal delivery ----------------------------------------------------
    // Check current task for SIGKILL (bit 9).  Only current runs here, so we
    // only need to kill ourselves; other tasks' signals are delivered when they
    // next become current (next tick after being woken by scheduler_kill).
    // Idle task (tid 0) cannot be killed.
    if (current->tid != 0 &&
        (current->pending_signals & (1u << 9))) {
        current->pending_signals &= ~(1u << 9);
        klog_warn("scheduler: SIGKILL delivered to tid=%u", current->tid);
        // exit_code = 128 + 9 = 137 (POSIX convention)
        scheduler_exit(128 + 9);
        // scheduler_exit sets need_reschedule; fall through to preemption check.
    }

    // --- time-slice preemption ----------------------------------------------
    if (--ticks_left > 0)
        return;

    ticks_left = SCHEDULER_TICKS;

    if (pick_next())
        need_reschedule = 1;
}

// Called from irq_stub.asm and isr_stub.asm with the interrupted task's ESP
// (pointing at the top of the saved register frame on that task's stack).
// Saves ESP into current->esp, picks the next runnable task, updates the TSS,
// advances current and run_queue_head, returns next->esp for the stub to load.
// Returns esp unchanged if no switch is needed.
//
// NOTE: reap_drain() must NOT be called here — we are still physically on
// prev's kernel stack until the stub executes "mov esp, eax" after we return.
// Freeing prev's stack here corrupts the live stack.  See scheduler_tick().
uint32_t scheduler_switch_from_irq(uint32_t esp)
{
    need_reschedule = 0;

    task_t* next = pick_next();
    if (!next)
        return esp;

    task_t* prev = current;
    prev->esp = esp;

    if (prev->state == TASK_RUNNING)
        prev->state = TASK_READY;

    next->state    = TASK_RUNNING;
    current        = next;
    run_queue_head = next;      // advance head to the new current for next pick

    tss_set_kernel_stack(next->stack_top);

    return next->esp;
}

// Voluntary yield — called from non-IRQ kernel context only.
void scheduler_yield(void)
{
    uint32_t flags = eflags_read();
    __asm__ volatile ("cli");

    task_t* next = pick_next();
    if (next) {
        task_t* prev = current;
        if (prev->state == TASK_RUNNING)
            prev->state = TASK_READY;
        next->state    = TASK_RUNNING;
        current        = next;
        run_queue_head = next;
        tss_set_kernel_stack(next->stack_top);
        switch_to(prev, next);
    }
    ticks_left = SCHEDULER_TICKS;

    if (flags & (1u << 9))
        __asm__ volatile ("sti");
}

task_t* scheduler_current(void)
{
    return current;
}

int scheduler_need_reschedule(void)
{
    return need_reschedule;
}

void scheduler_set_need_reschedule(void)
{
    need_reschedule = 1;
}

uint32_t scheduler_current_page_dir_phys(void)
{
    return current ? current->page_dir_phys : 0;
}