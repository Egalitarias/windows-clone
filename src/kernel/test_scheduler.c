#include "scheduler.h"
#include "tcb.h"
#include "console.h"

typedef struct {
  const char *name;
  int passed;
  int total;
} TestResult;

static TestResult results = {"Scheduler Tests", 0, 0};

#define TEST_ASSERT(condition, message) \
  do { \
    results.total++; \
    if (condition) { \
      results.passed++; \
      console_write_string("  ✓ "); \
      console_write_string(message); \
      console_write_string("\r\n"); \
    } else { \
      console_write_string("  ✗ "); \
      console_write_string(message); \
      console_write_string("\r\n"); \
    } \
  } while (0)

#define SCHEDULER_TEST_MAX_TASKS 8U
#define SCHEDULER_FRAME_RIP_INDEX 15U
#define SCHEDULER_FRAME_RFLAGS_INDEX 17U
#define SCHEDULER_SWITCH_LAUNCH_FLAG 0x1ULL

extern void scheduler_task_entry_stub(void);

static int scheduler_test_task(void *arg) {
  (void)arg;
  return 1;
}

static void test_scheduler_init_defaults(void) {
  console_write_string("[TEST] scheduler_init_defaults\r\n");

  tcb_init();
  scheduler_init();

  TEST_ASSERT(scheduler_get_timer_preemption_count() == 0, "Timer preemption count starts at 0");
  TEST_ASSERT(scheduler_get_context_switch_count() == 0, "Context switch count starts at 0");
  TEST_ASSERT(scheduler_get_irq_reentry_drop_count() == 0, "IRQ reentry drop count starts at 0");
  TEST_ASSERT(scheduler_get_invalid_state_count() == 0, "Invalid state count starts at 0");
  TEST_ASSERT(scheduler_get_invalid_frame_count() == 0, "Invalid frame count starts at 0");
}

static void test_scheduler_add_task_initializes_context_frame(void) {
  TCB *task;
  uint64_t *frame;

  console_write_string("[TEST] scheduler_add_task_initializes_context_frame\r\n");

  tcb_init();
  scheduler_init();

  TEST_ASSERT(scheduler_add_task("sched-test-task", scheduler_test_task, 0) == 1, "Task added successfully");

  task = tcb_get_by_id(1);
  TEST_ASSERT(task != 0, "Task is present in TCB pool");
  if (task == 0) {
    return;
  }

  TEST_ASSERT(task->state == TASK_STATE_READY, "Task state is READY after add");
  TEST_ASSERT(task->stack_pointer != 0, "Task stack pointer initialized");
  TEST_ASSERT(task->register_state != 0, "Task register state initialized");

  frame = (uint64_t *)task->stack_pointer;
  TEST_ASSERT(frame != 0, "Task has an initial interrupt frame");
  if (frame == 0) {
    return;
  }

  TEST_ASSERT(frame[SCHEDULER_FRAME_RIP_INDEX] != 0, "Initial frame RIP is populated");
  TEST_ASSERT((void *)(uintptr_t)frame[SCHEDULER_FRAME_RIP_INDEX] == task->instruction_pointer,
              "Frame RIP matches task instruction pointer");
  TEST_ASSERT(frame[SCHEDULER_FRAME_RIP_INDEX] == task->register_state[16],
              "Frame RIP matches saved register snapshot");
  TEST_ASSERT((frame[SCHEDULER_FRAME_RFLAGS_INDEX] & 0x200ULL) != 0,
              "Initial frame enables interrupts in RFLAGS");
}

static void test_scheduler_add_task_rejects_invalid_input(void) {
  console_write_string("[TEST] scheduler_add_task_rejects_invalid_input\r\n");

  tcb_init();
  scheduler_init();

  TEST_ASSERT(scheduler_add_task("invalid", 0, 0) == 0, "Null task function is rejected");
  TEST_ASSERT(tcb_get_by_id(1) == 0, "No task allocated when add_task fails");
}

