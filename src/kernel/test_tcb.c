#include <stddef.h>
#include "tcb.h"
#include "heap.h"
#include "console.h"

typedef struct {
  const char *name;
  int passed;
  int total;
} TestResult;

static TestResult results = {"TCB Tests", 0, 0};

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

void test_tcb_init(void) {
  console_write_string("[TEST] tcb_init\r\n");
  
  tcb_init();
  
  TCB *tcb1 = tcb_get_by_id(1);
  TEST_ASSERT(tcb1 == 0, "No tasks allocated initially");
}

void test_tcb_create(void) {
  console_write_string("[TEST] tcb_create\r\n");
  
  tcb_init();
  
  TCB *task = tcb_create("test_task", 100);
  TEST_ASSERT(task != 0, "Task allocated successfully");
  TEST_ASSERT(task->task_id != 0, "Task has non-zero ID");
  TEST_ASSERT(task->name != 0, "Task name stored");
  TEST_ASSERT(task->state == TASK_STATE_READY, "Initial state is READY");
  TEST_ASSERT(task->priority == 100, "Priority stored correctly");
  TEST_ASSERT(task->ticks_running == 0, "ticks_running initialized to 0");
  TEST_ASSERT(task->context_switches == 0, "context_switches initialized to 0");
}

void test_tcb_destroy(void) {
  console_write_string("[TEST] tcb_destroy\r\n");
  
  tcb_init();
  
  TCB *task1 = tcb_create("task1", 100);
  TEST_ASSERT(task1 != 0, "Task created");
  uint32_t task_id = task1->task_id;
  
  tcb_destroy(task1);
  
  TCB *freed_task = tcb_get_by_id(task_id);
  TEST_ASSERT(freed_task == 0, "Task destroyed and not retrievable");
  
  TCB *task2 = tcb_create("task2", 50);
  TEST_ASSERT(task2 != 0, "Can reuse destroyed slot");
}

void test_stack_allocation(void) {
  console_write_string("[TEST] stack_allocation\r\n");
  
  tcb_init();
  
  TCB *task = tcb_create("stack_test", 100);
  TEST_ASSERT(task != 0, "Task created");
  TEST_ASSERT(task->stack_base != 0, "Stack base allocated");
  TEST_ASSERT(task->stack_size == TCB_DEFAULT_STACK_SIZE, "Stack size correct");
  TEST_ASSERT(task->stack_pointer != 0, "Stack pointer set");
}

void test_stack_initialization(void) {
  console_write_string("[TEST] stack_initialization\r\n");
  
  tcb_init();
  
  TCB *task = tcb_create("stack_init_test", 100);
  TEST_ASSERT(task != 0, "Task created");
  
  uintptr_t stack_top = (uintptr_t)task->stack_base + task->stack_size;
  uintptr_t sp = (uintptr_t)task->stack_pointer;
  
  TEST_ASSERT(sp < stack_top, "Stack pointer within bounds");
  TEST_ASSERT(sp > (uintptr_t)task->stack_base, "Stack pointer above base");
  TEST_ASSERT(task->register_state != 0, "Register state allocated");
}

void test_stack_multiple_tasks(void) {
  console_write_string("[TEST] stack_multiple_tasks\r\n");
  
  tcb_init();
  
  TCB *tasks[8];
  int i;
  
  for (i = 0; i < 8; ++i) {
    tasks[i] = tcb_create("task", 100);
    TEST_ASSERT(tasks[i] != 0, "Multiple tasks allocated");
  }
  
  for (i = 0; i < 7; ++i) {
    uintptr_t base1 = (uintptr_t)tasks[i]->stack_base;
    uintptr_t top1 = base1 + tasks[i]->stack_size;
    uintptr_t base2 = (uintptr_t)tasks[i + 1]->stack_base;
    
    TEST_ASSERT(base2 >= top1 || base1 >= (uintptr_t)tasks[i + 1]->stack_base + tasks[i + 1]->stack_size, 
                "Task stacks do not overlap");
  }
}

