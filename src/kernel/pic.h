#ifndef KERNEL_PIC_H
#define KERNEL_PIC_H

// PIC Master controller ports
#define PIC_MASTER_CMD   0x20
#define PIC_MASTER_DATA  0x21

// PIC Slave controller ports  
#define PIC_SLAVE_CMD    0xA0
#define PIC_SLAVE_DATA   0xA1

// ICW1 (Initialization Command Word 1)
#define ICW1_ICW4        0x01    // ICW4 needed
#define ICW1_SINGLE      0x02    // Single (no cascade)
#define ICW1_INTERVAL4   0x04    // Call address interval 4
#define ICW1_LEVEL       0x08    // Level triggered
#define ICW1_INIT        0x10    // Initialization

// ICW4
#define ICW4_8086        0x01    // 8086 mode
#define ICW4_AUTO        0x02    // Auto EOI
#define ICW4_BUF_SLAVE   0x08    // Buffered slave
#define ICW4_BUF_MASTER  0x0C    // Buffered master
#define ICW4_SFNM        0x10    // Special fully nested mode

// Reprogram PIC to map IRQ0-7 to vectors 32-39, IRQ8-15 to vectors 40-47
void pic_init(void);

#endif // KERNEL_PIC_H
