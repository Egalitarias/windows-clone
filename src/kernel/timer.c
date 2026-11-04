#include "timer.h"
#include "console.h"
#include "pic.h"

#ifndef TIMER_VERBOSE_DEBUG
#define TIMER_VERBOSE_DEBUG 0
#endif

static volatile uint64_t g_ticks = 0;

static void pit_outb(uint16_t port, uint8_t value) {
  __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static uint8_t pit_inb(uint16_t port) {
  uint8_t value;
  __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
  return value;
}

void timer_init(void) {
  uint32_t divisor = TIMER_DIVISOR;

#if TIMER_VERBOSE_DEBUG
  console_printf("[TIMER] PIT divisor: %u (1193182 / %u = %u Hz)\r\n",
                 divisor, divisor, PIT_BASE_FREQUENCY / divisor);
#endif

  // Mask IRQ0 before reprogramming PIT so no timer IRQ can race in.
  timer_mask_irq0();

  // Set PIT to rate generator mode (mode 2)
  pit_outb(PIT_CONTROL, PIT_MODE_RATE_GENERATOR);
#if TIMER_VERBOSE_DEBUG
  console_write_string("[TIMER] Sent PIT mode control byte\r\n");
#endif

  // Write divisor (LSB first, then MSB)
  pit_outb(PIT_CHANNEL0, (uint8_t)(divisor & 0xFF));
  pit_outb(PIT_CHANNEL0, (uint8_t)((divisor >> 8) & 0xFF));
#if TIMER_VERBOSE_DEBUG
  console_printf("[TIMER] Wrote divisor: 0x%x\r\n", divisor);
#endif

  g_ticks = 0;

  console_write_string("[4] Timer initialized\r\n");
}

void timer_unmask_irq0(void) {
  uint8_t mask = pit_inb(PIC_MASTER_DATA);
#if TIMER_VERBOSE_DEBUG
  console_printf("[TIMER] PIC master mask before: 0x%x\r\n", mask);
#endif
  mask &= ~1;  // Unmask IRQ0
  pit_outb(PIC_MASTER_DATA, mask);
#if TIMER_VERBOSE_DEBUG
  console_printf("[TIMER] PIC master mask after: 0x%x\r\n", mask);
#endif
}

void timer_mask_irq0(void) {
  uint8_t mask = pit_inb(PIC_MASTER_DATA);
#if TIMER_VERBOSE_DEBUG
  console_printf("[TIMER] PIC master mask before: 0x%x\r\n", mask);
#endif
  mask |= 1;  // Mask IRQ0
  pit_outb(PIC_MASTER_DATA, mask);
#if TIMER_VERBOSE_DEBUG
  console_printf("[TIMER] PIC master mask after: 0x%x\r\n", mask);
#endif
}

#ifdef KERNEL_TESTS
int timer_is_irq0_masked(void) {
  return (pit_inb(PIC_MASTER_DATA) & 1U) != 0;
}
#endif

uint64_t timer_get_ticks(void) {
  return g_ticks;
}

void timer_tick(void) {
  g_ticks++;
}
