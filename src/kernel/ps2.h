#ifndef KERNEL_PS2_H
#define KERNEL_PS2_H

#include "console.h"

// Interrupt-driven PS/2 keyboard and mouse driver (i8042 controller). IRQ1 and
// IRQ12 push raw bytes into a ring buffer; ps2_poll() decodes them in normal
// context. Call irq_init() first, and enable interrupts (sti) afterwards.

#define PS2_HAS_KEYBOARD 0x1
#define PS2_HAS_MOUSE    0x2

// Non-printing keys are delivered as these control codes in `ch`.
#define PS2_KEY_LEFT   0x1C
#define PS2_KEY_RIGHT  0x1D
#define PS2_KEY_UP     0x1E
#define PS2_KEY_DOWN   0x1F
#define PS2_KEY_DELETE 0x7F
// These four are above 0x7F, so they never look like text (a char is signed: compare
// with these macros, or cast to unsigned char).
#define PS2_KEY_HOME   ((char)0x80)
#define PS2_KEY_END    ((char)0x81)
#define PS2_KEY_PGUP   ((char)0x82)
#define PS2_KEY_PGDN   ((char)0x83)

// Modifier bits in ps2_event_t.mods (key events: the state when the key was pressed).
#define PS2_MOD_SHIFT 0x1
#define PS2_MOD_CTRL  0x2
#define PS2_MOD_ALT   0x4
// Ctrl+letter arrives as the usual control code (Ctrl+A = 0x01 ... Ctrl+Z = 0x1A).

#define PS2_MOUSE_LEFT   0x1
#define PS2_MOUSE_RIGHT  0x2
#define PS2_MOUSE_MIDDLE 0x4

typedef enum {
  PS2_EVENT_NONE = 0,
  PS2_EVENT_KEY,
  PS2_EVENT_MOUSE
} ps2_event_type_t;

typedef struct {
  ps2_event_type_t type;
  // Key events: `ch` is the ASCII character (0 for non-printing keys),
  // '\b' for backspace, '\n' for enter, 27 for escape. `pressed` is 1 on
  // make and 0 on break.
  char ch;
  int pressed;
  unsigned int mods;           // PS2_MOD_* held down (key and mouse events)
  // Mouse events: movement since the last packet (screen orientation: +y is
  // down) and the current button state (PS2_MOUSE_* bits).
  int dx;
  int dy;
  unsigned int buttons;
} ps2_event_t;

// Initialise the controller and devices and hook IRQ1 / IRQ12. Returns
// PS2_HAS_* bits for the devices that answered.
int ps2_init(void);

// Fetch at most one decoded event. Returns 1 and fills `ev` if one is ready,
// otherwise 0.
int ps2_poll(ps2_event_t *ev);

#endif // KERNEL_PS2_H
