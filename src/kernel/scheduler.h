#ifndef KERNEL_SCHEDULER_H
#define KERNEL_SCHEDULER_H

#include "console.h"
#include "tcb.h"

typedef int (*SCHEDULER_TASK_FN)(void *arg);

void scheduler_init(void);

// The task executing right now, or NULL before the scheduler has started.
TCB *scheduler_current_tcb(void);
// Nonzero once scheduler_run() has started dispatching tasks (timer preemption is live).
int scheduler_is_running(void);

// Called once, after all tasks complete and before the scheduler halts.
void scheduler_set_idle_callback(void (*callback)(void));
int scheduler_add_task(const char *name, SCHEDULER_TASK_FN fn, void *arg);
int scheduler_add_task_with_priority(const char *name, SCHEDULER_TASK_FN fn, void *arg, uint32_t priority);
int scheduler_task_block(TCB *task);
int scheduler_task_unblock(TCB *task);
int scheduler_task_terminate(TCB *task);
void *scheduler_on_timer_tick(void *interrupt_stack_pointer);
uint64_t scheduler_get_timer_preemption_count(void);
uint64_t scheduler_get_context_switch_count(void);
uint64_t scheduler_get_irq_reentry_drop_count(void);
uint64_t scheduler_get_invalid_state_count(void);
uint64_t scheduler_get_invalid_frame_count(void);
void scheduler_task_entry(void);
__attribute__((noreturn)) void scheduler_run(void);

#ifdef KERNEL_TESTS
void scheduler_test_set_started(int started);
void scheduler_test_set_current_index(unsigned int index);
void scheduler_test_set_irq_active(int active);
void scheduler_test_set_task_started(unsigned int index, int started);
void scheduler_test_set_task_state(unsigned int index, uint32_t state);
void scheduler_test_activate_running(unsigned int index);
unsigned int scheduler_test_get_completed_tasks(void);
#endif

#endif // KERNEL_SCHEDULER_H