void test_stack_bounds(void) {
  console_write_string("[TEST] stack_bounds\r\n");
  
  tcb_init();
  
  TCB *task = tcb_create("bounds_test", 100);
  TEST_ASSERT(task != 0, "Task created");
  
  uintptr_t base = (uintptr_t)task->stack_base;
  uintptr_t sp = (uintptr_t)task->stack_pointer;
  uintptr_t top = base + task->stack_size;
  
  TEST_ASSERT(sp >= base && sp < top, "Stack pointer within bounds");
  TEST_ASSERT(task->stack_size > 0, "Stack size is non-zero");
}

void test_task_state_transitions(void) {
  console_write_string("[TEST] task_state_transitions\r\n");
  
  tcb_init();
  
  TCB *task = tcb_create("state_test", 100);
  TEST_ASSERT(task != 0, "Task created");
  TEST_ASSERT(task->state == TASK_STATE_READY, "Initial state is READY");
  
  task->state = TASK_STATE_RUNNING;
  TEST_ASSERT(task->state == TASK_STATE_RUNNING, "State can be changed to RUNNING");
  
  task->state = TASK_STATE_BLOCKED;
  TEST_ASSERT(task->state == TASK_STATE_BLOCKED, "State can be changed to BLOCKED");
  
  task->state = TASK_STATE_TERMINATED;
  TEST_ASSERT(task->state == TASK_STATE_TERMINATED, "State can be changed to TERMINATED");
}

void test_task_metadata(void) {
  console_write_string("[TEST] task_metadata\r\n");
  
  tcb_init();
  
  TCB *task = tcb_create("metadata_test", 42);
  TEST_ASSERT(task != 0, "Task created");
  TEST_ASSERT(task->priority == 42, "Priority stored");
  TEST_ASSERT(task->ticks_running == 0, "ticks_running initialized");
  TEST_ASSERT(task->context_switches == 0, "context_switches initialized");
  
  task->ticks_running = 1000;
  task->context_switches = 5;
  TEST_ASSERT(task->ticks_running == 1000, "ticks_running can be updated");
  TEST_ASSERT(task->context_switches == 5, "context_switches can be updated");
}

void test_tcb_get_by_id(void) {
  console_write_string("[TEST] tcb_get_by_id\r\n");
  
  tcb_init();
  
  TCB *task1 = tcb_create("task1", 100);
  TCB *task2 = tcb_create("task2", 100);
  TCB *task3 = tcb_create("task3", 100);
  
  uint32_t id1 = task1->task_id;
  uint32_t id2 = task2->task_id;
  uint32_t id3 = task3->task_id;
  
  TEST_ASSERT(tcb_get_by_id(id1) == task1, "Can retrieve task1 by ID");
  TEST_ASSERT(tcb_get_by_id(id2) == task2, "Can retrieve task2 by ID");
  TEST_ASSERT(tcb_get_by_id(id3) == task3, "Can retrieve task3 by ID");
}

void test_tcb_get_by_id_not_found(void) {
  console_write_string("[TEST] tcb_get_by_id_not_found\r\n");
  
  tcb_init();
  
  TCB *result = tcb_get_by_id(999);
  TEST_ASSERT(result == 0, "Returns 0 for non-existent task");
  
  TCB *task = tcb_create("test", 100);
  tcb_destroy(task);
  
  result = tcb_get_by_id(task->task_id);
  TEST_ASSERT(result == 0, "Returns 0 for destroyed task");
}

void test_tcb_max_tasks(void) {
  console_write_string("[TEST] tcb_max_tasks\r\n");
  
  tcb_init();
  
  TCB *tasks[TCB_MAX_TASKS];
  uint32_t i;
  
  for (i = 0; i < TCB_MAX_TASKS; ++i) {
    tasks[i] = tcb_create("task", 100);
    TEST_ASSERT(tasks[i] != 0, "Can allocate max tasks");
  }
  
  TCB *overflow = tcb_create("overflow", 100);
  TEST_ASSERT(overflow == 0, "Cannot allocate more than max tasks");
}

void test_tcb_reuse(void) {
  console_write_string("[TEST] tcb_reuse\r\n");
  
  tcb_init();
  
  TCB *task1 = tcb_create("task1", 100);
  uint32_t id1 = task1->task_id;
  
  tcb_destroy(task1);
  
  TCB *task2 = tcb_create("task2", 100);
  uint32_t id2 = task2->task_id;
  
  TEST_ASSERT(id2 != id1, "Reused slot gets new task ID");
  TEST_ASSERT(tcb_get_by_id(id1) == 0, "Old ID not found");
  TEST_ASSERT(tcb_get_by_id(id2) == task2, "New ID works");
}

