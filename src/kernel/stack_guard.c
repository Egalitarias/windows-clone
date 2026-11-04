#include "stack_guard.h"
#include "tcb.h"

static uint32_t g_overflow_attempts = 0;
static uint32_t g_watermark_errors = 0;

void stack_guard_init(void) {
  g_overflow_attempts = 0;
  g_watermark_errors = 0;
}

void stack_guard_check_watermarks(void) {
  uint32_t i;
  TCB *tcb;
  uint64_t *watermark_ptr;
  uint64_t watermark_value;

  for (i = 0; i < TCB_MAX_TASKS; ++i) {
    tcb = tcb_get_by_index(i);
    if (tcb == 0 || tcb->task_id == 0) {
      continue;
    }

    // Check lower watermark (at stack base)
    if (tcb->stack_base != 0) {
      watermark_ptr = (uint64_t *)tcb->stack_base;
      watermark_value = *watermark_ptr;
      if (watermark_value != STACK_GUARD_WATERMARK_LOW) {
        ++g_watermark_errors;
        ++tcb->overflow_attempts;
      }
    }

    // Check upper watermark (fixed at stack top - 8)
    if (tcb->stack_base != 0 && tcb->stack_size >= sizeof(uint64_t)) {
      watermark_ptr =
          (uint64_t *)((uintptr_t)tcb->stack_base + (uintptr_t)tcb->stack_size - sizeof(uint64_t));
      watermark_value = *watermark_ptr;
      if (watermark_value != STACK_GUARD_WATERMARK_HIGH) {
        ++g_watermark_errors;
        ++tcb->overflow_attempts;
      }
    }
  }
}

uint32_t stack_guard_get_overflow_attempts(void) {
  return g_overflow_attempts;
}

uint32_t stack_guard_get_watermark_errors(void) {
  return g_watermark_errors;
}

void stack_guard_record_overflow(void) {
  ++g_overflow_attempts;
}
