// Program entry. The flat binary is linked so that .text._start comes first, so
// the entry point is the first byte of the image (see user.ld).
#include "user.h"

extern int main(void);

__attribute__((section(".text._start"), used)) void _start(void) {
  exit(main());
}
