#ifndef KERNEL_PS2_H
#define KERNEL_PS2_H

#include "console.h"

// Polled PS/2 keyboard and mouse driver (i8042 controller). Interrupts from
// the controller are left disabled; call ps2_poll() from a loop.

#define PS2_HAS_KEYBOARD 0x1
#define PS2_HAS_MOUSE    0x2

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
  // Mouse events: movement since the last packet (screen orientation: +y is
  // down) and the current button state (PS2_MOUSE_* bits).
  int dx;
  int dy;
  unsigned int buttons;
} ps2_event_t;

// Initialise the controller and devices. Returns PS2_HAS_* bits for the
// devices that answered.
int ps2_init(void);

// Fetch at most one decoded event. Returns 1 and fills `ev` if one is ready,
// otherwise 0.
int ps2_poll(ps2_event_t *ev);

#endif // KERNEL_PS2_H
