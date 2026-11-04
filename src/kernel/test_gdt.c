#include "gdt.h"
#include "console.h"

typedef struct {
  const char *name;
  int passed;
  int total;
} TestResult;

static TestResult results = {"GDT Tests", 0, 0};

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

void test_kernel_cs_selector(void) {
  console_write_string("[TEST] kernel_cs_selector\r\n");
  
  // Kernel CS: index=1, RPL=0 → 1*8 + 0 = 0x08
  TEST_ASSERT(GDT_KERNEL_CS == 0x08U, "Kernel CS selector is 0x08");
}

void test_kernel_ds_selector(void) {
  console_write_string("[TEST] kernel_ds_selector\r\n");
  
  // Kernel DS: index=2, RPL=0 → 2*8 + 0 = 0x10
  TEST_ASSERT(GDT_KERNEL_DS == 0x10U, "Kernel DS selector is 0x10");
}

void test_user_cs_selector(void) {
  console_write_string("[TEST] user_cs_selector\r\n");
  
  // User CS: index=4, RPL=3 → 4*8 + 3 = 0x23
  TEST_ASSERT(GDT_USER_CS == 0x23U, "User CS selector is 0x23");
  
  // Verify RPL (lower 2 bits should be 3 for Ring 3)
  TEST_ASSERT((GDT_USER_CS & 3U) == 3U, "User CS has RPL=3 (Ring 3)");
}

void test_user_ds_selector(void) {
  console_write_string("[TEST] user_ds_selector\r\n");
  
  // User DS: index=3, RPL=3 → 3*8 + 3 = 0x1B
  TEST_ASSERT(GDT_USER_DS == 0x1BU, "User DS selector is 0x1B");
  
  // Verify RPL (lower 2 bits should be 3 for Ring 3)
  TEST_ASSERT((GDT_USER_DS & 3U) == 3U, "User DS has RPL=3 (Ring 3)");
}

void test_user_selectors_different(void) {
  console_write_string("[TEST] user_selectors_different\r\n");
  
  // User CS and DS should be different
  TEST_ASSERT(GDT_USER_CS != GDT_USER_DS, "User CS and DS are different selectors");
  
  // User selectors should be different from kernel selectors
  TEST_ASSERT(GDT_USER_CS != GDT_KERNEL_CS, "User CS differs from kernel CS");
  TEST_ASSERT(GDT_USER_DS != GDT_KERNEL_DS, "User DS differs from kernel DS");
}

void test_selector_calculation(void) {
  console_write_string("[TEST] selector_calculation\r\n");
  
  // Verify index calculation: selector >> 3 should give the index
  uint16_t user_cs_index = GDT_USER_CS >> 3;
  uint16_t user_ds_index = GDT_USER_DS >> 3;
  
  TEST_ASSERT(user_cs_index == 4U, "User CS index is 4");
  TEST_ASSERT(user_ds_index == 3U, "User DS index is 3");

  // SYSRET (STAR[63:48] = 0x10) loads SS = base+8 and CS = base+16.
  TEST_ASSERT((GDT_USER_DS & ~3U) == 0x10U + 8U, "User DS is SYSRET base + 8");
  TEST_ASSERT((GDT_USER_CS & ~3U) == 0x10U + 16U, "User CS is SYSRET base + 16");
}

void test_selector_rpl_bits(void) {
  console_write_string("[TEST] selector_rpl_bits\r\n");
  
  // RPL bits (lower 2 bits) should be 3 for user mode
  uint16_t user_cs_rpl = GDT_USER_CS & 0x3U;
  uint16_t user_ds_rpl = GDT_USER_DS & 0x3U;
  
  TEST_ASSERT(user_cs_rpl == 3U, "User CS RPL is 3 (user mode)");
  TEST_ASSERT(user_ds_rpl == 3U, "User DS RPL is 3 (user mode)");
  
  // Kernel selectors should have RPL=0
  uint16_t kernel_cs_rpl = GDT_KERNEL_CS & 0x3U;
  uint16_t kernel_ds_rpl = GDT_KERNEL_DS & 0x3U;
  
  TEST_ASSERT(kernel_cs_rpl == 0U, "Kernel CS RPL is 0 (kernel mode)");
  TEST_ASSERT(kernel_ds_rpl == 0U, "Kernel DS RPL is 0 (kernel mode)");
}

void test_tss_loaded(void) {
  uint16_t tr;
  uint64_t saved;

  console_write_string("[TEST] tss_loaded\r\n");

  __asm__ volatile("str %0" : "=r"(tr));
  TEST_ASSERT(tr == GDT_TSS, "Task register holds the TSS selector");

  saved = gdt_get_kernel_stack();
  gdt_set_kernel_stack(0x1234000ULL);
  TEST_ASSERT(gdt_get_kernel_stack() == 0x1234000ULL, "TSS rsp0 can be set and read back");
  gdt_set_kernel_stack(saved);
}

void run_gdt_tests(void) {
  console_write_string("========================================\r\n");
  console_write_string("GDT Test Suite\r\n");
  console_write_string("========================================\r\n");

  test_kernel_cs_selector();
  test_kernel_ds_selector();
  test_user_cs_selector();
  test_user_ds_selector();
  test_user_selectors_different();
  test_selector_calculation();
  test_selector_rpl_bits();
  test_tss_loaded();

  console_write_string("\n========================================\r\n");
  console_write_string("Test Results: ");
  console_printf("%d/%d passed", results.passed, results.total);
  console_write_string("\r\n");
  console_write_string("========================================\r\n");
}
