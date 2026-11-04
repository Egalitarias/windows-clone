#include "timer.h"
#include "console.h"

typedef struct {
  const char *name;
  int passed;
  int total;
} TestResult;

static TestResult results = {"Timer Tests", 0, 0};

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

void test_timer_init(void) {
  console_write_string("[TEST] timer_init\r\n");

  TEST_ASSERT(timer_get_ticks() == 0, "Tick counter starts at 0");
}

void test_pit_divisor(void) {
  console_write_string("[TEST] pit_divisor\r\n");
  
  TEST_ASSERT(TIMER_DIVISOR == 1193, "PIT divisor is correct for 1kHz");
  TEST_ASSERT(PIT_BASE_FREQUENCY / TIMER_DIVISOR == TIMER_TARGET_FREQUENCY, "Divisor produces target frequency");
}

void test_timer_declarations(void) {
  console_write_string("[TEST] timer_declarations\r\n");
  
  TEST_ASSERT(TIMER_TARGET_FREQUENCY == 1000, "Timer target frequency is 1kHz");
  TEST_ASSERT(PIT_BASE_FREQUENCY == 1193182, "PIT base frequency correct");
  TEST_ASSERT(PIT_CHANNEL0 == 0x40, "PIT channel 0 port correct");
  TEST_ASSERT(PIT_CONTROL == 0x43, "PIT control port correct");
}

void test_timer_ports(void) {
  console_write_string("[TEST] timer_ports\r\n");
  
  TEST_ASSERT(PIT_CHANNEL0 == 0x40, "Channel 0 port is 0x40");
  TEST_ASSERT(PIT_CHANNEL1 == 0x41, "Channel 1 port is 0x41");
  TEST_ASSERT(PIT_CHANNEL2 == 0x42, "Channel 2 port is 0x42");
  TEST_ASSERT(PIT_CONTROL == 0x43, "Control port is 0x43");
}

void test_interrupt_delivery(void) {
  console_write_string("[TEST] interrupt_delivery\r\n");
  
  uint64_t start = timer_get_ticks();
  console_printf("  Initial ticks: %llu\r\n", start);
  
  // Short busy wait - 10 million pauses (~10-100ms)
  volatile uint64_t count = 0;
  for (count = 0; count < 10000000; count++) {
    __asm__ volatile("pause");
  }
  
  uint64_t end = timer_get_ticks();
  console_printf("  Final ticks: %llu\r\n", end);
  
  uint64_t delta = end - start;
  TEST_ASSERT(delta > 0, "Timer interrupts fired (ticks incremented)");
  TEST_ASSERT(delta < 20000, "Timer tick growth remains within expected test bound");
}

void test_irq0_mask_controls(void) {
  console_write_string("[TEST] irq0_mask_controls\r\n");

  TEST_ASSERT(timer_is_irq0_masked() != 0, "IRQ0 is masked after timer_init");
  timer_unmask_irq0();
  TEST_ASSERT(timer_is_irq0_masked() == 0, "IRQ0 unmask clears PIC mask bit");
  timer_mask_irq0();
  TEST_ASSERT(timer_is_irq0_masked() != 0, "IRQ0 mask sets PIC mask bit");
  timer_unmask_irq0();
  TEST_ASSERT(timer_is_irq0_masked() == 0, "IRQ0 restored to unmasked for downstream runtime");
}

void run_timer_tests(void) {
  console_write_string("========================================\r\n");
  console_write_string("Timer Test Suite\r\n");
  console_write_string("========================================\r\n");

  // Timer tests own the hardware timer setup sequence.
  timer_init();
  
  test_timer_init();
  test_pit_divisor();
  test_timer_declarations();
  test_timer_ports();

  __asm__ volatile("sti");
  test_irq0_mask_controls();
  test_interrupt_delivery();
  
  console_write_string("\n========================================\r\n");
  console_write_string("Test Results: ");
  console_printf("%d/%d passed", results.passed, results.total);
  console_write_string("\r\n");
  console_write_string("========================================\r\n");
}
