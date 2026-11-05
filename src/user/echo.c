#include "user.h"

// Reads lines from the keyboard and shouts them back. "quit" or Ctrl+D ends it.
int main(void) {
  char line[128];
  int lines = 0;

  puts("echo: type a line and press Enter (quit or Ctrl+D to stop)\r\n");
  for (;;) {
    long n;
    long i;

    puts("> ");
    n = read(0, line, sizeof(line) - 1);
    if (n <= 0) {
      break;
    }
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) {
      --n;
    }
    line[n] = 0;
    if (n == 4 && line[0] == 'q' && line[1] == 'u' && line[2] == 'i' && line[3] == 't') {
      break;
    }
    for (i = 0; i < n; ++i) {
      if (line[i] >= 'a' && line[i] <= 'z') {
        line[i] = (char)(line[i] - 'a' + 'A');
      }
    }
    puts(line);
    puts("\r\n");
    ++lines;
  }
  puts("echo: bye, ");
  put_uint((unsigned long)lines);
  puts(" line(s)\r\n");
  return lines;
}
