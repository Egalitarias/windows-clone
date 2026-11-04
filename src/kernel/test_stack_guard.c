#include "tcb.h"
#include "stack_guard.h"
#include "console.h"

#if KERNEL_TESTS

static uint32_t g_tests_run = 0;
static uint32_t g_tests_passed = 0;

#define ASSERT(cond, msg)                                                     \
  do {                                                                          \
    ++g_tests_run;                                                             \
    if (cond) {                                                                \
      ++g_tests_passed;                                                        \
    } else {                                                                   \
      console_write_string("  [GUARD_TEST] FAIL: ");                          \
      console_write_string(msg);                                              \
      console_write_string("\r\n");                                           \
    }                                                                           \
  } while (0)

// Test 1: Guard pages allocated and tracked
static void test_guard_page_allocation(void) {
  TCB *tcb = tcb_create("guard_test_1", 50);
  ASSERT(tcb != 0, "TCB created");
  if (tcb == 0) {
    return;
  }
  ASSERT(tcb->guard_page_low != 0, "Lower guard page allocated");
  ASSERT(tcb->guard_page_high != 0, "Upper guard page allocated");
  ASSERT(tcb->stack_base != 0, "Stack base allocated");
  ASSERT(tcb->stack_pointer != 0, "Stack pointer initialized");
  ASSERT((uintptr_t)tcb->stack_base == (uintptr_t)tcb->guard_page_low + STACK_GUARD_PAGE_SIZE,
         "Stack is adjacent to lower guard");
  ASSERT((uintptr_t)tcb->guard_page_high == (uintptr_t)tcb->stack_base + STACK_GUARD_PAGE_SIZE,
         "Upper guard is adjacent to stack");
  tcb_destroy(tcb);
}

// Test 2: Stack watermarks initialized correctly
static void test_watermark_initialization(void) {
  TCB *tcb = tcb_create("guard_test_2", 50);
  ASSERT(tcb != 0, "TCB created");

  if (tcb != 0 && tcb->stack_base != 0) {
    uint64_t *watermark = (uint64_t *)tcb->stack_base;
    uint64_t *high_watermark =
        (uint64_t *)((uintptr_t)tcb->stack_base + tcb->stack_size - sizeof(uint64_t));
    ASSERT(*watermark == STACK_GUARD_WATERMARK_LOW, "Lower watermark initialized");
    ASSERT(*high_watermark == STACK_GUARD_WATERMARK_HIGH, "Upper watermark initialized");
  }

  tcb_destroy(tcb);
}

// Test 3: Guard page fields zero on cleanup
static void test_guard_page_cleanup(void) {
  TCB *tcb = tcb_create("guard_test_3", 50);
  ASSERT(tcb != 0, "TCB created");
  if (tcb == 0) {
    return;
  }

  tcb_destroy(tcb);

  // After destroy, guards should be freed
  ASSERT(tcb->guard_page_low == 0, "Lower guard page freed");
  ASSERT(tcb->guard_page_high == 0, "Upper guard page freed");
}

// Test 4: Multiple tasks have independent guards
static void test_independent_guard_pages(void) {
  TCB *tcb1 = tcb_create("guard_test_4a", 50);
  TCB *tcb2 = tcb_create("guard_test_4b", 50);

  ASSERT(tcb1 != 0, "First TCB created");
  ASSERT(tcb2 != 0, "Second TCB created");

  if (tcb1 != 0 && tcb2 != 0) {
    ASSERT(tcb1->guard_page_low != tcb2->guard_page_low, "Guard pages distinct");
    ASSERT(tcb1->stack_base != tcb2->stack_base, "Stack regions distinct");
  }

  tcb_destroy(tcb1);
  tcb_destroy(tcb2);
}

// Test 5: Stack guard initialization called
static void test_guard_init(void) {
  stack_guard_init();
  ASSERT(stack_guard_get_overflow_attempts() == 0, "Overflow counter init 0");
  ASSERT(stack_guard_get_watermark_errors() == 0, "Watermark error counter init 0");
}

