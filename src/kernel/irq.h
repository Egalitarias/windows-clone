#ifndef KERNEL_IRQ_H
#define KERNEL_IRQ_H

#include "console.h"

// Hardware interrupts through the 8259 PIC (IRQ0-15 -> vectors 32-47).
//
// Handlers run in interrupt context with interrupts disabled: keep them short,
// never draw or allocate, just move data into a buffer. The dispatcher sends
// the end-of-interrupt for you.
//
// Handlers are plain function pointers and must be registered at run time (the
// kernel is loaded without relocations, see docs/WINDOW_MANAGER.md).

typedef void (*irq_handler_t)(void);

// Mask every IRQ line. Call once before registering handlers.
void irq_init(void);

void irq_register(unsigned int irq, irq_handler_t handler);
void irq_unmask(unsigned int irq);   // also opens the cascade for IRQ8-15
void irq_mask(unsigned int irq);

// Called by the assembly stubs (irq_stubs.S).
void irq_dispatch(uint64_t irq);

#endif // KERNEL_IRQ_H
