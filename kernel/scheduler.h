#pragma once
#include "task.h"
#include <stdint.h>

void     scheduler_init(void);
void     scheduler_add(task_t* task);
void     scheduler_exit(void);      // unlink current task, defer free, reschedule
void     scheduler_tick(void);
void     scheduler_yield(void);
void     scheduler_sleep(uint32_t wake_tick);
void     scheduler_wait(uint32_t tid); // block until task tid exits (or already gone)
task_t*  scheduler_current(void);

// Returns the physical address of the current task's page directory.
// Used by irq_stub.asm to load CR3 before iret.
uint32_t scheduler_current_page_dir_phys(void);

uint32_t scheduler_switch_from_irq(uint32_t esp);
int      scheduler_need_reschedule(void);