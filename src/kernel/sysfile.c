#include "sysfile.h"
#include "fs.h"
#include "heap.h"
#include "paging.h"
#include "process.h"
#include "scheduler.h"
#include "string.h"
#include "syscall.h"
#include "syswin.h"

#define KIND_FREE  0
#define KIND_READ  1
#define KIND_WRITE 2
#define KIND_DIR   3

#define PATH_MAX 256

typedef struct {
  int kind;
  fat_volume_t *v;
  unsigned int gen;            // fs_generation() at open; a rescan invalidates v
  uint8_t *data;               // READ: the whole file; WRITE: bytes so far
  uint32_t len;
  uint32_t cap;
  uint32_t pos;                // read/write position
  uint8_t readable;            // WRITE: opened with O_RDWR
  uint8_t append;              // WRITE: every write goes to the end
  uint8_t dirty;               // WRITE: needs storing on close
  uint8_t loaded;              // WRITE: the buffer started as the file's old contents
  fat_file_t *df;              // streamed file (bigger than SYSFILE_BUF_MAX): no buffer, len = size
  uint32_t dir_cluster;        // WRITE: directory and name to replace on close
  char name[FAT_NAME_MAX];
  fat_iter_t *iter;            // DIR
} sysfile_t;

typedef struct {
  sysfile_t f[SYSFILE_MAX_FDS];
} sysfile_table_t;

// Syscalls run with interrupts off. Disk access needs the timer and the drivers'
// own interrupts, so open them for the duration of a file operation, exactly as
// sleep and read do while they wait. The FAT driver serialises itself.
static void io_begin(void) { __asm__ volatile("sti" : : : "memory"); }
static void io_end(void) { __asm__ volatile("cli" : : : "memory"); }

static sysfile_table_t *table(int create) {
  void **slot = process_files_slot();
  sysfile_table_t *t;

  if (slot == 0) {
    return 0;
  }
  if (*slot == 0 && create) {
    t = (sysfile_table_t *)kmalloc(sizeof(*t));
    if (t != 0) {
      memset(t, 0, sizeof(*t));
      *slot = t;
    }
  }
  return (sysfile_table_t *)*slot;
}

static sysfile_t *get_fd(uint64_t fd) {
  sysfile_table_t *t = table(0);

  if (t == 0 || fd < SYSFILE_FIRST_FD || fd >= SYSFILE_FIRST_FD + SYSFILE_MAX_FDS) {
    return 0;
  }
  if (t->f[fd - SYSFILE_FIRST_FD].kind == KIND_FREE) {
    return 0;
  }
  return &t->f[fd - SYSFILE_FIRST_FD];
}

// Copy a NUL-terminated string out of user memory, checking each page first.
int sys_copy_user_string(uint64_t uaddr, char *out, uint64_t max) {
  uint64_t i;

  for (i = 0; i < max; ++i) {
    uint64_t a = uaddr + i;

    if ((i == 0 || (a & 0xFFFULL) == 0) && !paging_user_range_ok(a, 1)) {
      return SYSCALL_EFAULT;
    }
    out[i] = *(const char *)(uintptr_t)a;
    if (out[i] == 0) {
      return 0;
    }
  }
  return SYSCALL_ENAMETOOLONG;
}

// ---- path resolution ---------------------------------------------------------

// Read one component into part and advance. Returns 0 for none, -1 if too long.
static int next_part(const char **pp, char *part, size_t max) {
  const char *p = *pp;
  size_t n = 0;

  while (*p == '/') {
    ++p;
  }
  if (*p == 0) {
    *pp = p;
    return 0;
  }
  while (*p != 0 && *p != '/') {
    if (n + 1 >= max) {
      return -1;
    }
    part[n++] = *p++;
  }
  part[n] = 0;
  while (*p == '/') {
    ++p;
  }
  *pp = p;
  return 1;
}

// Find `path` on one volume. A path with no components is the root directory
// (*is_root = 1, ent untouched). Returns 1 when found.
static int lookup(fat_volume_t *v, const char *path, fat_dirent_t *ent, int *is_root) {
  uint32_t dir = v->root_cluster;
  char part[FAT_NAME_MAX];
  int r;

  *is_root = 0;
  r = next_part(&path, part, sizeof(part));
  if (r == 0) {
    *is_root = 1;
    return 1;
  }
  for (; r > 0; r = next_part(&path, part, sizeof(part))) {
    if (!fat_lookup(v, dir, part, ent)) {
      return 0;
    }
    if (*path == 0) {
      return 1;
    }
    if ((ent->attr & FAT_ATTR_DIR) == 0) {
      return 0;
    }
    dir = ent->first_cluster;
  }
  return 0;
}

