#include "scheduler.h"
#include "stack_guard.h"

#define SCHEDULER_MAX_TASKS 8U
#define SCHEDULER_PRIORITY_LEVELS 256U
#define SCHEDULER_INVALID_INDEX SCHEDULER_MAX_TASKS
#define SCHEDULER_INITIAL_RFLAGS 0x202ULL
#define SCHEDULER_SWITCH_LAUNCH_FLAG 0x1ULL
#define SCHEDULER_FRAME_WORDS 18U
#define SCHEDULER_FRAME_RIP_INDEX 15U
#define SCHEDULER_FRAME_CS_INDEX 16U
#define SCHEDULER_FRAME_RFLAGS_INDEX 17U

typedef struct {
  TCB *tcb;
  const char *name;
  SCHEDULER_TASK_FN fn;
  void *arg;
  int started;
  int in_ready_queue;
  unsigned int queue_next;
} SchedulerTask;

typedef struct {
  unsigned int head;
  unsigned int tail;
  unsigned int count;
} ReadyQueue;

extern void scheduler_launch_first_task(void *stack_pointer);
extern void scheduler_task_entry_stub(void);

static SchedulerTask g_tasks[SCHEDULER_MAX_TASKS];
static ReadyQueue g_ready_queues[SCHEDULER_PRIORITY_LEVELS];
static unsigned int g_task_count = 0;
static unsigned int g_completed_tasks = 0;
static void (*g_idle_callback)(void) = 0;
static unsigned int g_current_index = SCHEDULER_INVALID_INDEX;
static int g_scheduler_started = 0;
static volatile int g_timer_irq_active = 0;
static volatile uint64_t g_timer_preemption_count = 0;
static volatile uint64_t g_context_switch_count = 0;
static volatile uint64_t g_irq_reentry_drop_count = 0;
static volatile uint64_t g_invalid_state_count = 0;
static volatile uint64_t g_invalid_frame_count = 0;

static uint16_t scheduler_read_cs_selector(void) {
  uint16_t selector;

  __asm__ volatile("mov %%cs, %0" : "=r"(selector));
  return selector;
}

static uint32_t scheduler_priority_from_tcb(const TCB *tcb) {
  if (tcb == 0) {
    return SCHEDULER_PRIORITY_LEVELS - 1U;
  }
  if (tcb->priority >= SCHEDULER_PRIORITY_LEVELS) {
    return SCHEDULER_PRIORITY_LEVELS - 1U;
  }
  return tcb->priority;
}

static void scheduler_ready_queues_reset(void) {
  unsigned int i;

  for (i = 0; i < SCHEDULER_PRIORITY_LEVELS; ++i) {
    g_ready_queues[i].head = SCHEDULER_INVALID_INDEX;
    g_ready_queues[i].tail = SCHEDULER_INVALID_INDEX;
    g_ready_queues[i].count = 0;
  }
}

static int scheduler_enqueue_ready_task(unsigned int index) {
  ReadyQueue *queue;
  uint32_t priority;

  if (index >= g_task_count || g_tasks[index].tcb == 0) {
    return 0;
  }
  if (g_tasks[index].in_ready_queue) {
    return 1;
  }

  priority = scheduler_priority_from_tcb(g_tasks[index].tcb);
  queue = &g_ready_queues[priority];
  g_tasks[index].queue_next = SCHEDULER_INVALID_INDEX;

  if (queue->tail == SCHEDULER_INVALID_INDEX) {
    queue->head = index;
    queue->tail = index;
  } else {
    g_tasks[queue->tail].queue_next = index;
    queue->tail = index;
  }

  g_tasks[index].in_ready_queue = 1;
  queue->count++;
  return 1;
}

static int scheduler_remove_ready_task(unsigned int index) {
  ReadyQueue *queue;
  unsigned int cursor;
  unsigned int previous;
  uint32_t priority;

  if (index >= g_task_count || g_tasks[index].tcb == 0 || !g_tasks[index].in_ready_queue) {
    return 0;
  }

  priority = scheduler_priority_from_tcb(g_tasks[index].tcb);
  queue = &g_ready_queues[priority];
  cursor = queue->head;
  previous = SCHEDULER_INVALID_INDEX;

  while (cursor != SCHEDULER_INVALID_INDEX && cursor != index) {
    previous = cursor;
    cursor = g_tasks[cursor].queue_next;
  }

  if (cursor == SCHEDULER_INVALID_INDEX) {
    g_tasks[index].in_ready_queue = 0;
    g_tasks[index].queue_next = SCHEDULER_INVALID_INDEX;
    return 0;
  }

  if (previous == SCHEDULER_INVALID_INDEX) {
    queue->head = g_tasks[cursor].queue_next;
  } else {
    g_tasks[previous].queue_next = g_tasks[cursor].queue_next;
  }

  if (queue->tail == cursor) {
    queue->tail = previous;
  }

  if (queue->count > 0) {
    queue->count--;
  }

  g_tasks[index].in_ready_queue = 0;
  g_tasks[index].queue_next = SCHEDULER_INVALID_INDEX;
  return 1;
}

