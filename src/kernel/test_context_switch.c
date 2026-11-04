#include "context_switch.h"
#include "heap.h"
#include "tcb.h"
#include "console.h"

typedef struct {
  const char *name;
  int passed;
  int total;
} TestResult;

static TestResult results = {"Context Switch Tests", 0, 0};

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

static TCB g_kernel_context;
static uint64_t g_kernel_register_state[17];
static TCB *g_task_a = 0;
static TCB *g_task_b = 0;
static volatile unsigned int g_task_a_iterations = 0;
static volatile unsigned int g_task_b_iterations = 0;
static volatile unsigned int g_register_restore_failures = 0;

static void context_switch_prepare_task(TCB *task, void (*entry)(void)) {
  uintptr_t stack_top;
  unsigned int i;

  if (task == 0 || task->stack_base == 0 || task->register_state == 0 || task->stack_size == 0 || entry == 0) {
    return;
  }

  stack_top = ((uintptr_t)task->stack_base + (uintptr_t)task->stack_size) & ~(uintptr_t)0xFULL;
  for (i = 0; i < 17U; ++i) {
    task->register_state[i] = 0;
  }

  // Follow SysV x86_64 ABI: RSP should point to stack location with 16-byte alignment BEFORE a push.
  // Set stack_pointer to stack_top - 8 (as if a return address was pushed).
  // Write a 0 sentinel as the return address at the stack top.
  task->stack_pointer = (void *)(stack_top - sizeof(uint64_t));
  *(uint64_t *)task->stack_pointer = 0;
  task->instruction_pointer = (void *)(uintptr_t)entry;
  // register_state[14] = RSP, register_state[16] = RIP
  task->register_state[14] = (uint64_t)(uintptr_t)task->stack_pointer;
  // register_state[15] is RBP, leave as 0
  task->register_state[16] = (uint64_t)(uintptr_t)entry;
}

static void context_switch_task_a_loop(void) {
  for (;;) {
    uint64_t observed_r10;
    uint64_t observed_r12;
    g_task_a_iterations++;
    __asm__ volatile(
        "mov $0x1122334455667788, %%r12\n\t"
        "mov $0x8877665544332211, %%r10"
        :
        :
        : "r10", "r12");
    context_switch(g_task_a, &g_kernel_context);
    __asm__ volatile(
        "mov %%r10, %0\n\t"
        "mov %%r12, %1"
        : "=r"(observed_r10), "=r"(observed_r12));
    if (observed_r10 != 0x8877665544332211ULL || observed_r12 != 0x1122334455667788ULL) {
      g_register_restore_failures++;
    }
  }
}

static void context_switch_task_b_loop(void) {
  for (;;) {
    uint64_t observed_r10;
    uint64_t observed_r12;
    g_task_b_iterations++;
    __asm__ volatile(
        "mov $0x1020304050607080, %%r12\n\t"
        "mov $0x8070605040302010, %%r10"
        :
        :
        : "r10", "r12");
    context_switch(g_task_b, &g_kernel_context);
    __asm__ volatile(
        "mov %%r10, %0\n\t"
        "mov %%r12, %1"
        : "=r"(observed_r10), "=r"(observed_r12));
    if (observed_r10 != 0x8070605040302010ULL || observed_r12 != 0x1020304050607080ULL) {
      g_register_restore_failures++;
    }
  }
}

static void test_context_switch_null_arguments(void) {
  console_write_string("[TEST] context_switch_null_arguments\r\n");
  context_switch(0, 0);
  TEST_ASSERT(1, "Null arguments return safely");
}

static void test_context_switch_round_robin_between_two_tasks(void) {
  uint64_t task_a_entry;
  uint64_t task_b_entry;
  unsigned int i;

  console_write_string("[TEST] context_switch_round_robin_between_two_tasks\r\n");

  tcb_init();
  g_task_a = tcb_create("ctx-a", 127U);
  g_task_b = tcb_create("ctx-b", 127U);
  TEST_ASSERT(g_task_a != 0, "Task A allocated");
  TEST_ASSERT(g_task_b != 0, "Task B allocated");
  if (g_task_a == 0 || g_task_b == 0) {
    return;
  }

  for (i = 0; i < 17U; ++i) {
    g_kernel_register_state[i] = 0;
  }
  g_kernel_context.task_id = 0;
  g_kernel_context.name = "kernel-context";
  g_kernel_context.stack_pointer = 0;
  g_kernel_context.instruction_pointer = 0;
  g_kernel_context.register_state = g_kernel_register_state;
  g_kernel_context.state = TASK_STATE_RUNNING;
  g_kernel_context.priority = 0;
  g_kernel_context.stack_base = 0;
  g_kernel_context.stack_size = 0;
  g_kernel_context.ticks_running = 0;
  g_kernel_context.context_switches = 0;

  context_switch_prepare_task(g_task_a, context_switch_task_a_loop);
  context_switch_prepare_task(g_task_b, context_switch_task_b_loop);

  task_a_entry = g_task_a->register_state[16];
  task_b_entry = g_task_b->register_state[16];
  g_task_a_iterations = 0;
  g_task_b_iterations = 0;
  g_register_restore_failures = 0;

  context_switch(&g_kernel_context, g_task_a);
  TEST_ASSERT(g_task_a_iterations == 1U, "Task A runs on first switch");
  TEST_ASSERT(g_task_b_iterations == 0U, "Task B has not run yet");

  context_switch(&g_kernel_context, g_task_b);
  TEST_ASSERT(g_task_b_iterations == 1U, "Task B runs on first switch");

  context_switch(&g_kernel_context, g_task_a);
  TEST_ASSERT(g_task_a_iterations == 2U, "Task A resumes on second activation");

  context_switch(&g_kernel_context, g_task_b);
  TEST_ASSERT(g_task_b_iterations == 2U, "Task B resumes on second activation");

  TEST_ASSERT(g_kernel_context.stack_pointer != 0, "Kernel context stack pointer captured");
  TEST_ASSERT(g_kernel_context.instruction_pointer != 0, "Kernel context instruction pointer captured");
  TEST_ASSERT(g_kernel_context.register_state[14] == (uint64_t)(uintptr_t)g_kernel_context.stack_pointer,
              "Kernel saved RSP matches stack pointer field");
  TEST_ASSERT(g_kernel_context.register_state[16] == (uint64_t)(uintptr_t)g_kernel_context.instruction_pointer,
              "Kernel saved RIP matches instruction pointer field");

  TEST_ASSERT(g_task_a->register_state[16] != task_a_entry, "Task A RIP advances after yielding");
  TEST_ASSERT(g_task_b->register_state[16] != task_b_entry, "Task B RIP advances after yielding");
  TEST_ASSERT(g_register_restore_failures == 0U, "Task register values survive switch round-trips");
}

void run_context_switch_tests(void) {
  console_write_string("========================================\r\n");
  console_write_string("Context Switch Test Suite\r\n");
  console_write_string("========================================\r\n");

  test_context_switch_null_arguments();
  test_context_switch_round_robin_between_two_tasks();

  console_write_string("\r\n========================================\r\n");
  console_write_string("Test Results: ");
  console_printf("%d/%d passed", results.passed, results.total);
  console_write_string("\r\n");
  console_write_string("========================================\r\n");

  tcb_init();
}