// Split `path` into the directory holding its last component and that name.
static int lookup_parent(fat_volume_t *v, const char *path, uint32_t *dir_out, char *name_out) {
  uint32_t dir = v->root_cluster;
  char part[FAT_NAME_MAX];
  fat_dirent_t ent;
  int r;

  r = next_part(&path, part, sizeof(part));
  if (r <= 0) {
    return 0;
  }
  while (*path != 0) {
    if (!fat_lookup(v, dir, part, &ent) || (ent.attr & FAT_ATTR_DIR) == 0) {
      return 0;
    }
    dir = ent.first_cluster;
    if (next_part(&path, part, sizeof(part)) <= 0) {
      return 0;
    }
  }
  str_copy(name_out, part, FAT_NAME_MAX);
  *dir_out = dir;
  return 1;
}

// Volume search order: the default volume, then the rest.
static fat_volume_t *volume_at(int n) {
  fat_volume_t *def = fs_default_volume();
  int i;

  if (n == 0) {
    return def;
  }
  for (i = 0; i < fs_volume_count(); ++i) {
    fat_volume_t *v = fs_volume(i);
    if (v != 0 && v != def && --n == 0) {
      return v;
    }
  }
  return 0;
}

// "/@N/rest" names volume N (the index fs_volume() uses, see volinfo) exactly, for
// every call that takes a path. Without the prefix, reads search the default volume
// first and writes go to it. Returns the part after the prefix; *pinned is the volume
// (0 when there is no prefix) and *bad is set when N does not exist.
static const char *split_volume(const char *path, fat_volume_t **pinned, int *bad) {
  int n = 0;
  int i = 2;

  *pinned = 0;
  *bad = 0;
  if (path[0] != '/' || path[1] != '@' || path[2] < '0' || path[2] > '9') {
    return path;
  }
  while (path[i] >= '0' && path[i] <= '9' && n < 1000) {
    n = n * 10 + (path[i] - '0');
    ++i;
  }
  if (path[i] != 0 && path[i] != '/') {
    return path;                                // "/@12x": an ordinary (unlikely) name
  }
  *pinned = n < fs_volume_count() ? fs_volume(n) : 0;
  *bad = *pinned == 0;
  return path + i;
}

// ---- open / close ------------------------------------------------------------

static void release(sysfile_t *f) {
  if (f->data != 0) {
    kfree(f->data);
  }
  if (f->iter != 0) {
    kfree(f->iter);
  }
  if (f->df != 0) {
    kfree(f->df);
  }
  memset(f, 0, sizeof(*f));
}

static int flush(sysfile_t *f) {
  int ok;

  if (f->gen != fs_generation()) {
    return 0;                                   // the volume went away
  }
  io_begin();
  ok = fat_write_file(f->v, f->dir_cluster, f->name, f->data, f->len);
  io_end();
  return ok;
}

// Load a whole file into f->data / f->len. Caller holds the preempt lock.
static int64_t load_file(sysfile_t *f, fat_volume_t *v, const fat_dirent_t *ent) {
  if (ent->size > SYSFILE_BUF_MAX) {
    return SYSCALL_EFBIG;
  }
  if (ent->size > 0) {
    uint32_t got = 0;
    int ok;

    f->data = (uint8_t *)kmalloc(ent->size);
    if (f->data == 0) {
      return SYSCALL_ENOMEM;
    }
    f->cap = ent->size;
    io_begin();
    ok = fat_read_file(v, ent, f->data, ent->size, &got);
    io_end();
    if (!ok || got != ent->size) {
      return SYSCALL_EIO;
    }
  }
  f->len = ent->size;
  return 0;
}