static unsigned int scheduler_dequeue_next_ready_task(void) {
  unsigned int priority;

  for (priority = 0; priority < SCHEDULER_PRIORITY_LEVELS; ++priority) {
    ReadyQueue *queue = &g_ready_queues[priority];
    unsigned int index = queue->head;

    if (index == SCHEDULER_INVALID_INDEX) {
      continue;
    }

    queue->head = g_tasks[index].queue_next;
    if (queue->head == SCHEDULER_INVALID_INDEX) {
      queue->tail = SCHEDULER_INVALID_INDEX;
    }
    if (queue->count > 0) {
      queue->count--;
    }

    g_tasks[index].in_ready_queue = 0;
    g_tasks[index].queue_next = SCHEDULER_INVALID_INDEX;
    return index;
  }

  return SCHEDULER_INVALID_INDEX;
}

static int scheduler_has_runnable_tasks(void) {
  unsigned int priority;

  for (priority = 0; priority < SCHEDULER_PRIORITY_LEVELS; ++priority) {
    if (g_ready_queues[priority].count > 0) {
      return 1;
    }
  }
  return 0;
}

static unsigned int scheduler_find_task_index_by_tcb(const TCB *task) {
  unsigned int i;

  if (task == 0) {
    return SCHEDULER_INVALID_INDEX;
  }

  for (i = 0; i < g_task_count; ++i) {
    if (g_tasks[i].tcb == task) {
      return i;
    }
  }
  return SCHEDULER_INVALID_INDEX;
}

static void *scheduler_encode_launch_stack_pointer(void *stack_pointer) {
  uintptr_t value;

  if (stack_pointer == 0) {
    return 0;
  }

  value = (uintptr_t)stack_pointer;
  return (void *)(value | (uintptr_t)SCHEDULER_SWITCH_LAUNCH_FLAG);
}

static void scheduler_snapshot_context(TCB *tcb, uint64_t *interrupt_stack_pointer) {
  uint64_t *register_state;

  if (tcb == 0 || interrupt_stack_pointer == 0) {
    return;
  }

  tcb->stack_pointer = (void *)interrupt_stack_pointer;
  tcb->instruction_pointer = (void *)(uintptr_t)interrupt_stack_pointer[SCHEDULER_FRAME_RIP_INDEX];

  register_state = tcb->register_state;
  if (register_state == 0) {
    return;
  }

  register_state[0] = interrupt_stack_pointer[0];
  register_state[1] = interrupt_stack_pointer[1];
  register_state[2] = interrupt_stack_pointer[2];
  register_state[3] = interrupt_stack_pointer[3];
  register_state[4] = interrupt_stack_pointer[4];
  register_state[5] = interrupt_stack_pointer[5];
  register_state[6] = interrupt_stack_pointer[7];
  register_state[7] = interrupt_stack_pointer[8];
  register_state[8] = interrupt_stack_pointer[9];
  register_state[9] = interrupt_stack_pointer[10];
  register_state[10] = interrupt_stack_pointer[11];
  register_state[11] = interrupt_stack_pointer[12];
  register_state[12] = interrupt_stack_pointer[13];
  register_state[13] = interrupt_stack_pointer[14];
  register_state[14] = (uint64_t)(uintptr_t)interrupt_stack_pointer;
  register_state[15] = interrupt_stack_pointer[6];
  register_state[16] = interrupt_stack_pointer[SCHEDULER_FRAME_RIP_INDEX];
}

