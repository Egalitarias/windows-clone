#ifndef KERNEL_HEAP_H
#define KERNEL_HEAP_H

#include "console.h"  // for uint*_t typedefs

// Initialise the kernel heap by claiming pages from the PMM.
// Must be called after pmm_init().
void heap_init(void);

// Allocate `size` bytes.  Returns a pointer to aligned memory, or 0 on failure.
void *kmalloc(uint64_t size);

// Free a pointer previously returned by kmalloc.
void kfree(void *ptr);

// Bytes currently allocated (excludes heap metadata overhead).
uint64_t heap_used_bytes(void);

#endif // KERNEL_HEAP_H
