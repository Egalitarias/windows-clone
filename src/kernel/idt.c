#include "idt.h"
#include "process.h"

#define IDT_ENTRY_COUNT 256U
#define IDT_GATE_TYPE_INTERRUPT 0x8EU
#define IDT_GATE_TYPE_TRAP 0x8FU

typedef struct {
  uint16_t OffsetLow;
  uint16_t Selector;
  uint8_t Ist;
  uint8_t TypeAttr;
  uint16_t OffsetMid;
  uint32_t OffsetHigh;
  uint32_t Zero;
} __attribute__((packed)) IDTEntry;

typedef struct {
  uint16_t Limit;
  uint64_t Base;
} __attribute__((packed)) IDTPointer;

extern void exception_stub_0(void);
extern void exception_stub_1(void);
extern void exception_stub_2(void);
extern void exception_stub_3(void);
extern void exception_stub_4(void);
extern void exception_stub_5(void);
extern void exception_stub_6(void);
extern void exception_stub_7(void);
extern void exception_stub_8(void);
extern void exception_stub_9(void);
extern void exception_stub_10(void);
extern void exception_stub_11(void);
extern void exception_stub_12(void);
extern void exception_stub_13(void);
extern void exception_stub_14(void);
extern void exception_stub_15(void);
extern void exception_stub_16(void);
extern void exception_stub_17(void);
extern void exception_stub_18(void);
extern void exception_stub_19(void);
extern void exception_stub_20(void);
extern void exception_stub_21(void);
extern void exception_stub_22(void);
extern void exception_stub_23(void);
extern void exception_stub_24(void);
extern void exception_stub_25(void);
extern void exception_stub_26(void);
extern void exception_stub_27(void);
extern void exception_stub_28(void);
extern void exception_stub_29(void);
extern void exception_stub_30(void);
extern void exception_stub_31(void);

extern void timer_interrupt_stub(void);

extern void irq_stub_1(void);
extern void irq_stub_2(void);
extern void irq_stub_3(void);
extern void irq_stub_4(void);
extern void irq_stub_5(void);
extern void irq_stub_6(void);
extern void irq_stub_7(void);
extern void irq_stub_8(void);
extern void irq_stub_9(void);
extern void irq_stub_10(void);
extern void irq_stub_11(void);
extern void irq_stub_12(void);
extern void irq_stub_13(void);
extern void irq_stub_14(void);
extern void irq_stub_15(void);

static IDTEntry g_idt[IDT_ENTRY_COUNT];
static IDTPointer g_idt_ptr;

static uint16_t read_cs_selector(void) {
  uint16_t selector;

  __asm__ volatile("mov %%cs, %0" : "=r"(selector));
  return selector;
}

static void idt_set_gate_with_type(unsigned int vector,
                                   void *handler,
                                   uint16_t selector,
                                   uint8_t type_attr) {
  uint64_t addr = (uint64_t)(uintptr_t)handler;

  g_idt[vector].OffsetLow = (uint16_t)(addr & 0xFFFFULL);
  g_idt[vector].Selector = selector;
  g_idt[vector].Ist = 0;
  g_idt[vector].TypeAttr = type_attr;
  g_idt[vector].OffsetMid = (uint16_t)((addr >> 16) & 0xFFFFULL);
  g_idt[vector].OffsetHigh = (uint32_t)((addr >> 32) & 0xFFFFFFFFULL);
  g_idt[vector].Zero = 0;
}

static void idt_set_gate(unsigned int vector, void *handler, uint16_t selector) {
  idt_set_gate_with_type(vector, handler, selector, IDT_GATE_TYPE_INTERRUPT);
}

__attribute__((noreturn)) void exception_dispatch(uint64_t vector, uint64_t error_code, const uint64_t *frame) {
  if (frame != 0) {
    int err_pushed = (vector == 8 || (vector >= 10 && vector <= 14) || vector == 17 || vector == 30);
    const uint64_t *cpu = err_pushed ? frame + 1 : frame;

    // A fault in a user program must not take the machine down: kill the
    // process and return to whoever ran it. Does not return if one was running.
    if ((cpu[1] & 3ULL) == 3ULL) {
      uint64_t cr2;
      __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
      process_handle_user_fault(vector, error_code, cpu[0], cr2);
    } else {
      // A fault in the kernel while it was serving a process's syscall: kill that
      // process rather than the machine. Does not return when it applies.
      uint64_t cr2;
      __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
      process_handle_kernel_fault(vector, error_code, cpu[0], cr2);
    }
  }

  console_write_string("\r\n*** EXCEPTION ***\r\n");
  console_printf("Vector: %llu\r\n", vector);
  console_printf("Error:  0x%llx\r\n", error_code);
  if (frame != 0) {
    // The stubs pass a pointer to the CPU-pushed frame. Vectors that push an
    // error code have it at frame[0]; RIP, CS, RFLAGS, RSP, SS follow.
    int has_error = (vector == 8 || (vector >= 10 && vector <= 14) || vector == 17 || vector == 30);
    const uint64_t *f = has_error ? frame + 1 : frame;
    console_printf("RIP:    0x%llx\r\n", f[0]);
    console_printf("CS:     0x%llx (%s)\r\n", f[1], (f[1] & 3ULL) == 3ULL ? "ring 3" : "ring 0");
    console_printf("RSP:    0x%llx\r\n", f[3]);
  }
  console_write_string("System halted.\r\n");

  __asm__ volatile("cli");
  for (;;) {
    __asm__ volatile("pause");
  }
}