// Take an existing file's contents: into memory when small, else as a streamed file.
static int64_t load_or_stream(sysfile_t *f, fat_volume_t *v, const fat_dirent_t *ent) {
  if (ent->size > SYSFILE_MAX_SIZE) {
    return SYSCALL_EFBIG;
  }
  if (ent->size > SYSFILE_BUF_MAX) {
    f->df = (fat_file_t *)kmalloc(sizeof(*f->df));
    if (f->df == 0) {
      return SYSCALL_ENOMEM;
    }
    if (!fat_file_open(v, ent, f->df)) {
      return SYSCALL_EIO;
    }
    f->len = ent->size;
    return 0;
  }
  f->loaded = 1;
  return load_file(f, v, ent);
}

static int64_t open_read(sysfile_t *f, const char *path, fat_volume_t *pinned, int bad) {
  int n;

  if (bad) {
    return SYSCALL_ENOENT;
  }
  for (n = 0; n < fs_volume_count(); ++n) {
    fat_volume_t *v = pinned != 0 ? pinned : volume_at(n);
    fat_dirent_t ent;
    int is_root = 0;
    int found;

    if (v == 0) {
      break;
    }
    io_begin();
    found = lookup(v, path, &ent, &is_root);
    io_end();
    if (!found) {
      if (pinned != 0) {
        break;
      }
      continue;
    }
    f->v = v;
    f->gen = fs_generation();
    if (is_root || (ent.attr & FAT_ATTR_DIR) != 0) {
      f->iter = (fat_iter_t *)kmalloc(sizeof(*f->iter));
      if (f->iter == 0) {
        return SYSCALL_ENOMEM;
      }
      io_begin();
      fat_iter_open(v, is_root ? v->root_cluster : ent.first_cluster, f->iter);
      io_end();
      f->kind = KIND_DIR;
      return 0;
    }
    {
      int64_t lr = load_or_stream(f, v, &ent);

      if (lr < 0) {
        return lr;
      }
    }
    f->kind = KIND_READ;
    return 0;
  }
  return SYSCALL_ENOENT;
}

static int64_t open_write(sysfile_t *f, const char *path, uint64_t flags, fat_volume_t *pinned, int bad) {
  fat_volume_t *v = pinned != 0 ? pinned : fs_default_volume();
  fat_dirent_t ent;
  int found;
  int ok;

  if (v == 0 || bad) {
    return SYSCALL_ENOENT;
  }
  io_begin();
  if (!lookup_parent(v, path, &f->dir_cluster, f->name)) {
    int is_root = 0;

    ok = 0;
    if (lookup(v, path, &ent, &is_root) && is_root) {
      io_end();
      return SYSCALL_EISDIR;                    // "/" has no name to write
    }
  } else {
    ok = 1;
  }
  found = ok && fat_lookup(v, f->dir_cluster, f->name, &ent);
  io_end();
  if (!ok) {
    return SYSCALL_ENOENT;
  }
  if (found && (ent.attr & FAT_ATTR_DIR) != 0) {
    return SYSCALL_EISDIR;
  }
  if (found && (ent.attr & FAT_ATTR_READONLY) != 0) {
    return SYSCALL_EACCES;
  }
  f->v = v;
  f->gen = fs_generation();
  f->readable = (flags & SYSFILE_O_RDWR) != 0;
  f->append = (flags & SYSFILE_O_APPEND) != 0;
  if ((flags & (SYSFILE_O_RDWR | SYSFILE_O_APPEND)) != 0 && found) {
    int64_t lr = load_or_stream(f, v, &ent);    // keep the old contents

    if (lr < 0) {
      return lr;
    }
    f->dirty = 0;
  } else {
    f->dirty = 1;                               // truncated or newly created
  }
  f->pos = f->append ? f->len : 0;
  f->kind = KIND_WRITE;
  return 0;
}