static void test_scheduler_task_capacity_limit(void) {
  unsigned int i;

  console_write_string("[TEST] scheduler_task_capacity_limit\r\n");

  tcb_init();
  scheduler_init();

  for (i = 0; i < SCHEDULER_TEST_MAX_TASKS; ++i) {
    TEST_ASSERT(scheduler_add_task("cap", scheduler_test_task, 0) == 1, "Task accepted before capacity");
  }
  TEST_ASSERT(scheduler_add_task("overflow", scheduler_test_task, 0) == 0, "Task rejected at capacity");
}

static void test_scheduler_on_timer_tick_safe_before_start(void) {
  uint64_t fake_interrupt_frame[18];
  void *returned_sp;

  console_write_string("[TEST] scheduler_on_timer_tick_safe_before_start\r\n");

  tcb_init();
  scheduler_init();
  scheduler_add_task("task-a", scheduler_test_task, 0);

  returned_sp = scheduler_on_timer_tick(fake_interrupt_frame);
  TEST_ASSERT(returned_sp == fake_interrupt_frame, "Timer tick returns current frame before scheduler start");
  TEST_ASSERT(scheduler_get_timer_preemption_count() == 0, "Timer preemption count unchanged before scheduler start");
  TEST_ASSERT(scheduler_get_context_switch_count() == 0, "Context switch count unchanged before scheduler start");
}

static void test_scheduler_on_timer_tick_handles_null_frame_before_start(void) {
  void *returned_sp;

  console_write_string("[TEST] scheduler_on_timer_tick_handles_null_frame_before_start\r\n");

  tcb_init();
  scheduler_init();
  scheduler_add_task("task-a", scheduler_test_task, 0);

  returned_sp = scheduler_on_timer_tick(0);
  TEST_ASSERT(returned_sp == 0, "Timer tick returns null when input frame is null");
  TEST_ASSERT(scheduler_get_timer_preemption_count() == 0, "Timer preemption count unchanged for null frame");
  TEST_ASSERT(scheduler_get_context_switch_count() == 0, "Context switch count unchanged for null frame");
}

static void test_scheduler_add_task_assigns_distinct_tcb_ids(void) {
  TCB *task1;
  TCB *task2;

  console_write_string("[TEST] scheduler_add_task_assigns_distinct_tcb_ids\r\n");

  tcb_init();
  scheduler_init();
  scheduler_add_task("task-a", scheduler_test_task, 0);
  scheduler_add_task("task-b", scheduler_test_task, 0);

  task1 = tcb_get_by_id(1);
  task2 = tcb_get_by_id(2);
  TEST_ASSERT(task1 != 0, "First task exists in TCB pool");
  TEST_ASSERT(task2 != 0, "Second task exists in TCB pool");
  if (task1 == 0 || task2 == 0) {
    return;
  }
  TEST_ASSERT(task1->task_id != task2->task_id, "Task IDs are distinct");
  TEST_ASSERT(task1->stack_pointer != task2->stack_pointer, "Task stack pointers are distinct");
}

static void test_scheduler_on_timer_tick_rejects_reentry(void) {
  uint64_t fake_interrupt_frame[18];
  void *returned_sp;
  unsigned int i;

  console_write_string("[TEST] scheduler_on_timer_tick_rejects_reentry\r\n");

  tcb_init();
  scheduler_init();
  scheduler_add_task("task-a", scheduler_test_task, 0);
  for (i = 0; i < 18U; ++i) {
    fake_interrupt_frame[i] = 0;
  }
  scheduler_test_set_started(1);
  scheduler_test_activate_running(0);
  scheduler_test_set_irq_active(1);

  returned_sp = scheduler_on_timer_tick(fake_interrupt_frame);
  TEST_ASSERT(returned_sp == fake_interrupt_frame, "Reentrant timer tick returns current frame");
  TEST_ASSERT(scheduler_get_irq_reentry_drop_count() == 1, "Reentrant timer tick increments drop count");
  TEST_ASSERT(scheduler_get_timer_preemption_count() == 0, "Reentrant timer tick does not count preemption");
}

