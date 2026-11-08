#include "user.h"

// Exercises the file syscalls. Exit code 0 = all checks passed; 50 = everything
// passed except that FLUSH.TXT (left open by the previous run) was not found, which
// is expected on the very first run; anything else is the number of the failed check.

static int g_failed = 0;

static void check(int ok, int number, const char *what) {
  if (!ok) {
    puts("files: FAIL ");
    put_uint((unsigned long)number);
    puts(" ");
    puts(what);
    puts("\r\n");
    if (g_failed == 0) {
      g_failed = number;
    }
  }
}

static int same(const char *a, const char *b, unsigned long n) {
  unsigned long i;

  for (i = 0; i < n; ++i) {
    if (a[i] != b[i]) {
      return 0;
    }
  }
  return 1;
}

static char g_out[6000];
static char g_in[6100];

int main(void) {
  long fd;
  long n;
  long total;
  int i;
  int found_hello = 0;
  int entries = 0;
  int flush_seen = 0;
  dirent_t de;

  // The previous run left /FLUSH.TXT open on purpose: the kernel stores it at exit.
  fd = open("/FLUSH.TXT", O_RDONLY);
  if (fd >= 3) {
    n = read((int)fd, g_in, sizeof(g_in));
    flush_seen = (n == 3 && same(g_in, "bye", 3));
    close((int)fd);
  }

  // Read a program image from disk.
  fd = open("/APPS/HELLO.BIN", O_RDONLY);
  check(fd >= 3, 1, "open HELLO.BIN");
  total = 0;
  while ((n = read((int)fd, g_in, 512)) > 0) {
    total += n;
  }
  check(total > 100, 2, "read HELLO.BIN to the end");
  check(read((int)fd, g_in, 16) == 0, 3, "read at end of file returns 0");
  check(write((int)fd, "x", 1) < 0, 4, "write to a read-only file fails");
  check(readdir((int)fd, &de) == -20, 5, "readdir on a file is ENOTDIR");
  check(close((int)fd) == 0, 6, "close");
  check(close((int)fd) == -9, 7, "second close is EBADF");

  // List a directory.
  fd = open("/APPS", O_RDONLY);
  check(fd >= 3, 8, "open directory");
  while (readdir((int)fd, &de) == 1) {
    ++entries;
    if (de.name[0] == 'H' && de.name[1] == 'E' && de.name[2] == 'L' && de.name[3] == 'L' && de.name[4] == 'O') {
      found_hello = 1;
    }
  }
  check(entries >= 3, 9, "directory has entries");
  check(found_hello, 10, "HELLO.BIN is listed");
  check(read((int)fd, g_in, 8) == -21, 11, "read on a directory is EISDIR");
  close((int)fd);
  fd = open("/", O_RDONLY);
  check(fd >= 3 && readdir((int)fd, &de) == 1, 12, "root directory lists");
  close((int)fd);

  // Write a file larger than one page, close, read it back.
  for (i = 0; i < (int)sizeof(g_out); ++i) {
    g_out[i] = (char)('A' + (i * 7) % 26);
  }
  fd = open("/FTEST.TXT", O_WRONLY);
  check(fd >= 3, 13, "open for writing");
  check(write((int)fd, g_out, 10) == 10, 14, "first write");
  check(write((int)fd, g_out + 10, sizeof(g_out) - 10) == (long)sizeof(g_out) - 10, 15, "second write");
  check(read((int)fd, g_in, 4) == -9, 16, "read from a write-only file fails");
  check(close((int)fd) == 0, 17, "close stores the file");
  fd = open("/FTEST.TXT", O_RDONLY);
  check(fd >= 3, 18, "reopen");
  total = 0;
  while ((n = read((int)fd, g_in + total, 1000)) > 0) {
    total += n;
  }
  check(total == (long)sizeof(g_out), 19, "size read back");
  check(same(g_in, g_out, sizeof(g_out)), 20, "contents read back");
  close((int)fd);

  // Truncate: writing less replaces the old contents.
  fd = open("/FTEST.TXT", O_WRONLY);
  write((int)fd, "short", 5);
  check(close((int)fd) == 0, 21, "close after truncating write");
  fd = open("/FTEST.TXT", O_RDONLY);
  check(read((int)fd, g_in, sizeof(g_in)) == 5, 22, "file was truncated");
  close((int)fd);

  // Errors.
  check(open("/NOPE/NOPE.TXT", O_RDONLY) == -2, 23, "missing file is ENOENT");
  check(open("/NODIR/X.TXT", O_WRONLY) == -2, 24, "write into a missing directory is ENOENT");
  check(open("/", O_WRONLY) == -21, 25, "write-open of the root directory is EISDIR");
  check(open((const char *)1, O_RDONLY) == -14, 26, "bad path pointer is EFAULT");
  check(open("/FTEST.TXT", 7) == -22, 27, "bad flags are EINVAL");
  check(read(77, g_in, 4) == -9 && close(77) == -9, 28, "unknown fd is EBADF");
  fd = open("/FTEST.TXT", O_RDONLY);
  check(read((int)fd, (void *)1, 4) == -14, 29, "bad buffer pointer is EFAULT");
  close((int)fd);

  // Descriptor table: 8 open files, the 9th fails.
  {
    long fds[9];
    for (i = 0; i < 9; ++i) {
      fds[i] = open("/FTEST.TXT", O_RDONLY);
    }
    check(fds[7] >= 3 && fds[8] == -24, 30, "ninth open file is EMFILE");
    for (i = 0; i < 8; ++i) {
      close((int)fds[i]);
    }
    fd = open("/FTEST.TXT", O_RDONLY);
    check(fd >= 3, 31, "descriptors are reusable after close");
    close((int)fd);
  }

  // Leave a file open for writing: the kernel must store it when the program ends.
  fd = open("/FLUSH.TXT", O_WRONLY);
  write((int)fd, "bye", 3);

  if (g_failed != 0) {
    return g_failed;
  }
  puts("files: all checks passed\r\n");
  return flush_seen ? 0 : 50;
}
