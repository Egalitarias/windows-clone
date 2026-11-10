#ifndef KERNEL_SYSFILE_H
#define KERNEL_SYSFILE_H

#include "console.h"

// File syscalls for user programs, backed by FAT32 (phase 5 of SYSCALL_PLAN.md).
//
// Paths look like "/APPS/HELLO.BIN". There are no drive letters: the default
// volume (fs_default_volume) is searched first, then the others in order.
//
// Files are whole-file buffered, because the FAT driver reads and replaces whole
// files: open() for reading loads the file into kernel memory and read() serves it
// from there; open() for writing starts an empty buffer, write() appends to it and
// close() replaces the file on disk. Files are limited to SYSFILE_MAX_SIZE bytes.
// A write-opened file is only created or truncated by close(). Writes always go to
// the default volume. Opened with O_RDWR or O_APPEND the file keeps its contents; writes
// then land at the current position (lseek moves it) and close() stores the file only if
// something was written.
//
// Descriptors 0, 1 and 2 are the program's console; files start at 3.

#define SYSFILE_FIRST_FD 3
#define SYSFILE_MAX_FDS  8                      // open files per process
#define SYSFILE_MAX_SIZE (256U * 1024U)

#define SYSFILE_O_RDONLY 0                      // file or directory
#define SYSFILE_O_WRONLY 1                      // create or truncate, applied on close
#define SYSFILE_O_RDWR   2                      // read and write; keeps the contents, creates if missing
#define SYSFILE_O_APPEND 4                      // with WRONLY or RDWR: keep the contents, write at the end

#define SYSFILE_SEEK_SET 0
#define SYSFILE_SEEK_CUR 1
#define SYSFILE_SEEK_END 2

#define SYSFILE_NAME_MAX 128

// What readdir() stores in the caller's buffer. Keep in sync with src/user/user.h.
typedef struct {
  uint32_t size;
  uint32_t attr;                                // FAT_ATTR_* (0x10 = directory)
  char name[SYSFILE_NAME_MAX];
} sysfile_dirent_t;

// Each returns a non-negative result or a negative SYSCALL_E* code.
int64_t sysfile_open(uint64_t path_uaddr, uint64_t flags);
int64_t sysfile_close(uint64_t fd);
int64_t sysfile_read(uint64_t fd, uint64_t buf, uint64_t len);
int64_t sysfile_write(uint64_t fd, uint64_t buf, uint64_t len);
int64_t sysfile_readdir(uint64_t fd, uint64_t out_uaddr);
int64_t sysfile_lseek(uint64_t fd, int64_t offset, uint64_t whence);   // -> new position
int64_t sysfile_stat(uint64_t path_uaddr, uint64_t out_uaddr);         // fills a sysfile_dirent_t
int64_t sysfile_unlink(uint64_t path_uaddr);                           // file or empty directory
int64_t sysfile_mkdir(uint64_t path_uaddr);

// Close every file a process left open (flushing pending writes if `flush`).
void sysfile_close_all(void *table, int flush);

#endif // KERNEL_SYSFILE_H