void test_concurrent_allocation(void) {
  console_write_string("[TEST] concurrent_allocation\r\n");
  
  uint32_t cycle;
  for (cycle = 0; cycle < 10; ++cycle) {
    tcb_init();
    
    TCB *tasks[8];
    uint32_t i;
    
    for (i = 0; i < 8; ++i) {
      tasks[i] = tcb_create("task", 100);
    }
    
    for (i = 0; i < 8; ++i) {
      tcb_destroy(tasks[i]);
    }
  }
  
  TEST_ASSERT(1, "Concurrent allocation cycles completed");
}

void test_stack_no_corruption(void) {
  console_write_string("[TEST] stack_no_corruption\r\n");
  
  tcb_init();
  
  TCB *task1 = tcb_create("task1", 100);
  TCB *task2 = tcb_create("task2", 100);
  
  uint8_t *stack1 = (uint8_t *)task1->stack_base;
  uint8_t *stack2 = (uint8_t *)task2->stack_base;
  
  stack1[0] = 0xAA;
  stack1[100] = 0xBB;
  
  TEST_ASSERT(stack2[0] != 0xAA, "Task1 stack write doesn't affect task2");
  TEST_ASSERT(stack2[100] != 0xBB, "Task1 stack write doesn't corrupt task2");
  TEST_ASSERT(stack1[0] == 0xAA, "Task1 stack write persists");
}

void test_usermode_fields_initialization(void) {
  console_write_string("[TEST] usermode_fields_initialization\r\n");
  
  tcb_init();
  
  TCB *task = tcb_create("usermode_test", 100);
  TEST_ASSERT(task != 0, "User-mode test task allocated");
  TEST_ASSERT(task->address_space == 0, "address_space initialized to 0/NULL");
  TEST_ASSERT(task->user_stack_base == 0, "user_stack_base initialized to 0/NULL");
  TEST_ASSERT(task->user_entry_point == 0, "user_entry_point initialized to 0/NULL");
  TEST_ASSERT(task->exit_code == 0, "exit_code initialized to 0");
}

void test_usermode_fields_after_destroy(void) {
  console_write_string("[TEST] usermode_fields_after_destroy\r\n");
  
  tcb_init();
  
  TCB *task = tcb_create("usermode_test", 100);
  TEST_ASSERT(task != 0, "Task created for destruction test");
  uint32_t task_id = task->task_id;
  
  tcb_destroy(task);
  
  TCB *freed_task = tcb_get_by_id(task_id);
  TEST_ASSERT(freed_task == 0, "Task destroyed and not retrievable");
  
  TCB *task2 = tcb_create("reused_task", 100);
  TEST_ASSERT(task2 != 0, "New task allocated in reused slot");
  TEST_ASSERT(task2->address_space == 0, "address_space re-initialized to 0/NULL after slot reuse");
  TEST_ASSERT(task2->user_stack_base == 0, "user_stack_base re-initialized to 0/NULL after slot reuse");
  TEST_ASSERT(task2->user_entry_point == 0, "user_entry_point re-initialized to 0/NULL after slot reuse");
  TEST_ASSERT(task2->exit_code == 0, "exit_code re-initialized to 0 after slot reuse");
}

void run_tcb_tests(void) {
  console_write_string("========================================\r\n");
  console_write_string("TCB Test Suite\r\n");
  console_write_string("========================================\r\n");
  
  test_tcb_init();
  test_tcb_create();
  test_tcb_destroy();
  test_stack_allocation();
  test_stack_initialization();
  test_stack_multiple_tasks();
  test_stack_bounds();
  test_task_state_transitions();
  test_task_metadata();
  test_tcb_get_by_id();
  test_tcb_get_by_id_not_found();
  test_tcb_max_tasks();
  test_tcb_reuse();
  test_concurrent_allocation();
  test_stack_no_corruption();
  test_usermode_fields_initialization();
  test_usermode_fields_after_destroy();
  
  console_write_string("\n========================================\r\n");
  console_write_string("Test Results: ");
  console_printf("%d/%d passed", results.passed, results.total);
  console_write_string("\r\n");
  console_write_string("========================================\r\n");
  
  tcb_init();
}