int64_t sysfile_open(uint64_t path_uaddr, uint64_t flags) {
  char path[PATH_MAX];
  sysfile_table_t *t;
  sysfile_t *f = 0;
  fat_volume_t *pinned;
  const char *rest;
  int bad;
  int64_t r;
  int i;

  // Valid: 0, WRONLY, RDWR, and either of the last two with APPEND.
  if (flags != SYSFILE_O_RDONLY && flags != SYSFILE_O_WRONLY && flags != SYSFILE_O_RDWR &&
      flags != (SYSFILE_O_WRONLY | SYSFILE_O_APPEND) && flags != (SYSFILE_O_RDWR | SYSFILE_O_APPEND)) {
    return SYSCALL_EINVAL;
  }
  r = sys_copy_user_string(path_uaddr, path, sizeof(path));
  if (r < 0) {
    return r;
  }
  rest = split_volume(path, &pinned, &bad);
  t = table(1);
  if (t == 0) {
    return SYSCALL_ENOMEM;
  }
  for (i = 0; i < SYSFILE_MAX_FDS; ++i) {
    if (t->f[i].kind == KIND_FREE) {
      f = &t->f[i];
      break;
    }
  }
  if (f == 0) {
    return SYSCALL_EMFILE;
  }
  // Each fat_* call is atomic on its own, but opening is a lookup followed by a read
  // (or a parent lookup followed by a name lookup). Keep other tasks from switching in
  // between so the directory cannot change under us.
  scheduler_preempt_disable();
  r = (flags != SYSFILE_O_RDONLY) ? open_write(f, rest, flags, pinned, bad) : open_read(f, rest, pinned, bad);
  scheduler_preempt_enable();
  if (r < 0) {
    release(f);
    return r;
  }
  return (int64_t)(SYSFILE_FIRST_FD + i);
}

int64_t sysfile_close(uint64_t fd) {
  sysfile_t *f = get_fd(fd);
  int ok = 1;

  if (f == 0) {
    return SYSCALL_EBADF;
  }
  if (f->kind == KIND_WRITE && f->dirty) {
    ok = flush(f);
  }
  release(f);
  return ok ? 0 : SYSCALL_EIO;
}

void sysfile_close_all(void *tbl, int do_flush) {
  sysfile_table_t *t = (sysfile_table_t *)tbl;
  int i;

  if (t == 0) {
    return;
  }
  for (i = 0; i < SYSFILE_MAX_FDS; ++i) {
    if (t->f[i].kind == KIND_WRITE && t->f[i].dirty && do_flush) {
      flush(&t->f[i]);
    }
    release(&t->f[i]);
  }
  kfree(t);
}

// ---- read / write / readdir --------------------------------------------------

// ---- streamed files ------------------------------------------------------------------

static int64_t stream_read(sysfile_t *f, uint64_t buf, uint64_t len) {
  uint32_t got = 0;
  uint8_t *tmp;
  uint64_t n = len < SYSFILE_CHUNK ? len : SYSFILE_CHUNK;
  int ok;

  if (f->pos >= f->len) {
    return 0;                                   // end of file
  }
  if (n > f->len - f->pos) {
    n = f->len - f->pos;
  }
  if (!paging_user_range_ok(buf, n)) {
    return SYSCALL_EFAULT;
  }
  if (f->gen != fs_generation()) {
    return SYSCALL_EIO;                         // the volume went away
  }
  tmp = (uint8_t *)kmalloc((size_t)n);
  if (tmp == 0) {
    return SYSCALL_ENOMEM;
  }
  io_begin();
  ok = fat_file_read(f->df, f->pos, tmp, (uint32_t)n, &got);
  io_end();
  if (got > 0) {
    memcpy((void *)(uintptr_t)buf, tmp, got);   // the kernel copy: the disk driver cannot DMA into user memory
    f->pos += got;
  }
  kfree(tmp);
  if (got == 0) {
    return ok ? 0 : SYSCALL_EIO;
  }
  return (int64_t)got;
}

static int64_t stream_write(sysfile_t *f, uint64_t buf, uint64_t len) {
  uint32_t w = 0;
  uint8_t *tmp;
  uint64_t n;
  int ok;

  if (f->pos > f->len) {
    return SYSCALL_EINVAL;
  }
  if (f->pos >= SYSFILE_MAX_SIZE) {
    return SYSCALL_EFBIG;
  }
  n = len < SYSFILE_CHUNK ? len : SYSFILE_CHUNK;
  if (n > SYSFILE_MAX_SIZE - f->pos) {
    n = SYSFILE_MAX_SIZE - f->pos;
  }
  if (!paging_user_range_ok(buf, n)) {
    return SYSCALL_EFAULT;
  }
  if (f->gen != fs_generation()) {
    return SYSCALL_EIO;
  }
  tmp = (uint8_t *)kmalloc((size_t)n);
  if (tmp == 0) {
    return SYSCALL_ENOMEM;
  }
  memcpy(tmp, (const void *)(uintptr_t)buf, (size_t)n);
  io_begin();
  ok = fat_file_write(f->df, f->pos, tmp, (uint32_t)n, &w);
  io_end();
  kfree(tmp);
  f->len = f->df->ent.size;
  if (w == 0) {
    return ok ? SYSCALL_ENOSPC : SYSCALL_EIO;
  }
  f->pos += w;
  return (int64_t)w;
}

