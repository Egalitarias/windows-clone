#ifndef KERNEL_IDT_H
#define KERNEL_IDT_H

#include "console.h"

void idt_init(void);

// Update every IDT gate's code-segment selector and reload the IDTR.
// Call this after installing a new GDT so that exception delivery uses a
// code descriptor that is actually mapped in the active page tables.
void idt_reload_selector(uint16_t new_cs);

// Debug: Dump IDT entry details for a given vector
void idt_debug_dump_vector(unsigned int vector);

#endif // KERNEL_IDT_H
