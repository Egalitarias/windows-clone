#ifndef KERNEL_IRQLOCK_H
#define KERNEL_IRQLOCK_H

#include "console.h"

// The scheduler preempts on the timer interrupt, so any data structure shared
// between tasks needs protection on this single-CPU kernel. For short sections
// (allocators, task tables) the simplest correct tool is to mask interrupts.
// Long operations (disk I/O) use scheduler_preempt_disable() instead, which
// keeps interrupts on but stops the tick from switching tasks.

static inline uint64_t irq_save(void) {
  uint64_t flags;

  __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
  return flags;
}

static inline void irq_restore(uint64_t flags) {
  if ((flags & 0x200ULL) != 0) {
    __asm__ volatile("sti" : : : "memory");
  }
}

#endif // KERNEL_IRQLOCK_H