static void scheduler_initialize_task_context(TCB *tcb) {
  uint64_t *frame;
  uintptr_t stack_top;
  unsigned int i;

  if (tcb == 0 || tcb->stack_base == 0 || tcb->stack_size < (SCHEDULER_FRAME_WORDS * sizeof(uint64_t))) {
    return;
  }

  stack_top = ((uintptr_t)tcb->stack_base + (uintptr_t)tcb->stack_size) & ~(uintptr_t)0xFULL;
  frame = (uint64_t *)(stack_top - (SCHEDULER_FRAME_WORDS * sizeof(uint64_t)));

  for (i = 0; i < SCHEDULER_FRAME_WORDS; ++i) {
    frame[i] = 0;
  }

  frame[SCHEDULER_FRAME_RIP_INDEX] = (uint64_t)(uintptr_t)scheduler_task_entry_stub;
  frame[SCHEDULER_FRAME_CS_INDEX] = (uint64_t)scheduler_read_cs_selector();
  frame[SCHEDULER_FRAME_RFLAGS_INDEX] = SCHEDULER_INITIAL_RFLAGS;

  tcb->stack_pointer = (void *)frame;
  tcb->instruction_pointer = (void *)(uintptr_t)frame[SCHEDULER_FRAME_RIP_INDEX];
  scheduler_snapshot_context(tcb, frame);
}

static void scheduler_activate_task(unsigned int index, int switched_tasks) {
  if (index >= g_task_count || g_tasks[index].tcb == 0) {
    return;
  }

  g_current_index = index;
  g_tasks[index].tcb->state = TASK_STATE_RUNNING;
  if (switched_tasks) {
    g_tasks[index].tcb->context_switches++;
  }
}

static __attribute__((noreturn)) void scheduler_halt_forever(void) {
  __asm__ volatile("cli");
  for (;;) {
    __asm__ volatile("hlt");
  }
}

static __attribute__((noreturn)) void scheduler_wait_for_reschedule(void) {
  for (;;) {
    __asm__ volatile("sti; hlt");
  }
}

static void scheduler_print_summary(void) {
  unsigned int i;

  console_printf("Scheduler preemption ticks: %llu\r\n", scheduler_get_timer_preemption_count());
  console_printf("Scheduler context switches: %llu\r\n", scheduler_get_context_switch_count());
  console_printf("Scheduler IRQ reentry drops: %llu\r\n", scheduler_get_irq_reentry_drop_count());
  console_printf("Scheduler invalid states: %llu\r\n", scheduler_get_invalid_state_count());
  console_printf("Scheduler invalid frames: %llu\r\n", scheduler_get_invalid_frame_count());
  for (i = 0; i < g_task_count; ++i) {
    TCB *tcb = g_tasks[i].tcb;
    if (tcb == 0) {
      continue;
    }
    console_write_string("    ");
    console_write_string(g_tasks[i].name ? g_tasks[i].name : "(unnamed)");
    console_printf(": state=%u ticks=%llu switches=%u priority=%u\r\n",
                   (unsigned int)tcb->state,
                   tcb->ticks_running,
                   tcb->context_switches,
                   tcb->priority);
  }
}

void scheduler_init(void) {
  unsigned int i;

  for (i = 0; i < SCHEDULER_MAX_TASKS; ++i) {
    g_tasks[i].tcb = 0;
    g_tasks[i].name = 0;
    g_tasks[i].fn = 0;
    g_tasks[i].arg = 0;
    g_tasks[i].started = 0;
    g_tasks[i].in_ready_queue = 0;
    g_tasks[i].queue_next = SCHEDULER_INVALID_INDEX;
  }
  scheduler_ready_queues_reset();
  g_task_count = 0;
  g_completed_tasks = 0;
  g_current_index = SCHEDULER_INVALID_INDEX;
  g_scheduler_started = 0;
  g_timer_irq_active = 0;
  g_timer_preemption_count = 0;
  g_context_switch_count = 0;
  g_irq_reentry_drop_count = 0;
  g_invalid_state_count = 0;
  g_invalid_frame_count = 0;
  stack_guard_init();
}