static void test_scheduler_on_timer_tick_tracks_invalid_state(void) {
  uint64_t fake_interrupt_frame[18];
  TCB *task;
  void *returned_sp;
  unsigned int i;

  console_write_string("[TEST] scheduler_on_timer_tick_tracks_invalid_state\r\n");

  tcb_init();
  scheduler_init();
  scheduler_add_task("task-a", scheduler_test_task, 0);
  for (i = 0; i < 18U; ++i) {
    fake_interrupt_frame[i] = 0;
  }
  task = tcb_get_by_id(1);
  TEST_ASSERT(task != 0, "Task exists for invalid-state test");
  if (task == 0) {
    return;
  }

  scheduler_test_set_started(1);
  scheduler_test_activate_running(0);
  scheduler_test_set_task_state(0, 99U);

  returned_sp = scheduler_on_timer_tick(fake_interrupt_frame);
  TEST_ASSERT(returned_sp == fake_interrupt_frame, "Invalid-state timer tick keeps current frame");
  TEST_ASSERT(scheduler_get_invalid_state_count() == 1, "Invalid state count increments");
  TEST_ASSERT(scheduler_get_context_switch_count() == 0, "Invalid state does not context switch");
}

static void test_scheduler_on_timer_tick_marks_first_switch_for_launch(void) {
  uint64_t fake_interrupt_frame[18];
  TCB *task2;
  void *returned_sp;
  unsigned int i;
  uintptr_t returned_value;
  uintptr_t task2_sp;

  console_write_string("[TEST] scheduler_on_timer_tick_marks_first_switch_for_launch\r\n");

  tcb_init();
  scheduler_init();
  scheduler_add_task("task-a", scheduler_test_task, 0);
  scheduler_add_task("task-b", scheduler_test_task, 0);
  task2 = tcb_get_by_id(2);
  TEST_ASSERT(task2 != 0, "Second task exists for launch-path test");
  if (task2 == 0) {
    return;
  }

  for (i = 0; i < 18U; ++i) {
    fake_interrupt_frame[i] = 0;
  }
  scheduler_test_set_started(1);
  scheduler_test_activate_running(0);
  scheduler_test_set_task_started(0, 1);

  returned_sp = scheduler_on_timer_tick(fake_interrupt_frame);
  returned_value = (uintptr_t)returned_sp;
  task2_sp = (uintptr_t)task2->stack_pointer;

  TEST_ASSERT((returned_value & SCHEDULER_SWITCH_LAUNCH_FLAG) != 0, "First switch to a new task is launch-marked");
  TEST_ASSERT((returned_value & ~(uintptr_t)SCHEDULER_SWITCH_LAUNCH_FLAG) == task2_sp,
              "Launch-marked pointer resolves to selected task stack");
}

static void test_scheduler_on_timer_tick_resumes_started_task_without_launch_marker(void) {
  uint64_t fake_interrupt_frame[18];
  TCB *task1;
  void *returned_sp;
  unsigned int i;
  uintptr_t returned_value;

  console_write_string("[TEST] scheduler_on_timer_tick_resumes_started_task_without_launch_marker\r\n");

  tcb_init();
  scheduler_init();
  scheduler_add_task("task-a", scheduler_test_task, 0);
  scheduler_add_task("task-b", scheduler_test_task, 0);
  task1 = tcb_get_by_id(1);
  TEST_ASSERT(task1 != 0, "First task exists for resume-path test");
  if (task1 == 0) {
    return;
  }

  for (i = 0; i < 18U; ++i) {
    fake_interrupt_frame[i] = 0;
  }
  scheduler_test_set_started(1);
  scheduler_test_activate_running(1);
  scheduler_test_set_task_started(0, 1);
  scheduler_test_set_task_started(1, 1);

  returned_sp = scheduler_on_timer_tick(fake_interrupt_frame);
  returned_value = (uintptr_t)returned_sp;

  TEST_ASSERT((returned_value & SCHEDULER_SWITCH_LAUNCH_FLAG) == 0, "Resume switch does not set launch marker");
  TEST_ASSERT(returned_sp == task1->stack_pointer, "Resume switch returns direct task stack pointer");
}