// A buffered write handle outgrew SYSFILE_BUF_MAX: put what it holds on the disk and carry
// on streaming. A file that was loaded is overwritten in place; a new or emptied one is
// created (or emptied) now. A hole left by a seek past the end is filled with zeros.
static int64_t spill(sysfile_t *f) {
  static const uint8_t zeros[4096];
  fat_file_t *df;
  fat_dirent_t ent;
  uint32_t w = 0;
  uint32_t at;
  int ok;

  if (f->gen != fs_generation()) {
    return SYSCALL_EIO;
  }
  df = (fat_file_t *)kmalloc(sizeof(*df));
  if (df == 0) {
    return SYSCALL_ENOMEM;
  }
  scheduler_preempt_disable();
  io_begin();
  if (f->loaded && fat_lookup(f->v, f->dir_cluster, f->name, &ent)) {
    ok = fat_file_open(f->v, &ent, df);
  } else {
    ok = fat_file_create(f->v, f->dir_cluster, f->name, df);
  }
  if (ok && f->len > 0) {
    ok = fat_file_write(df, 0, f->data, f->len, &w) && w == f->len;
  }
  for (at = f->len; ok && at < f->pos;) {
    uint32_t n = f->pos - at < sizeof(zeros) ? f->pos - at : (uint32_t)sizeof(zeros);

    ok = fat_file_write(df, at, zeros, n, &w) && w == n;
    at += n;
  }
  io_end();
  scheduler_preempt_enable();
  if (!ok) {
    kfree(df);
    return SYSCALL_EIO;
  }
  if (f->data != 0) {
    kfree(f->data);
  }
  f->data = 0;
  f->cap = 0;
  f->df = df;
  f->len = df->ent.size;
  f->dirty = 0;
  f->loaded = 0;
  return 0;
}

// ---- read / write / lseek -----------------------------------------------------------------

int64_t sysfile_read(uint64_t fd, uint64_t buf, uint64_t len) {
  sysfile_t *f = get_fd(fd);
  uint64_t n;

  if (f == 0) {
    return SYSCALL_EBADF;
  }
  if (f->kind == KIND_DIR) {
    return SYSCALL_EISDIR;
  }
  if (f->kind != KIND_READ && !(f->kind == KIND_WRITE && f->readable)) {
    return SYSCALL_EBADF;                       // opened write-only
  }
  if (f->df != 0) {
    return stream_read(f, buf, len);
  }
  n = f->pos < f->len ? f->len - f->pos : 0;
  if (n > len) {
    n = len;
  }
  if (n == 0) {
    return 0;                                   // end of file
  }
  if (!paging_user_range_ok(buf, n)) {
    return SYSCALL_EFAULT;
  }
  memcpy((void *)(uintptr_t)buf, f->data + f->pos, (size_t)n);
  f->pos += (uint32_t)n;
  return (int64_t)n;
}

int64_t sysfile_write(uint64_t fd, uint64_t buf, uint64_t len) {
  sysfile_t *f = get_fd(fd);
  uint64_t end;

  if (f == 0 || f->kind != KIND_WRITE) {
    return SYSCALL_EBADF;
  }
  if (len == 0) {
    return 0;
  }
  if (f->append) {
    f->pos = f->len;
  }
  if (f->df != 0) {
    return stream_write(f, buf, len);
  }
  if ((uint64_t)f->pos + len > SYSFILE_BUF_MAX) {
    int64_t r = spill(f);                       // too big to hold: continue on the disk

    if (r < 0) {
      return r;
    }
    return stream_write(f, buf, len);
  }
  if (!paging_user_range_ok(buf, len)) {
    return SYSCALL_EFAULT;
  }
  end = (uint64_t)f->pos + len;
  if (end > f->cap) {
    uint32_t cap = f->cap != 0 ? f->cap : 4096U;
    uint8_t *bigger;

    while (cap < end) {
      cap *= 2U;
    }
    bigger = (uint8_t *)kmalloc(cap);
    if (bigger == 0) {
      return SYSCALL_ENOMEM;
    }
    if (f->data != 0) {
      memcpy(bigger, f->data, f->len);
      kfree(f->data);
    }
    f->data = bigger;
    f->cap = cap;
  }
  if (f->pos > f->len) {
    memset(f->data + f->len, 0, f->pos - f->len);   // a seek past the end leaves a hole
  }
  memcpy(f->data + f->pos, (const void *)(uintptr_t)buf, (size_t)len);
  f->pos += (uint32_t)len;
  if (f->pos > f->len) {
    f->len = f->pos;
  }
  f->dirty = 1;
  return (int64_t)len;
}

