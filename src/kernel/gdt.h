#ifndef KERNEL_GDT_H
#define KERNEL_GDT_H

#include "console.h"

// GDT selector constants.
//
// The order of the user entries is dictated by SYSRET: with STAR[63:48] = 0x10,
// a 64-bit SYSRET loads SS = 0x10 + 8 (|3) and CS = 0x10 + 16 (|3), so user data
// must sit directly below user code.
#define GDT_KERNEL_CS   0x08U  // Index 1, RPL=0: Kernel Code
#define GDT_KERNEL_DS   0x10U  // Index 2, RPL=0: Kernel Data
#define GDT_USER_DS     0x1BU  // Index 3, RPL=3: User Data (3*8+3)
#define GDT_USER_CS     0x23U  // Index 4, RPL=3: User Code (4*8+3)
#define GDT_TSS         0x28U  // Index 5-6: 64-bit TSS (16-byte descriptor)

// Install a minimal kernel GDT whose descriptors live in the kernel .data section
// (initialized static array, guaranteed mapped after paging_init) and load the
// task register. Returns the new kernel CS selector (0x08), which callers must use
// to update the IDT.
unsigned short gdt_init(void);

// Set the stack the CPU switches to when an interrupt or exception arrives from
// ring 3 (TSS.rsp0). Must be called whenever the running user task changes.
void gdt_set_kernel_stack(uint64_t rsp0);

// Read back the current TSS.rsp0 (for tests).
uint64_t gdt_get_kernel_stack(void);

#endif // KERNEL_GDT_H