static void test_scheduler_on_timer_tick_alternates_started_tasks(void) {
  uint64_t frame_a[18];
  uint64_t frame_b[18];
  TCB *task1;
  TCB *task2;
  void *returned_sp;
  void *task2_initial_sp;
  unsigned int i;

  console_write_string("[TEST] scheduler_on_timer_tick_alternates_started_tasks\r\n");

  tcb_init();
  scheduler_init();
  scheduler_add_task("task-a", scheduler_test_task, 0);
  scheduler_add_task("task-b", scheduler_test_task, 0);
  task1 = tcb_get_by_id(1);
  task2 = tcb_get_by_id(2);
  TEST_ASSERT(task1 != 0, "Task A exists for alternation test");
  TEST_ASSERT(task2 != 0, "Task B exists for alternation test");
  if (task1 == 0 || task2 == 0) {
    return;
  }

  for (i = 0; i < 18U; ++i) {
    frame_a[i] = 0;
    frame_b[i] = 0;
  }

  task2_initial_sp = task2->stack_pointer;
  scheduler_test_set_started(1);
  scheduler_test_activate_running(0);
  scheduler_test_set_task_started(0, 1);
  scheduler_test_set_task_started(1, 1);

  returned_sp = scheduler_on_timer_tick(frame_a);
  TEST_ASSERT(returned_sp == task2_initial_sp, "First preemption switches from task A to task B");

  returned_sp = scheduler_on_timer_tick(frame_b);
  TEST_ASSERT(returned_sp == frame_a, "Second preemption switches back from task B to task A");
  TEST_ASSERT(scheduler_get_timer_preemption_count() == 2, "Preemption counter reflects two timer-driven switches");
  TEST_ASSERT(scheduler_get_context_switch_count() == 2, "Context switch counter reflects two alternations");
  TEST_ASSERT(task1->ticks_running == 1, "Task A tick counter increments once");
  TEST_ASSERT(task2->ticks_running == 1, "Task B tick counter increments once");
}

static void test_scheduler_priority_selection_prefers_higher_priority(void) {
  uint64_t frame[18];
  TCB *high_priority_task;
  void *returned_sp;
  unsigned int i;
  uintptr_t returned_value;

  console_write_string("[TEST] scheduler_priority_selection_prefers_higher_priority\r\n");

  tcb_init();
  scheduler_init();
  scheduler_add_task_with_priority("task-low", scheduler_test_task, 0, 200U);
  scheduler_add_task_with_priority("task-high", scheduler_test_task, 0, 5U);
  high_priority_task = tcb_get_by_id(2);
  TEST_ASSERT(high_priority_task != 0, "High-priority task exists");
  if (high_priority_task == 0) {
    return;
  }

  for (i = 0; i < 18U; ++i) {
    frame[i] = 0;
  }

  scheduler_test_set_started(1);
  scheduler_test_activate_running(0);
  scheduler_test_set_task_started(0, 1);
  scheduler_test_set_task_started(1, 0);

  returned_sp = scheduler_on_timer_tick(frame);
  returned_value = (uintptr_t)returned_sp & ~(uintptr_t)SCHEDULER_SWITCH_LAUNCH_FLAG;
  TEST_ASSERT(returned_value == (uintptr_t)high_priority_task->stack_pointer,
              "Scheduler picks higher-priority ready task first");
}

static void test_scheduler_block_unblock_lifecycle(void) {
  uint64_t frame_a[18];
  uint64_t frame_b[18];
  TCB *task1;
  TCB *task2;
  void *returned_sp;
  unsigned int i;

  console_write_string("[TEST] scheduler_block_unblock_lifecycle\r\n");

  tcb_init();
  scheduler_init();
  scheduler_add_task("task-a", scheduler_test_task, 0);
  scheduler_add_task("task-b", scheduler_test_task, 0);
  task1 = tcb_get_by_id(1);
  task2 = tcb_get_by_id(2);
  TEST_ASSERT(task1 != 0, "Task A exists for block/unblock test");
  TEST_ASSERT(task2 != 0, "Task B exists for block/unblock test");
  if (task1 == 0 || task2 == 0) {
    return;
  }

  for (i = 0; i < 18U; ++i) {
    frame_a[i] = 0;
    frame_b[i] = 0;
  }

  scheduler_test_set_started(1);
  scheduler_test_activate_running(0);
  scheduler_test_set_task_started(0, 1);
  scheduler_test_set_task_started(1, 1);

  TEST_ASSERT(scheduler_task_block(task2) == 1, "Blocking READY task succeeds");
  TEST_ASSERT(task2->state == TASK_STATE_BLOCKED, "Blocked task state is BLOCKED");

  returned_sp = scheduler_on_timer_tick(frame_a);
  TEST_ASSERT(returned_sp == frame_a, "Only runnable task continues while peer is blocked");
  TEST_ASSERT(scheduler_get_context_switch_count() == 0, "No context switch is counted when same task continues");
  TEST_ASSERT(task1->context_switches == 0, "Per-task switches stay unchanged when no task switch occurs");

  TEST_ASSERT(scheduler_task_unblock(task2) == 1, "Unblocking BLOCKED task succeeds");
  TEST_ASSERT(task2->state == TASK_STATE_READY, "Unblocked task returns to READY");

  returned_sp = scheduler_on_timer_tick(frame_b);
  TEST_ASSERT(returned_sp == task2->stack_pointer, "Unblocked task is scheduled on next tick");
  TEST_ASSERT(scheduler_get_context_switch_count() == 1, "Context switch count increments when task changes");
  TEST_ASSERT(task2->context_switches == 1, "Activated task records one switch when task changes");
}

