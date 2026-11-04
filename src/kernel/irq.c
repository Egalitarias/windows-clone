#include "irq.h"
#include "pic.h"

#define PIC_EOI        0x20
#define PIC_READ_ISR   0x0B

static irq_handler_t g_handlers[16];

static inline void outb(uint16_t port, uint8_t value) {
  __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
  uint8_t value;
  __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
  return value;
}

void irq_init(void) {
  unsigned int i;
  for (i = 0; i < 16; i++) {
    g_handlers[i] = 0;
  }
  outb(PIC_MASTER_DATA, 0xFF);
  outb(PIC_SLAVE_DATA, 0xFF);
}

void irq_register(unsigned int irq, irq_handler_t handler) {
  if (irq < 16) {
    g_handlers[irq] = handler;
  }
}

void irq_unmask(unsigned int irq) {
  if (irq < 8) {
    outb(PIC_MASTER_DATA, (uint8_t)(inb(PIC_MASTER_DATA) & ~(1u << irq)));
  } else if (irq < 16) {
    outb(PIC_SLAVE_DATA, (uint8_t)(inb(PIC_SLAVE_DATA) & ~(1u << (irq - 8))));
    outb(PIC_MASTER_DATA, (uint8_t)(inb(PIC_MASTER_DATA) & ~(1u << 2)));  // cascade
  }
}

void irq_mask(unsigned int irq) {
  if (irq < 8) {
    outb(PIC_MASTER_DATA, (uint8_t)(inb(PIC_MASTER_DATA) | (1u << irq)));
  } else if (irq < 16) {
    outb(PIC_SLAVE_DATA, (uint8_t)(inb(PIC_SLAVE_DATA) | (1u << (irq - 8))));
  }
}

void irq_dispatch(uint64_t irq) {
  // IRQ7 and IRQ15 can be spurious: the line dropped before we acknowledged.
  // Check the in-service register; a spurious IRQ gets no EOI (except the
  // master still needs one for a spurious slave IRQ15 on the cascade line).
  if (irq == 7) {
    outb(PIC_MASTER_CMD, PIC_READ_ISR);
    if (!(inb(PIC_MASTER_CMD) & 0x80)) {
      return;
    }
  } else if (irq == 15) {
    outb(PIC_SLAVE_CMD, PIC_READ_ISR);
    if (!(inb(PIC_SLAVE_CMD) & 0x80)) {
      outb(PIC_MASTER_CMD, PIC_EOI);
      return;
    }
  }

  if (irq < 16 && g_handlers[irq]) {
    g_handlers[irq]();
  }

  if (irq >= 8) {
    outb(PIC_SLAVE_CMD, PIC_EOI);
  }
  outb(PIC_MASTER_CMD, PIC_EOI);
}
