#include "pic.h"
#include "console.h"

static void pic_outb(uint16_t port, uint8_t value) {
  __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static uint8_t pic_inb(uint16_t port) {
  uint8_t value;
  __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
  return value;
}

void pic_init(void) {
  uint8_t mask_master, mask_slave;

  // Save current masks
  mask_master = pic_inb(PIC_MASTER_DATA);
  mask_slave = pic_inb(PIC_SLAVE_DATA);

  // ICW1: Initialize master PIC
  // ICW1_INIT | ICW1_ICW4: Start initialization, will send ICW4
  pic_outb(PIC_MASTER_CMD, ICW1_INIT | ICW1_ICW4);
  
  // ICW1: Initialize slave PIC
  pic_outb(PIC_SLAVE_CMD, ICW1_INIT | ICW1_ICW4);

  // ICW2: Master PIC - remap to vectors 32-39 (IRQ0 = vector 32)
  pic_outb(PIC_MASTER_DATA, 32);
  
  // ICW2: Slave PIC - remap to vectors 40-47 (IRQ8 = vector 40)
  pic_outb(PIC_SLAVE_DATA, 40);

  // ICW3: Master PIC - slave on IRQ2
  pic_outb(PIC_MASTER_DATA, 4);
  
  // ICW3: Slave PIC - slave identity (cascade identity)
  pic_outb(PIC_SLAVE_DATA, 2);

  // ICW4: Master PIC - 8086 mode
  pic_outb(PIC_MASTER_DATA, ICW4_8086);
  
  // ICW4: Slave PIC - 8086 mode
  pic_outb(PIC_SLAVE_DATA, ICW4_8086);

  // Restore masks
  pic_outb(PIC_MASTER_DATA, mask_master);
  pic_outb(PIC_SLAVE_DATA, mask_slave);

  console_write_string("[PIC] Initialized (IRQ0-7 -> vectors 32-39, IRQ8-15 -> vectors 40-47)\r\n");
}
