#ifndef KERNEL_IDT_H
#define KERNEL_IDT_H

#include "console.h"

void idt_init(void);

// Point an existing IDT vector at a new handler, keeping the current code
// selector. Only call with interrupts disabled.
void idt_set_handler(unsigned int vector, void *handler);

// Generic IRQ entry stubs (irq_stubs.S): irq_stub_N serves PIC line N and
// dispatches through irq_dispatch() in irq.c. Vector = 32 + N.
extern void irq_stub_0(void);

// Update every IDT gate's code-segment selector and reload the IDTR.
// Call this after installing a new GDT so that exception delivery uses a
// code descriptor that is actually mapped in the active page tables.
void idt_reload_selector(uint16_t new_cs);

// Debug: Dump IDT entry details for a given vector
void idt_debug_dump_vector(unsigned int vector);

#endif // KERNEL_IDT_H