int64_t sysfile_lseek(uint64_t fd, int64_t offset, uint64_t whence) {
  sysfile_t *f = get_fd(fd);
  int64_t base;
  int64_t target;
  int64_t bound;

  if (f == 0) {
    return SYSCALL_EBADF;
  }
  if (f->kind == KIND_DIR) {
    return SYSCALL_EISDIR;
  }
  if (whence == SYSFILE_SEEK_SET) {
    base = 0;
  } else if (whence == SYSFILE_SEEK_CUR) {
    base = (int64_t)f->pos;
  } else if (whence == SYSFILE_SEEK_END) {
    base = (int64_t)f->len;
  } else {
    return SYSCALL_EINVAL;
  }
  if (offset > (int64_t)SYSFILE_MAX_SIZE || offset < -(int64_t)SYSFILE_MAX_SIZE) {
    return SYSCALL_EINVAL;
  }
  bound = f->df != 0 ? (int64_t)f->len : (int64_t)SYSFILE_BUF_MAX;   // a streamed file has no holes
  target = base + offset;
  if (target < 0 || target > bound) {
    return SYSCALL_EINVAL;
  }
  f->pos = (uint32_t)target;
  return target;
}

int64_t sysfile_readdir(uint64_t fd, uint64_t out_uaddr) {
  sysfile_t *f = get_fd(fd);
  sysfile_dirent_t out;
  fat_dirent_t ent;
  int more;

  if (f == 0) {
    return SYSCALL_EBADF;
  }
  if (f->kind != KIND_DIR) {
    return SYSCALL_ENOTDIR;
  }
  if (!paging_user_range_ok(out_uaddr, sizeof(out))) {
    return SYSCALL_EFAULT;
  }
  if (f->gen != fs_generation()) {
    return SYSCALL_EIO;
  }
  io_begin();
  more = fat_iter_next(f->iter, &ent);
  io_end();
  if (!more) {
    return 0;
  }
  memset(&out, 0, sizeof(out));
  out.size = ent.size;
  out.attr = ent.attr;
  str_copy(out.name, ent.name, sizeof(out.name));
  memcpy((void *)(uintptr_t)out_uaddr, &out, sizeof(out));
  return 1;
}

// ---- stat / unlink / mkdir -----------------------------------------------------

static void fill_stat(sysfile_dirent_t *out, const fat_dirent_t *ent, int is_root) {
  memset(out, 0, sizeof(*out));
  if (is_root) {
    out->attr = FAT_ATTR_DIR;
    out->name[0] = '/';
    return;
  }
  out->size = ent->size;
  out->attr = ent->attr;
  str_copy(out->name, ent->name, sizeof(out->name));
}

int64_t sysfile_stat(uint64_t path_uaddr, uint64_t out_uaddr) {
  char path[PATH_MAX];
  sysfile_dirent_t out;
  fat_volume_t *pinned;
  const char *rest;
  int bad;
  int64_t r;
  int n;

  if (!paging_user_range_ok(out_uaddr, sizeof(out))) {
    return SYSCALL_EFAULT;
  }
  r = sys_copy_user_string(path_uaddr, path, sizeof(path));
  if (r < 0) {
    return r;
  }
  rest = split_volume(path, &pinned, &bad);
  r = SYSCALL_ENOENT;
  scheduler_preempt_disable();
  for (n = 0; n < fs_volume_count() && !bad; ++n) {
    fat_volume_t *v = pinned != 0 ? pinned : volume_at(n);
    fat_dirent_t ent;
    int is_root = 0;
    int found;

    if (v == 0) {
      break;
    }
    io_begin();
    found = lookup(v, rest, &ent, &is_root);
    io_end();
    if (found) {
      fill_stat(&out, &ent, is_root);
      r = 0;
      break;
    }
    if (pinned != 0) {
      break;
    }
  }
  scheduler_preempt_enable();
  if (r == 0) {
    memcpy((void *)(uintptr_t)out_uaddr, &out, sizeof(out));
  }
  return r;
}

