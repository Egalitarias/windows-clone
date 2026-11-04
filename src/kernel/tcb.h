#ifndef KERNEL_TCB_H
#define KERNEL_TCB_H

#include "console.h"

typedef enum {
  TASK_STATE_READY,
  TASK_STATE_RUNNING,
  TASK_STATE_BLOCKED,
  TASK_STATE_TERMINATED
} task_state_t;

typedef struct {
  uint32_t task_id;
  const char *name;

  void *stack_pointer;
  void *instruction_pointer;
  uint64_t *register_state;

  task_state_t state;
  uint32_t priority;

  void *stack_base;
  uint64_t stack_size;

  uint64_t ticks_running;
  uint32_t context_switches;

  // Stack guard pages (adjacent non-present pages around stack)
  void *guard_page_low;
  void *guard_page_high;
  uint64_t overflow_attempts;

  // User-mode support (for Ring 3 processes)
  void *address_space;      // Pointer to address_space struct (NULL for kernel tasks)
  void *user_stack_base;    // Base of user-mode stack (user VA space)
  void *user_entry_point;   // Entry point for user process
  uint32_t exit_code;       // Exit code when task terminates
} TCB;

#define TCB_MAX_TASKS 256
#define TCB_DEFAULT_STACK_SIZE 4096
#define TCB_STACK_ALIGNMENT 16

void tcb_init(void);
TCB *tcb_create(const char *name, uint32_t priority);
void tcb_destroy(TCB *tcb);
TCB *tcb_get_by_id(uint32_t task_id);

// Get TCB pool entry by index (used by runtime diagnostics and tests).
TCB *tcb_get_by_index(uint32_t index);

#endif
