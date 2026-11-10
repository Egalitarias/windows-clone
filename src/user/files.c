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

  // lseek, O_RDWR, O_APPEND.
  fd = open("/SEEK.TXT", O_WRONLY);
  write((int)fd, "0123456789", 10);
  check(lseek((int)fd, 0, SEEK_CUR) == 10, 32, "lseek CUR reports the position");
  check(lseek((int)fd, 2, SEEK_SET) == 2, 33, "lseek SET");
  write((int)fd, "AB", 2);                         // overwrite in place
  check(close((int)fd) == 0, 34, "close seek file");
  fd = open("/SEEK.TXT", O_RDWR);
  check(fd >= 3, 35, "open O_RDWR");
  check(read((int)fd, g_in, 20) == 10 && same(g_in, "01AB456789", 10), 36, "in-place write kept the rest");
  check(lseek((int)fd, -3, SEEK_END) == 7, 37, "lseek END");
  check(read((int)fd, g_in, 20) == 3 && same(g_in, "789", 3), 38, "read after seek");
  check(lseek((int)fd, -1, SEEK_SET) == -22, 39, "negative position is EINVAL");
  check(lseek((int)fd, 0, 9) == -22, 40, "bad whence is EINVAL");
  lseek((int)fd, 0, SEEK_SET);
  write((int)fd, "xy", 2);
  check(close((int)fd) == 0, 41, "close O_RDWR file");
  fd = open("/SEEK.TXT", O_WRONLY | O_APPEND);
  check(fd >= 3, 42, "open O_APPEND");
  lseek((int)fd, 0, SEEK_SET);                    // append ignores the position
  write((int)fd, "!!", 2);
  check(close((int)fd) == 0, 43, "close appended file");
  fd = open("/SEEK.TXT", O_RDONLY);
  n = read((int)fd, g_in, 40);
  check(n == 12 && same(g_in, "xyAB456789!!", 12), 44, "append result");
  check(lseek((int)fd, 0, SEEK_SET) == 0 && read((int)fd, g_in, 2) == 2, 45, "rewind a read file");
  close((int)fd);
  fd = open("/NEWRW.TXT", O_RDWR);                // creates when missing
  check(fd >= 3 && close((int)fd) == 0, 46, "O_RDWR creates a missing file");
  check(stat("/NEWRW.TXT", &de) == 0 && de.size == 0, 47, "created file is empty");
  check(open("/SEEK.TXT", O_RDWR | O_WRONLY) == -22, 48, "RDWR|WRONLY is EINVAL");

  // stat, mkdir, unlink.
  check(stat("/SEEK.TXT", &de) == 0 && de.size == 12 && (de.attr & 0x10) == 0, 49, "stat a file");
  check(stat("/APPS", &de) == 0 && (de.attr & 0x10) != 0, 50, "stat a directory");
  check(stat("/", &de) == 0 && (de.attr & 0x10) != 0, 51, "stat the root");
  check(stat("/NOPE.TXT", &de) == -2, 52, "stat a missing file is ENOENT");
  check(stat("/SEEK.TXT", (dirent_t *)1) == -14, 53, "stat with a bad buffer is EFAULT");
  check(mkdir("/NEWDIR") == 0, 54, "mkdir");
  check(mkdir("/NEWDIR") == -17, 55, "mkdir twice is EEXIST");
  check(mkdir("/NODIR/SUB") == -2, 56, "mkdir in a missing directory is ENOENT");
  fd = open("/NEWDIR/IN.TXT", O_WRONLY);
  write((int)fd, "in", 2);
  check(close((int)fd) == 0, 57, "file in the new directory");
  check(unlink("/NEWDIR") == -39, 58, "unlink of a non-empty directory is ENOTEMPTY");
  check(unlink("/NEWDIR/IN.TXT") == 0, 59, "unlink a file");
  check(stat("/NEWDIR/IN.TXT", &de) == -2, 60, "unlinked file is gone");
  check(unlink("/NEWDIR") == 0, 61, "unlink an empty directory");
  check(unlink("/NEWDIR") == -2, 62, "unlink twice is ENOENT");
  check(unlink("/") == -22, 63, "unlink of the root is EINVAL");
  check(unlink("/SEEK.TXT") == 0 && unlink("/NEWRW.TXT") == 0, 64, "clean up");

  // Leave a file open for writing: the kernel must store it when the program ends.
  fd = open("/FLUSH.TXT", O_WRONLY);
  write((int)fd, "bye", 3);

  if (g_failed != 0) {
    return g_failed;
  }
  puts("files: all checks passed\r\n");
  return flush_seen ? 0 : 50;
}