int64_t sysfile_unlink(uint64_t path_uaddr) {
  char path[PATH_MAX];
  fat_volume_t *v;
  fat_dirent_t ent;
  const char *rest;
  int is_root = 0;
  int bad;
  int64_t r;

  r = sys_copy_user_string(path_uaddr, path, sizeof(path));
  if (r < 0) {
    return r;
  }
  rest = split_volume(path, &v, &bad);
  if (v == 0 && !bad) {
    v = fs_default_volume();
  }
  if (v == 0 || bad) {
    return SYSCALL_ENOENT;
  }
  scheduler_preempt_disable();
  io_begin();
  if (!lookup(v, rest, &ent, &is_root)) {
    r = SYSCALL_ENOENT;
  } else if (is_root) {
    r = SYSCALL_EINVAL;
  } else if ((ent.attr & FAT_ATTR_READONLY) != 0) {
    r = SYSCALL_EACCES;
  } else {
    int empty = 1;

    if ((ent.attr & FAT_ATTR_DIR) != 0) {
      fat_iter_t *it = (fat_iter_t *)kmalloc(sizeof(*it));
      fat_dirent_t child;

      if (it == 0) {
        empty = -1;
      } else {
        fat_iter_open(v, ent.first_cluster, it);
        empty = fat_iter_next(it, &child) ? 0 : 1;
        kfree(it);
      }
    }
    if (empty < 0) {
      r = SYSCALL_ENOMEM;
    } else if (!empty) {
      r = SYSCALL_ENOTEMPTY;
    } else {
      r = fat_delete(v, &ent) ? 0 : SYSCALL_EIO;
    }
  }
  io_end();
  scheduler_preempt_enable();
  return r;
}

int64_t sysfile_mkdir(uint64_t path_uaddr) {
  char path[PATH_MAX];
  char name[FAT_NAME_MAX];
  fat_volume_t *v;
  fat_dirent_t ent;
  const char *rest;
  uint32_t dir;
  uint32_t cluster = 0;
  int bad;
  int64_t r;

  r = sys_copy_user_string(path_uaddr, path, sizeof(path));
  if (r < 0) {
    return r;
  }
  rest = split_volume(path, &v, &bad);
  if (v == 0 && !bad) {
    v = fs_default_volume();
  }
  if (v == 0 || bad) {
    return SYSCALL_ENOENT;
  }
  scheduler_preempt_disable();
  io_begin();
  if (!lookup_parent(v, rest, &dir, name)) {
    r = SYSCALL_ENOENT;
  } else if (fat_lookup(v, dir, name, &ent)) {
    r = SYSCALL_EEXIST;
  } else {
    r = fat_mkdir(v, dir, name, &cluster) ? 0 : SYSCALL_EIO;
  }
  io_end();
  scheduler_preempt_enable();
  return r;
}

// Volume `index` as a directory entry: name = its label (or "Disk N"), size = megabytes,
// attr = directory. Use "/@index/..." in paths to reach it. -ENOENT past the last one.
int64_t sysfile_volinfo(uint64_t index, uint64_t out_uaddr) {
  sysfile_dirent_t out;
  fat_volume_t *v;

  if (!paging_user_range_ok(out_uaddr, sizeof(out))) {
    return SYSCALL_EFAULT;
  }
  if (index >= (uint64_t)fs_volume_count()) {
    return SYSCALL_ENOENT;
  }
  v = fs_volume((int)index);
  if (v == 0) {
    return SYSCALL_ENOENT;
  }
  memset(&out, 0, sizeof(out));
  out.size = v->total_sectors / 2048U;
  out.attr = FAT_ATTR_DIR;
  str_copy(out.name, fs_volume_name((int)index), sizeof(out.name));
  memcpy((void *)(uintptr_t)out_uaddr, &out, sizeof(out));
  return 0;
}
