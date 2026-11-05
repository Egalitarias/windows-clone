#include "user.h"

// Zero-initialised data: proves the loader clears the pages after the image.
static volatile char g_bss_buffer[1024];
static volatile int g_data_value = 1234;

int main(void) {
  unsigned int i;
  int bss_clean = 1;

  {
    // write() must return the byte count and leave the other registers intact.
    const char *msg = "Hello from a program loaded from disk!\r\n";
    if (write(1, msg, strlen(msg)) != (long)strlen(msg)) {
      return 2;
    }
  }
  puts("  pid: ");
  put_uint((unsigned long)getpid());
  puts("\r\n");

  for (i = 0; i < sizeof(g_bss_buffer); ++i) {
    if (g_bss_buffer[i] != 0) {
      bss_clean = 0;
    }
  }
  puts(bss_clean ? "  bss is zeroed\r\n" : "  bss is NOT zeroed\r\n");
  puts(g_data_value == 1234 ? "  data section intact\r\n" : "  data section broken\r\n");

  yield();
  return (bss_clean && g_data_value == 1234) ? 7 : 1;
}
