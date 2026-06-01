#pragma once
#include <stdint.h>

// Initialise PIT at the given frequency (Hz). Typical: 1000 for 1ms ticks.
void pit_init(uint32_t frequency);

// Current tick count since boot
uint32_t pit_get_ticks(void);

// Busy-wait for approximately ms milliseconds
void pit_sleep(uint32_t ms);