int scheduler_add_task_with_priority(const char *name, SCHEDULER_TASK_FN fn, void *arg, uint32_t priority) {
  TCB *tcb;
  unsigned int index;

  if (fn == 0 || g_task_count >= SCHEDULER_MAX_TASKS) {
    return 0;
  }

  tcb = tcb_create(name, priority);
  if (tcb == 0) {
    return 0;
  }

  scheduler_initialize_task_context(tcb);
  if (tcb->stack_pointer == 0) {
    tcb_destroy(tcb);
    return 0;
  }

  tcb->state = TASK_STATE_READY;

  index = g_task_count;
  g_tasks[index].tcb = tcb;
  g_tasks[index].name = name;
  g_tasks[index].fn = fn;
  g_tasks[index].arg = arg;
  g_tasks[index].started = 0;
  g_tasks[index].in_ready_queue = 0;
  g_tasks[index].queue_next = SCHEDULER_INVALID_INDEX;
  ++g_task_count;

  if (!scheduler_enqueue_ready_task(index)) {
    tcb_destroy(tcb);
    g_tasks[index].tcb = 0;
    g_tasks[index].name = 0;
    g_tasks[index].fn = 0;
    g_tasks[index].arg = 0;
    g_tasks[index].started = 0;
    g_tasks[index].in_ready_queue = 0;
    g_tasks[index].queue_next = SCHEDULER_INVALID_INDEX;
    --g_task_count;
    return 0;
  }

  return 1;
}

int scheduler_add_task(const char *name, SCHEDULER_TASK_FN fn, void *arg) {
  return scheduler_add_task_with_priority(name, fn, arg, 127U);
}

int scheduler_task_block(TCB *task) {
  unsigned int index = scheduler_find_task_index_by_tcb(task);

  if (index == SCHEDULER_INVALID_INDEX || task == 0 || task->state == TASK_STATE_TERMINATED) {
    return 0;
  }
  if (task->state == TASK_STATE_BLOCKED) {
    return 1;
  }

  if (task->state == TASK_STATE_READY) {
    scheduler_remove_ready_task(index);
  }
  task->state = TASK_STATE_BLOCKED;
  return 1;
}

int scheduler_task_unblock(TCB *task) {
  unsigned int index = scheduler_find_task_index_by_tcb(task);

  if (index == SCHEDULER_INVALID_INDEX || task == 0 || task->state != TASK_STATE_BLOCKED) {
    return 0;
  }

  task->state = TASK_STATE_READY;
  return scheduler_enqueue_ready_task(index);
}

int scheduler_task_terminate(TCB *task) {
  unsigned int index = scheduler_find_task_index_by_tcb(task);

  if (index == SCHEDULER_INVALID_INDEX || task == 0 || task->state == TASK_STATE_TERMINATED) {
    return 0;
  }

  if (task->state == TASK_STATE_READY) {
    scheduler_remove_ready_task(index);
  }
  task->state = TASK_STATE_TERMINATED;
  if (g_completed_tasks < g_task_count) {
    g_completed_tasks++;
  }
  return 1;
}

void *scheduler_on_timer_tick(void *interrupt_stack_pointer) {
  SchedulerTask *current_task;
  void *selected_interrupt_stack_pointer;
  unsigned int next_index;

  /*
   * Called from IRQ0 with a pointer to the saved interrupt frame.
   * Semantics:
   * - On invalid/unsafe conditions, return current frame unchanged.
   * - New tasks are launch-marked for non-iret first-run path.
   * - Started tasks resume via normal iret frame restore.
   */
  selected_interrupt_stack_pointer = interrupt_stack_pointer;
  if (!g_scheduler_started ||
      interrupt_stack_pointer == 0 ||
      g_task_count == 0 ||
      g_current_index >= g_task_count) {
    return selected_interrupt_stack_pointer;
  }

  if (g_timer_irq_active) {
    g_irq_reentry_drop_count++;
    return selected_interrupt_stack_pointer;
  }

  g_timer_irq_active = 1;
  g_timer_preemption_count++;
  stack_guard_check_watermarks();
  current_task = &g_tasks[g_current_index];

  if (current_task->tcb == 0 || current_task->fn == 0) {
    g_invalid_state_count++;
    goto on_timer_tick_done;
  }

  if (current_task->tcb->state == TASK_STATE_RUNNING) {
    current_task->tcb->ticks_running++;
    scheduler_snapshot_context(current_task->tcb, (uint64_t *)interrupt_stack_pointer);
    current_task->tcb->state = TASK_STATE_READY;
    scheduler_enqueue_ready_task(g_current_index);
  } else if (current_task->tcb->state != TASK_STATE_BLOCKED &&
             current_task->tcb->state != TASK_STATE_TERMINATED &&
             current_task->tcb->state != TASK_STATE_READY) {
    g_invalid_state_count++;
    goto on_timer_tick_done;
  }

  next_index = scheduler_dequeue_next_ready_task();
  if (next_index == SCHEDULER_INVALID_INDEX) {
    goto on_timer_tick_done;
  }

  if (g_tasks[next_index].tcb == 0 || g_tasks[next_index].tcb->stack_pointer == 0) {
    g_invalid_frame_count++;
    goto on_timer_tick_done;
  }

  if (next_index != g_current_index) {
    g_context_switch_count++;
    scheduler_activate_task(next_index, 1);
  } else {
    scheduler_activate_task(next_index, 0);
  }
  if (!g_tasks[next_index].started) {
    g_tasks[next_index].started = 1;
    selected_interrupt_stack_pointer =
        scheduler_encode_launch_stack_pointer(g_tasks[next_index].tcb->stack_pointer);
  } else {
    selected_interrupt_stack_pointer = g_tasks[next_index].tcb->stack_pointer;
  }

on_timer_tick_done:
  g_timer_irq_active = 0;
  return selected_interrupt_stack_pointer;
}

