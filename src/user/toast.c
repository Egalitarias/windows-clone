#include "user.h"

// Raises a notification: `run toast MESSAGE` (a default message without an argument).
int main(void) {
  char text[64];

  if (getarg(text, sizeof(text)) <= 0) {
    return (int)notify("Hello from a program");
  }
  return (int)notify(text);
}
