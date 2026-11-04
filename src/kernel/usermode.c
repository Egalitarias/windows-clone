#include "usermode.h"
#include "gdt.h"

__attribute__((noreturn)) void user_enter(uint64_t entry, uint64_t user_rsp) {
  __asm__ volatile(
      "movw %[uds], %%ax\n\t"
      "movw %%ax, %%ds\n\t"
      "movw %%ax, %%es\n\t"
      "pushq %[uds64]\n\t"       // SS
      "pushq %[rsp]\n\t"         // RSP
      "pushq $0x202\n\t"         // RFLAGS: IF=1, reserved bit 1
      "pushq %[ucs64]\n\t"       // CS
      "pushq %[rip]\n\t"         // RIP
      "iretq\n\t"
      :
      : [uds] "i"(GDT_USER_DS),
        [uds64] "i"((uint64_t)GDT_USER_DS),
        [ucs64] "i"((uint64_t)GDT_USER_CS),
        [rsp] "r"(user_rsp),
        [rip] "r"(entry)
      : "rax", "memory");
  __builtin_unreachable();
}

#ifdef KERNEL_USER_SELFTEST
#include "console.h"
#include "paging.h"
#include "pmm.h"

// Virtual addresses far from anything the kernel identity-maps.
#define USER_TEST_CODE  0x00007F0000001000ULL
#define USER_TEST_STACK 0x00007F0000010000ULL  // top of a one-page stack

static uint8_t g_user_test_kstack[16384] __attribute__((aligned(16)));

__attribute__((noreturn)) void usermode_selftest(void) {
  uint64_t code_phys = pmm_alloc_page();
  uint64_t stack_phys = pmm_alloc_page();
  volatile uint8_t *code;

  console_write_string("[USER] ring-3 self-test: user program will execute HLT\r\n");
  if (code_phys == 0 || stack_phys == 0 ||
      !paging_map_range(code_phys, 4096) || !paging_map_range(stack_phys, 4096)) {
    console_write_string("[USER] FAIL: could not allocate pages\r\n");
    for (;;) { __asm__ volatile("hlt"); }
  }

  // User program: hlt; jmp . -- hlt is privileged and must fault at CPL 3.
  code = (volatile uint8_t *)(uintptr_t)code_phys;
  code[0] = 0xF4;
  code[1] = 0xEB;
  code[2] = 0xFE;

  if (!paging_map_user(USER_TEST_CODE, code_phys, 4096, 0) ||
      !paging_map_user(USER_TEST_STACK - 4096, stack_phys, 4096, 1)) {
    console_write_string("[USER] FAIL: could not map user pages\r\n");
    for (;;) { __asm__ volatile("hlt"); }
  }

  gdt_set_kernel_stack((uint64_t)(uintptr_t)(g_user_test_kstack + sizeof(g_user_test_kstack)));
  console_printf("[USER] entering ring 3 at 0x%llx\r\n", USER_TEST_CODE);
  user_enter(USER_TEST_CODE, USER_TEST_STACK);
}
#endif
