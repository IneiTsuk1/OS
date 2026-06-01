#include "pit.h"
#include "../../kernel/irq.h"
#include "../../kernel/klog.h"
#include "../../kernel/panic.h"
#include "../../kernel/scheduler.h"
#include <stdint.h>

#define PIT_CHANNEL0  0x40
#define PIT_CMD       0x43
#define PIT_BASE_HZ   1193182

static volatile uint32_t ticks = 0;
static uint32_t tick_freq = 0;

static inline void outb(uint16_t port, uint8_t val)
{
    __asm__ volatile ("outb %0, %1" :: "a"(val), "Nd"(port));
}

static void pit_irq_handler(regs_t* r)
{
    (void)r;
    ticks++;
    scheduler_tick();
}

void pit_init(uint32_t frequency)
{
    tick_freq = frequency;

    uint32_t divisor = PIT_BASE_HZ / frequency;

    // Channel 0, lobyte/hibyte, rate generator (mode 2)
    outb(PIT_CMD, 0x36);
    outb(PIT_CHANNEL0, (uint8_t)(divisor & 0xFF));
    outb(PIT_CHANNEL0, (uint8_t)(divisor >> 8));

    irq_install_handler(0, pit_irq_handler);
    klog_info("PIT: %u Hz (divisor=%u)", frequency, divisor);
}

uint32_t pit_get_ticks(void)
{
    return ticks;
}

void pit_sleep(uint32_t ms)
{
    if (ms == 0)
        return;

    // Sanity check: scheduler_sleep uses hlt internally; IF=0 would hang.
    uint32_t eflags;
    __asm__ volatile ("pushf; pop %0" : "=r"(eflags));
    if (!(eflags & (1u << 9)))
        panic("pit_sleep: called with interrupts disabled (IF=0)");

    uint32_t wake_tick = ticks + (tick_freq * ms / 1000);
    scheduler_sleep(wake_tick);
}