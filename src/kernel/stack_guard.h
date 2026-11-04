#ifndef KERNEL_STACK_GUARD_H
#define KERNEL_STACK_GUARD_H

#include "console.h"

#define STACK_GUARD_PAGE_SIZE 4096ULL
#define STACK_GUARD_WATERMARK_LOW  0x5354414348475541ULL  // "STACKGUA"
#define STACK_GUARD_WATERMARK_HIGH 0x5244504147454E44ULL  // "RDPAGEEND"

// Initialize stack guard system
void stack_guard_init(void);

// Perform periodic watermark validation on all active tasks
// Called from scheduler timer tick
void stack_guard_check_watermarks(void);

// Get diagnostic counters
uint32_t stack_guard_get_overflow_attempts(void);
uint32_t stack_guard_get_watermark_errors(void);
void stack_guard_record_overflow(void);

#endif // KERNEL_STACK_GUARD_H
