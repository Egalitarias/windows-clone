#ifndef KERNEL_TIMER_H
#define KERNEL_TIMER_H

#include "console.h"

// PIT (Programmable Interval Timer) ports
#define PIT_CHANNEL0 0x40
#define PIT_CHANNEL1 0x41
#define PIT_CHANNEL2 0x42
#define PIT_CONTROL  0x43

// PIT base frequency (Hz)
#define PIT_BASE_FREQUENCY 1193182

// Target frequency: 1000 Hz (1ms per tick)
#define TIMER_TARGET_FREQUENCY 1000
#define TIMER_DIVISOR (PIT_BASE_FREQUENCY / TIMER_TARGET_FREQUENCY)

// PIT control byte settings
#define PIT_MODE_RATE_GENERATOR 0x34  // Channel 0, lobyte/hibyte, mode 2 (rate generator), binary
#define PIT_EOI_COMMAND 0x20           // End of Interrupt command for PIC

void timer_init(void);
uint64_t timer_get_ticks(void);
void timer_tick(void);
void timer_unmask_irq0(void);
void timer_mask_irq0(void);
#ifdef KERNEL_TESTS
int timer_is_irq0_masked(void);
#endif

#endif // KERNEL_TIMER_H