void idt_init(void) {
  unsigned int i;
  uint16_t selector;

  // Disable hardware interrupts while we install our IDT.  UEFI hardware
  // IRQs would otherwise fire at our default stub (exception_stub_0) and be
  // misreported as Vector 0 exceptions.  We have no use for timer/device
  // interrupts this early in boot.
  __asm__ volatile("cli");

  selector = read_cs_selector();
  for (i = 0; i < IDT_ENTRY_COUNT; ++i) {
    idt_set_gate(i, (void *)exception_stub_0, selector);
  }

  idt_set_gate(0, (void *)exception_stub_0, selector);
  idt_set_gate(1, (void *)exception_stub_1, selector);
  idt_set_gate(2, (void *)exception_stub_2, selector);
  idt_set_gate(3, (void *)exception_stub_3, selector);
  idt_set_gate(4, (void *)exception_stub_4, selector);
  idt_set_gate(5, (void *)exception_stub_5, selector);
  idt_set_gate(6, (void *)exception_stub_6, selector);
  idt_set_gate(7, (void *)exception_stub_7, selector);
  idt_set_gate(8, (void *)exception_stub_8, selector);
  idt_set_gate(9, (void *)exception_stub_9, selector);
  idt_set_gate(10, (void *)exception_stub_10, selector);
  idt_set_gate(11, (void *)exception_stub_11, selector);
  idt_set_gate(12, (void *)exception_stub_12, selector);
  idt_set_gate(13, (void *)exception_stub_13, selector);
  idt_set_gate(14, (void *)exception_stub_14, selector);
  idt_set_gate(15, (void *)exception_stub_15, selector);
  idt_set_gate(16, (void *)exception_stub_16, selector);
  idt_set_gate(17, (void *)exception_stub_17, selector);
  idt_set_gate(18, (void *)exception_stub_18, selector);
  idt_set_gate(19, (void *)exception_stub_19, selector);
  idt_set_gate(20, (void *)exception_stub_20, selector);
  idt_set_gate(21, (void *)exception_stub_21, selector);
  idt_set_gate(22, (void *)exception_stub_22, selector);
  idt_set_gate(23, (void *)exception_stub_23, selector);
  idt_set_gate(24, (void *)exception_stub_24, selector);
  idt_set_gate(25, (void *)exception_stub_25, selector);
  idt_set_gate(26, (void *)exception_stub_26, selector);
  idt_set_gate(27, (void *)exception_stub_27, selector);
  idt_set_gate(28, (void *)exception_stub_28, selector);
  idt_set_gate(29, (void *)exception_stub_29, selector);
  idt_set_gate(30, (void *)exception_stub_30, selector);
  idt_set_gate(31, (void *)exception_stub_31, selector);

  idt_set_gate_with_type(32, (void *)timer_interrupt_stub, selector, IDT_GATE_TYPE_TRAP);

  // Remaining PIC lines go through the generic IRQ dispatcher. Without these,
  // a stray hardware interrupt would land on the exception stub and be
  // misreported as a CPU exception.
  idt_set_gate(33, (void *)irq_stub_1, selector);
  idt_set_gate(34, (void *)irq_stub_2, selector);
  idt_set_gate(35, (void *)irq_stub_3, selector);
  idt_set_gate(36, (void *)irq_stub_4, selector);
  idt_set_gate(37, (void *)irq_stub_5, selector);
  idt_set_gate(38, (void *)irq_stub_6, selector);
  idt_set_gate(39, (void *)irq_stub_7, selector);
  idt_set_gate(40, (void *)irq_stub_8, selector);
  idt_set_gate(41, (void *)irq_stub_9, selector);
  idt_set_gate(42, (void *)irq_stub_10, selector);
  idt_set_gate(43, (void *)irq_stub_11, selector);
  idt_set_gate(44, (void *)irq_stub_12, selector);
  idt_set_gate(45, (void *)irq_stub_13, selector);
  idt_set_gate(46, (void *)irq_stub_14, selector);
  idt_set_gate(47, (void *)irq_stub_15, selector);

  g_idt_ptr.Limit = (uint16_t)(sizeof(g_idt) - 1U);
  g_idt_ptr.Base = (uint64_t)(uintptr_t)g_idt;

  __asm__ volatile("lidt %0" : : "m"(g_idt_ptr));
}

void idt_set_handler(unsigned int vector, void *handler) {
  if (vector >= IDT_ENTRY_COUNT) {
    return;
  }
  // Keep whatever selector the table currently uses (it changes when the
  // kernel installs its own GDT).
  idt_set_gate(vector, handler, g_idt[vector].Selector);
}

void idt_reload_selector(uint16_t new_cs) {
  unsigned int i;

  for (i = 0; i < IDT_ENTRY_COUNT; ++i) {
    g_idt[i].Selector = new_cs;
  }
  __asm__ volatile("lidt %0" : : "m"(g_idt_ptr));
}

void idt_debug_dump_vector(unsigned int vector) {
  if (vector >= IDT_ENTRY_COUNT) {
    console_write_string("idt_debug_dump_vector: vector out of range\r\n");
    return;
  }
  
  IDTEntry *entry = &g_idt[vector];
  console_write_string("IDT Entry ");
  console_printf("%d", vector);
  console_write_string(":\r\n");
  
  uint64_t handler = ((uint64_t)entry->OffsetHigh << 32) | 
                     ((uint64_t)entry->OffsetMid << 16) | 
                     (uint64_t)entry->OffsetLow;
  
  console_printf("  Handler: 0x%llx\r\n", handler);
  console_printf("  Selector: 0x%x\r\n", entry->Selector);
  console_printf("  Type/Attr: 0x%x\r\n", entry->TypeAttr);
  console_printf("  IST: %d\r\n", entry->Ist);
}
