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

// uint64_t user_run(uint64_t entry /*rdi*/, uint64_t user_rsp /*rsi*/, uint64_t *saved_rsp /*rdx*/)
// Saves flags and callee-saved registers on the kernel stack, records RSP in
// *saved_rsp, then iretq's into ring 3. user_return() unwinds that frame.
__asm__(
    ".text\n"
    ".globl user_run\n"
    "user_run:\n"
    "    pushfq\n"
    "    push %rbx\n"
    "    push %rbp\n"
    "    push %r12\n"
    "    push %r13\n"
    "    push %r14\n"
    "    push %r15\n"
    "    mov %rsp, (%rdx)\n"
    "    mov $0x1B, %eax\n"
    "    mov %ax, %ds\n"
    "    mov %ax, %es\n"
    "    push $0x1B\n"          // SS
    "    push %rsi\n"           // user RSP
    "    push $0x202\n"         // RFLAGS: IF=1
    "    push $0x23\n"          // CS
    "    push %rdi\n"           // RIP
    "    iretq\n"
    ".globl user_return\n"
    "user_return:\n"
    "    mov %rdi, %rsp\n"
    "    mov %rsi, %rax\n"
    "    mov $0x10, %ecx\n"
    "    mov %cx, %ds\n"
    "    mov %cx, %es\n"
    "    pop %r15\n"
    "    pop %r14\n"
    "    pop %r13\n"
    "    pop %r12\n"
    "    pop %rbp\n"
    "    pop %rbx\n"
    "    popfq\n"
    "    ret\n");

#if defined(KERNEL_USER_SELFTEST) && (KERNEL_USER_SELFTEST == 1 || KERNEL_USER_SELFTEST == 2)
#include "console.h"
#include "paging.h"
#include "pmm.h"
#include "syscall.h"

// Virtual addresses far from anything the kernel identity-maps.
#define USER_TEST_CODE  0x00007F0000001000ULL
#define USER_TEST_STACK 0x00007F0000010000ULL  // top of a one-page stack

#define USER_TEST_EXIT_PASS 42

static uint8_t g_user_test_kstack[16384] __attribute__((aligned(16)));

// The user program is assembled into the kernel image and copied to a user page.
// Hidden visibility makes the compiler use RIP-relative addressing: the kernel is a
// PIE but the loader applies no relocations, so GOT entries cannot be trusted.
// It is position independent (RIP-relative only).
extern const uint8_t user_test_start[] __attribute__((visibility("hidden")));
extern const uint8_t user_test_end[] __attribute__((visibility("hidden")));

#if KERNEL_USER_SELFTEST == 2
// hlt is privileged: expect a #GP reported with CS=0x23.
__asm__(
    ".section .rodata\n"
    ".globl user_test_start\n"
    ".globl user_test_end\n"
    "user_test_start:\n"
    "    hlt\n"
    "    jmp user_test_start\n"
    "user_test_end:\n"
    ".previous\n");
#else
// Exercises the syscall path from ring 3:
//   write(1, msg, len)          -> must print and return len
//   getpid()                    -> must return 1
//   write(1, kernel_ptr, 4)     -> must fail with -EFAULT (-14)
//   write(7, msg, 1)            -> must fail with -EBADF (-9)
//   unknown syscall 999         -> must fail with -ENOSYS (-38)
//   exit(42) on success, exit(1..5) identifies the failing step.
__asm__(
    ".section .rodata\n"
    ".globl user_test_start\n"
    ".globl user_test_end\n"
    "user_test_start:\n"
    "    mov $1, %eax\n"
    "    mov $1, %edi\n"
    "    lea 9f(%rip), %rsi\n"
    "    mov $(10f - 9f), %edx\n"
    "    syscall\n"
    "    cmp $(10f - 9f), %rax\n"
    "    jne 1f\n"
    "    mov $3, %eax\n"
    "    syscall\n"
    "    cmp $1, %rax\n"
    "    jne 2f\n"
    "    mov $1, %eax\n"
    "    mov $1, %edi\n"
    "    movabs $0xFFFF800000000000, %rsi\n"
    "    mov $4, %edx\n"
    "    syscall\n"
    "    cmp $-14, %rax\n"
    "    jne 3f\n"
    "    mov $1, %eax\n"
    "    mov $7, %edi\n"
    "    lea 9f(%rip), %rsi\n"
    "    mov $1, %edx\n"
    "    syscall\n"
    "    cmp $-9, %rax\n"
    "    jne 4f\n"
    "    mov $999, %eax\n"
    "    syscall\n"
    "    cmp $-38, %rax\n"
    "    jne 5f\n"
    "    mov $0, %eax\n"
    "    mov $42, %edi\n"
    "    syscall\n"
    "1:  mov $1, %edi\n"
    "    jmp 6f\n"
    "2:  mov $2, %edi\n"
    "    jmp 6f\n"
    "3:  mov $3, %edi\n"
    "    jmp 6f\n"
    "4:  mov $4, %edi\n"
    "    jmp 6f\n"
    "5:  mov $5, %edi\n"
    "6:  mov $0, %eax\n"
    "    syscall\n"
    "    jmp 6b\n"
    "9:  .ascii \"[USER] hello from ring 3 via SYSCALL\\r\\n\"\n"
    "10:\n"
    "user_test_end:\n"
    ".previous\n");
#endif

static void user_test_exit(uint64_t code) {
  if (code == USER_TEST_EXIT_PASS) {
    console_write_string("[USER] PASS: all syscall checks succeeded (exit 42)\r\n");
  } else {
    console_printf("[USER] FAIL: user program exited with %llu (step that failed)\r\n", code);
  }
}

__attribute__((noreturn)) void usermode_selftest(void) {
  uint64_t code_phys = pmm_alloc_page();
  uint64_t stack_phys = pmm_alloc_page();
  volatile uint8_t *code;
  uint64_t size = (uint64_t)(user_test_end - user_test_start);
  uint64_t i;

  console_write_string("[USER] ring-3 self-test\r\n");
  if (code_phys == 0 || stack_phys == 0 || size > 4096 ||
      !paging_map_range(code_phys, 4096) || !paging_map_range(stack_phys, 4096)) {
    console_printf("[USER] FAIL: could not allocate pages (code=0x%llx stack=0x%llx size=%llu)\r\n",
                   code_phys, stack_phys, size);
    for (;;) { __asm__ volatile("hlt"); }
  }

  code = (volatile uint8_t *)(uintptr_t)code_phys;
  for (i = 0; i < size; ++i) {
    code[i] = user_test_start[i];
  }

  if (!paging_map_user(USER_TEST_CODE, code_phys, 4096, 0) ||
      !paging_map_user(USER_TEST_STACK - 4096, stack_phys, 4096, 1)) {
    console_write_string("[USER] FAIL: could not map user pages\r\n");
    for (;;) { __asm__ volatile("hlt"); }
  }

  gdt_set_kernel_stack((uint64_t)(uintptr_t)(g_user_test_kstack + sizeof(g_user_test_kstack)));
  syscall_set_exit_handler(user_test_exit);
  console_printf("[USER] entering ring 3 at 0x%llx\r\n", USER_TEST_CODE);
  user_enter(USER_TEST_CODE, USER_TEST_STACK);
}
#endif
