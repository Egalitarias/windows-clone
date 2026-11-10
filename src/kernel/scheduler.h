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

// While the count is above zero the timer tick still runs but does not switch
// tasks. Used around short non-reentrant sections (the FAT driver). Calls nest.
// Do not block or wait for another task while holding it.
void scheduler_preempt_disable(void);
void scheduler_preempt_enable(void);
// Nesting depth of scheduler_preempt_disable(). A syscall that is aborted by a
// kernel fault never reaches its matching enable, so the process code saves the
// level at syscall entry and puts it back when it kills the process.
unsigned int scheduler_preempt_level(void);
void scheduler_preempt_restore(unsigned int level);

// Called once, after all tasks complete. It may add new tasks (the desktop does);
// if none are runnable afterwards the scheduler halts.
void scheduler_set_idle_callback(void (*callback)(void));
int scheduler_add_task(const char *name, SCHEDULER_TASK_FN fn, void *arg);
int scheduler_add_task_with_priority(const char *name, SCHEDULER_TASK_FN fn, void *arg, uint32_t priority);
// Block the running task until a wake-up: kind TASK_WAIT_SLEEP wakes at wake_ms
// (timer_get_ms), TASK_WAIT_INPUT on scheduler_wake_all(TASK_WAIT_INPUT) (or at wake_ms
// when that is non-zero: input with a timeout). Call with
// interrupts off, re-check the wake condition in a loop, and park with
// "sti; hlt; cli": the next tick switches to another task, and the task returns
// here once woken. scheduler_wait_end() must be called when the wait is over.
void scheduler_block_current(uint32_t kind, uint64_t wake_ms);
void scheduler_wait_end(void);
void scheduler_wake_all(uint32_t kind);
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