// Test 6: Watermark validation on clean stack
static void test_watermark_validation_clean(void) {
  stack_guard_init();
  TCB *tcb = tcb_create("guard_test_6", 50);

  ASSERT(tcb != 0, "TCB created");

  stack_guard_check_watermarks();

  ASSERT(stack_guard_get_watermark_errors() == 0, "No watermark errors clean");

  tcb_destroy(tcb);
}

// Test 7: Stack size is correct
static void test_stack_size(void) {
  TCB *tcb = tcb_create("guard_test_7", 50);
  ASSERT(tcb != 0, "TCB created");
  if (tcb == 0) {
    return;
  }
  ASSERT(tcb->stack_size == 4096, "Stack size is 4KB");
  tcb_destroy(tcb);
}

// Test 8: Stack pointer is within bounds
static void test_stack_pointer_bounds(void) {
  TCB *tcb = tcb_create("guard_test_8", 50);
  ASSERT(tcb != 0, "TCB created");

  if (tcb != 0 && tcb->stack_base != 0 && tcb->stack_pointer != 0) {
    uint64_t base = (uint64_t)tcb->stack_base;
    uint64_t ptr = (uint64_t)tcb->stack_pointer;
    uint64_t top = base + tcb->stack_size;

    ASSERT(ptr >= base && ptr < top, "Stack pointer within bounds");
    ASSERT((ptr & 0xF) == 0x8, "Stack pointer ABI aligned");
  }

  tcb_destroy(tcb);
}

// Test 9: Register state allocated
static void test_register_state_allocation(void) {
  TCB *tcb = tcb_create("guard_test_9", 50);
  ASSERT(tcb != 0, "TCB created");
  if (tcb == 0) {
    return;
  }
  ASSERT(tcb->register_state != 0, "Register state allocated");
  tcb_destroy(tcb);
}

// Test 10: Guard pages different from stack
static void test_guard_stack_separation(void) {
  TCB *tcb = tcb_create("guard_test_10", 50);
  ASSERT(tcb != 0, "TCB created");
  if (tcb == 0) {
    return;
  }

  if (tcb != 0) {
    ASSERT(tcb->guard_page_low != tcb->stack_base, "Lower guard != stack");
    ASSERT(tcb->guard_page_high != tcb->stack_base, "Upper guard != stack");
    ASSERT(tcb->guard_page_low != tcb->guard_page_high, "Guards distinct");
  }

  tcb_destroy(tcb);
}

// Test 11: Overflow counter increments
static void test_overflow_counter(void) {
  stack_guard_init();
  uint32_t initial = stack_guard_get_overflow_attempts();
  ASSERT(initial == 0, "Initial overflow count 0");
  stack_guard_record_overflow();
  ASSERT(stack_guard_get_overflow_attempts() == 1, "Overflow count increments");
}

// Test 12: Stress test: many tasks with guards
static void test_stress_many_tasks(void) {
  TCB *tasks[16];
  uint32_t i;

  for (i = 0; i < 16; ++i) {
    tasks[i] = tcb_create("stress", 50);
    ASSERT(tasks[i] != 0, "Task created");
    if (tasks[i] != 0) {
      ASSERT(tasks[i]->guard_page_low != 0, "Task has lower guard");
      ASSERT(tasks[i]->guard_page_high != 0, "Task has upper guard");
    }
  }

  for (i = 0; i < 16; ++i) {
    if (tasks[i] != 0) {
      tcb_destroy(tasks[i]);
    }
  }
}

uint32_t run_stack_guard_tests(void) {
  console_write_string("\r\n=== Stack Guard Tests ===\r\n");

  test_guard_page_allocation();
  test_watermark_initialization();
  test_guard_page_cleanup();
  test_independent_guard_pages();
  test_guard_init();
  test_watermark_validation_clean();
  test_stack_size();
  test_stack_pointer_bounds();
  test_register_state_allocation();
  test_guard_stack_separation();
  test_overflow_counter();
  test_stress_many_tasks();

  console_write_string("Stack Guard Tests: ");
  if (g_tests_passed == g_tests_run) {
    console_write_string("PASSED\r\n\r\n");
  } else {
    console_write_string("FAILED\r\n\r\n");
  }

  return g_tests_passed == g_tests_run ? 0 : 1;
}

#else

uint32_t run_stack_guard_tests(void) {
  return 0;  // No-op in production
}

#endif
