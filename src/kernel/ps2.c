#include "ps2.h"
#include "irq.h"

#define PS2_DATA   0x60
#define PS2_STATUS 0x64
#define PS2_CMD    0x64

#define PS2_STATUS_OUT_FULL 0x01
#define PS2_STATUS_IN_FULL  0x02
#define PS2_STATUS_AUX      0x20

#define PS2_SPIN_LIMIT 200000

static inline void outb(uint16_t port, uint8_t value) {
  __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
  uint8_t value;
  __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
  return value;
}

// ---- Interrupt side ---------------------------------------------------------
// Single producer (the IRQ handlers) and single consumer (ps2_poll): the
// producer only moves g_head, the consumer only moves g_tail.

#define RING_SIZE 256
#define RING_AUX  0x100

static volatile uint16_t g_ring[RING_SIZE];
static volatile unsigned int g_head = 0;
static volatile unsigned int g_tail = 0;

static void ring_push(uint8_t byte, int aux) {
  unsigned int next = (g_head + 1) % RING_SIZE;
  if (next == g_tail) {
    return;  // full: drop the byte
  }
  g_ring[g_head] = (uint16_t)(byte | (aux ? RING_AUX : 0));
  g_head = next;
}

// Both lines share the data port, so either handler drains whatever is pending
// and tags each byte with its source from the status register.
static void drain_controller(void) {
  int guard;
  for (guard = 0; guard < 32; guard++) {
    uint8_t status = inb(PS2_STATUS);
    if (!(status & PS2_STATUS_OUT_FULL)) {
      return;
    }
    ring_push(inb(PS2_DATA), (status & PS2_STATUS_AUX) != 0);
  }
}

static void keyboard_irq(void) { drain_controller(); }
static void mouse_irq(void) { drain_controller(); }

static int wait_write_ready(void) {
  int i;
  for (i = 0; i < PS2_SPIN_LIMIT; i++) {
    if (!(inb(PS2_STATUS) & PS2_STATUS_IN_FULL)) {
      return 1;
    }
  }
  return 0;
}

static int wait_read_ready(void) {
  int i;
  for (i = 0; i < PS2_SPIN_LIMIT; i++) {
    if (inb(PS2_STATUS) & PS2_STATUS_OUT_FULL) {
      return 1;
    }
  }
  return 0;
}

static void controller_cmd(uint8_t cmd) {
  if (wait_write_ready()) {
    outb(PS2_CMD, cmd);
  }
}

static void controller_cmd_arg(uint8_t cmd, uint8_t arg) {
  controller_cmd(cmd);
  if (wait_write_ready()) {
    outb(PS2_DATA, arg);
  }
}

static void flush_output(void) {
  int guard = 0;
  while ((inb(PS2_STATUS) & PS2_STATUS_OUT_FULL) && guard++ < 64) {
    (void)inb(PS2_DATA);
  }
}

// Send a byte to the keyboard (port 1). Returns 1 if it was acknowledged.
static int keyboard_send(uint8_t value) {
  if (!wait_write_ready()) {
    return 0;
  }
  outb(PS2_DATA, value);
  if (!wait_read_ready()) {
    return 0;
  }
  return inb(PS2_DATA) == 0xFA;
}

// Send a byte to the mouse (port 2). Returns 1 if it was acknowledged.
static int mouse_send(uint8_t value) {
  controller_cmd_arg(0xD4, value);
  if (!wait_read_ready()) {
    return 0;
  }
  return inb(PS2_DATA) == 0xFA;
}

int ps2_init(void) {
  int found = 0;
  uint8_t config;

  controller_cmd(0xAD);  // disable keyboard port
  controller_cmd(0xA7);  // disable mouse port
  flush_output();

  controller_cmd(0x20);  // read configuration byte
  config = wait_read_ready() ? inb(PS2_DATA) : 0;
  config &= (uint8_t)~0x03;  // IRQs stay off while we talk to the devices
  config &= (uint8_t)~0x30;  // enable both port clocks
  config |= 0x40;            // translate keyboard to scancode set 1
  controller_cmd_arg(0x60, config);

  controller_cmd(0xAE);  // enable keyboard port
  controller_cmd(0xA8);  // enable mouse port
  flush_output();

  if (keyboard_send(0xF4)) {  // enable scanning
    found |= PS2_HAS_KEYBOARD;
  }
  mouse_send(0xF6);           // defaults
  if (mouse_send(0xF4)) {     // enable data reporting
    found |= PS2_HAS_MOUSE;
  }
  flush_output();

  // Devices are up: now let the controller raise IRQ1 / IRQ12.
  controller_cmd(0x20);
  config = wait_read_ready() ? inb(PS2_DATA) : config;
  config |= 0x03;
  controller_cmd_arg(0x60, config);

  g_head = 0;
  g_tail = 0;
  irq_register(1, keyboard_irq);
  irq_register(12, mouse_irq);
  irq_unmask(1);
  irq_unmask(12);
  return found;
}

