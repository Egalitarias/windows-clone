#ifndef KERNEL_GDT_H
#define KERNEL_GDT_H

// GDT Selector Constants
#define GDT_KERNEL_CS   0x08U  // Index 1, RPL=0: Kernel Code
#define GDT_KERNEL_DS   0x10U  // Index 2, RPL=0: Kernel Data
#define GDT_USER_CS     0x2BU  // Index 3, RPL=3: User Code (3*8+3)
#define GDT_USER_DS     0x33U  // Index 4, RPL=3: User Data (4*8+3)

// Install a minimal kernel GDT whose descriptors live in the kernel .data section
// (initialized static array, guaranteed mapped after paging_init).  Returns the 
// new kernel CS selector (0x08), which callers must use to update the IDT.
unsigned short gdt_init(void);

#endif // KERNEL_GDT_H