uint64_t scheduler_get_timer_preemption_count(void) {
  return g_timer_preemption_count;
}

uint64_t scheduler_get_context_switch_count(void) {
  return g_context_switch_count;
}

uint64_t scheduler_get_irq_reentry_drop_count(void) {
  return g_irq_reentry_drop_count;
}

uint64_t scheduler_get_invalid_state_count(void) {
  return g_invalid_state_count;
}

uint64_t scheduler_get_invalid_frame_count(void) {
  return g_invalid_frame_count;
}

#ifdef KERNEL_TESTS
void scheduler_test_set_started(int started) {
  g_scheduler_started = started ? 1 : 0;
}

void scheduler_test_set_current_index(unsigned int index) {
  g_current_index = index;
}

void scheduler_test_set_irq_active(int active) {
  g_timer_irq_active = active ? 1 : 0;
}

void scheduler_test_set_task_started(unsigned int index, int started) {
  if (index >= g_task_count) {
    return;
  }
  g_tasks[index].started = started ? 1 : 0;
}

void scheduler_test_set_task_state(unsigned int index, uint32_t state) {
  if (index >= g_task_count || g_tasks[index].tcb == 0) {
    return;
  }
  g_tasks[index].tcb->state = (task_state_t)state;
}

void scheduler_test_activate_running(unsigned int index) {
  if (index >= g_task_count || g_tasks[index].tcb == 0) {
    return;
  }

  scheduler_remove_ready_task(index);
  g_current_index = index;
  g_tasks[index].tcb->state = TASK_STATE_RUNNING;
}

unsigned int scheduler_test_get_completed_tasks(void) {
  return g_completed_tasks;
}
#endif

void scheduler_task_entry(void) {
  SchedulerTask *task;
  TCB *tcb;

  for (;;) {
    if (g_current_index >= g_task_count) {
      scheduler_halt_forever();
    }

    task = &g_tasks[g_current_index];
    tcb = task->tcb;
    if (task->fn == 0 || tcb == 0) {
      scheduler_halt_forever();
    }

    if (task->fn(task->arg) != 0) {
      continue;
    }

    scheduler_task_terminate(tcb);

    console_write_string("    task complete: ");
    console_write_string(task->name ? task->name : "(unnamed)");
    console_write_string("\r\n");

    if (g_completed_tasks >= g_task_count) {
      scheduler_print_summary();
      console_write_string("Scheduler idle\r\n");
      if (g_idle_callback != 0) {
        g_idle_callback();
      }
      scheduler_halt_forever();
    }

    scheduler_wait_for_reschedule();
  }
}

void scheduler_set_idle_callback(void (*callback)(void)) {
  g_idle_callback = callback;
}

__attribute__((noreturn)) void scheduler_run(void) {
  unsigned int first_task_index;

  console_write_string("[6] Scheduler online\r\n");
  if (!scheduler_has_runnable_tasks()) {
    console_write_string("Scheduler idle\r\n");
    scheduler_halt_forever();
  }

  first_task_index = scheduler_dequeue_next_ready_task();
  if (first_task_index == SCHEDULER_INVALID_INDEX) {
    console_write_string("Scheduler idle\r\n");
    scheduler_halt_forever();
  }

  __asm__ volatile("cli");
  scheduler_activate_task(first_task_index, 0);
  g_tasks[first_task_index].started = 1;
  g_scheduler_started = 1;

  console_write_string("    launching task: ");
  console_write_string(g_tasks[first_task_index].name ? g_tasks[first_task_index].name : "(unnamed)");
  console_write_string("\r\n");

  scheduler_launch_first_task(g_tasks[first_task_index].tcb->stack_pointer);
  __builtin_unreachable();
}
