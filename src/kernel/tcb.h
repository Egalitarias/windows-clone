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

  // Per-process state, loaded by the scheduler whenever this task is switched in.
  uint64_t cr3;             // PML4 of the task's address space; 0 = the kernel's tables
  uint64_t kernel_rsp0;     // kernel stack for ring-3 entries (TSS.rsp0); 0 = leave unchanged

  // Why a BLOCKED task is blocked (TASK_WAIT_*), and for sleepers when to wake it.
  uint32_t wait_kind;
  uint64_t wake_ms;

  // x87/SSE register state (FXSAVE format), saved and restored by the scheduler when the task
  // is switched out and in. Kernel code uses no SSE, but user programs (Windows ones in
  // particular) do. Last, so the offsets the assembly uses stay put.
  uint8_t fx_state[512] __attribute__((aligned(16)));
} TCB;

#define TASK_WAIT_NONE  0U
#define TASK_WAIT_SLEEP 1U   // wakes when timer_get_ms() reaches wake_ms
#define TASK_WAIT_INPUT 2U   // wakes when scheduler_wake_all(TASK_WAIT_INPUT) is called

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