static void test_scheduler_terminate_removes_ready_task(void) {
  uint64_t frame[18];
  TCB *task2;
  void *returned_sp;
  unsigned int i;

  console_write_string("[TEST] scheduler_terminate_removes_ready_task\r\n");

  tcb_init();
  scheduler_init();
  scheduler_add_task("task-a", scheduler_test_task, 0);
  scheduler_add_task("task-b", scheduler_test_task, 0);
  task2 = tcb_get_by_id(2);
  TEST_ASSERT(task2 != 0, "Task B exists for terminate test");
  if (task2 == 0) {
    return;
  }

  for (i = 0; i < 18U; ++i) {
    frame[i] = 0;
  }

  scheduler_test_set_started(1);
  scheduler_test_activate_running(0);
  scheduler_test_set_task_started(0, 1);
  scheduler_test_set_task_started(1, 1);

  TEST_ASSERT(scheduler_task_terminate(task2) == 1, "Terminating READY task succeeds");
  TEST_ASSERT(task2->state == TASK_STATE_TERMINATED, "Terminated task state is TERMINATED");
  TEST_ASSERT(scheduler_test_get_completed_tasks() == 1U, "Terminated task contributes to completion tracking");
  TEST_ASSERT(scheduler_task_terminate(task2) == 0, "Terminating already-terminated task fails");
  TEST_ASSERT(scheduler_test_get_completed_tasks() == 1U, "Repeated terminate does not double-count completion");

  returned_sp = scheduler_on_timer_tick(frame);
  TEST_ASSERT(returned_sp == frame, "Terminated task is not selected by scheduler");
}

void run_scheduler_tests(void) {
  // Timer tests enable interrupts; keep scheduler unit tests deterministic.
  __asm__ volatile("cli");

  console_write_string("========================================\r\n");
  console_write_string("Scheduler Test Suite\r\n");
  console_write_string("========================================\r\n");

  test_scheduler_init_defaults();
  test_scheduler_add_task_initializes_context_frame();
  test_scheduler_add_task_rejects_invalid_input();
  test_scheduler_task_capacity_limit();
  test_scheduler_on_timer_tick_safe_before_start();
  test_scheduler_on_timer_tick_handles_null_frame_before_start();
  test_scheduler_add_task_assigns_distinct_tcb_ids();
  test_scheduler_on_timer_tick_rejects_reentry();
  test_scheduler_on_timer_tick_tracks_invalid_state();
  test_scheduler_on_timer_tick_marks_first_switch_for_launch();
  test_scheduler_on_timer_tick_resumes_started_task_without_launch_marker();
  test_scheduler_on_timer_tick_alternates_started_tasks();
  test_scheduler_priority_selection_prefers_higher_priority();
  test_scheduler_block_unblock_lifecycle();
  test_scheduler_terminate_removes_ready_task();

  console_write_string("\r\n========================================\r\n");
  console_write_string("Test Results: ");
  console_printf("%d/%d passed", results.passed, results.total);
  console_write_string("\r\n");
  console_write_string("========================================\r\n");

  scheduler_init();
  tcb_init();
}