// ---- Keyboard decoding (scancode set 1) -----------------------------------

static const char kKeymap[0x3A] = {
  0, 27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b', '\t',
  'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n', 0, 'a', 's',
  'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0, '\\', 'z', 'x', 'c', 'v',
  'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' '
};

static const char kKeymapShift[0x3A] = {
  0, 27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b', '\t',
  'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n', 0, 'A', 'S',
  'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0, '|', 'Z', 'X', 'C', 'V',
  'B', 'N', 'M', '<', '>', '?', 0, '*', 0, ' '
};

static int g_shift = 0;
static int g_caps = 0;
static int g_extended = 0;
static int g_ctrl = 0;

static int decode_key(uint8_t code, ps2_event_t *ev) {
  int pressed = !(code & 0x80);
  uint8_t key = code & 0x7F;
  char ch = 0;

  if (code == 0xE0) {          // extended prefix: swallow the next byte
    g_extended = 1;
    return 0;
  }
  if (g_extended) {
    g_extended = 0;
    if (key == 0x1D) {                 // right ctrl
      g_ctrl = pressed;
      return 0;
    }
    switch (key) {
      case 0x48: ch = PS2_KEY_UP; break;
      case 0x50: ch = PS2_KEY_DOWN; break;
      case 0x4B: ch = PS2_KEY_LEFT; break;
      case 0x4D: ch = PS2_KEY_RIGHT; break;
      case 0x53: ch = PS2_KEY_DELETE; break;
      default: return 0;
    }
    ev->type = PS2_EVENT_KEY;
    ev->ch = ch;
    ev->pressed = pressed;
    return 1;
  }
  if (key == 0x1D) {                   // left ctrl
    g_ctrl = pressed;
    return 0;
  }
  if (key == 0x2A || key == 0x36) {  // left / right shift
    g_shift = pressed;
    return 0;
  }
  if (key == 0x3A) {                 // caps lock toggles on press
    if (pressed) {
      g_caps = !g_caps;
    }
    return 0;
  }
  if (key < sizeof(kKeymap)) {
    ch = g_shift ? kKeymapShift[key] : kKeymap[key];
    if (g_caps && ch >= 'a' && ch <= 'z') {
      ch = (char)(ch - 'a' + 'A');
    } else if (g_caps && ch >= 'A' && ch <= 'Z' && g_shift) {
      ch = (char)(ch - 'A' + 'a');
    }
  }
  if (g_ctrl && ch >= 'a' && ch <= 'z') {
    ch = (char)(ch - 'a' + 1);         // Ctrl+letter -> control code
  }
  ev->type = PS2_EVENT_KEY;
  ev->ch = ch;
  ev->pressed = pressed;
  return 1;
}

// ---- Mouse decoding (3-byte standard packets) ------------------------------

static uint8_t g_packet[3];
static int g_packet_len = 0;

static int decode_mouse(uint8_t byte, ps2_event_t *ev) {
  int dx, dy;

  if (g_packet_len == 0 && !(byte & 0x08)) {
    return 0;  // out of sync: first byte always has bit 3 set
  }
  g_packet[g_packet_len++] = byte;
  if (g_packet_len < 3) {
    return 0;
  }
  g_packet_len = 0;

  dx = (int)g_packet[1] - (((int)g_packet[0] << 4) & 0x100);
  dy = (int)g_packet[2] - (((int)g_packet[0] << 3) & 0x100);
  ev->type = PS2_EVENT_MOUSE;
  ev->dx = dx;
  ev->dy = -dy;  // PS/2 reports +y up; the screen has +y down
  ev->buttons = g_packet[0] & 0x07;
  return 1;
}

int ps2_poll(ps2_event_t *ev) {
  while (g_tail != g_head) {
    uint16_t entry = g_ring[g_tail];
    uint8_t byte = (uint8_t)(entry & 0xFF);
    int aux = (entry & RING_AUX) != 0;

    g_tail = (g_tail + 1) % RING_SIZE;
    ev->type = PS2_EVENT_NONE;
    ev->ch = 0;
    ev->pressed = 0;
    ev->dx = 0;
    ev->dy = 0;
    ev->buttons = 0;
    if (aux) {
      if (decode_mouse(byte, ev)) {
        return 1;
      }
    } else if (decode_key(byte, ev)) {
      return 1;
    }
  }
  return 0;
}
