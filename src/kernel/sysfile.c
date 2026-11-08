#include "sysfile.h"
#include "fs.h"
#include "heap.h"
#include "paging.h"
#include "process.h"
#include "string.h"
#include "syscall.h"

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
  uint32_t pos;                // READ position
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
static int copy_user_path(uint64_t uaddr, char *out, size_t max) {
  size_t i;

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

// ---- open / close ------------------------------------------------------------

static void release(sysfile_t *f) {
  if (f->data != 0) {
    kfree(f->data);
  }
  if (f->iter != 0) {
    kfree(f->iter);
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

static int64_t open_read(sysfile_t *f, const char *path) {
  int n;

  for (n = 0; n < fs_volume_count(); ++n) {
    fat_volume_t *v = volume_at(n);
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
    if (ent.size > SYSFILE_MAX_SIZE) {
      return SYSCALL_EFBIG;
    }
    if (ent.size > 0) {
      uint32_t got = 0;
      int ok;

      f->data = (uint8_t *)kmalloc(ent.size);
      if (f->data == 0) {
        return SYSCALL_ENOMEM;
      }
      io_begin();
      ok = fat_read_file(v, &ent, f->data, ent.size, &got);
      io_end();
      if (!ok || got != ent.size) {
        return SYSCALL_EIO;
      }
    }
    f->len = ent.size;
    f->kind = KIND_READ;
    return 0;
  }
  return SYSCALL_ENOENT;
}

static int64_t open_write(sysfile_t *f, const char *path) {
  fat_volume_t *v = fs_default_volume();
  fat_dirent_t ent;
  int found;
  int ok;

  if (v == 0) {
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
  f->kind = KIND_WRITE;
  return 0;
}

int64_t sysfile_open(uint64_t path_uaddr, uint64_t flags) {
  char path[PATH_MAX];
  sysfile_table_t *t;
  sysfile_t *f = 0;
  int64_t r;
  int i;

  if (flags != SYSFILE_O_RDONLY && flags != SYSFILE_O_WRONLY) {
    return SYSCALL_EINVAL;
  }
  r = copy_user_path(path_uaddr, path, sizeof(path));
  if (r < 0) {
    return r;
  }
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
  r = (flags == SYSFILE_O_WRONLY) ? open_write(f, path) : open_read(f, path);
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
  if (f->kind == KIND_WRITE) {
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
    if (t->f[i].kind == KIND_WRITE && do_flush) {
      flush(&t->f[i]);
    }
    release(&t->f[i]);
  }
  kfree(t);
}

// ---- read / write / readdir --------------------------------------------------

int64_t sysfile_read(uint64_t fd, uint64_t buf, uint64_t len) {
  sysfile_t *f = get_fd(fd);
  uint64_t n;

  if (f == 0) {
    return SYSCALL_EBADF;
  }
  if (f->kind == KIND_DIR) {
    return SYSCALL_EISDIR;
  }
  if (f->kind != KIND_READ) {
    return SYSCALL_EBADF;                       // opened for writing
  }
  n = f->len - f->pos;
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
  uint64_t room;

  if (f == 0 || f->kind != KIND_WRITE) {
    return SYSCALL_EBADF;
  }
  if (len == 0) {
    return 0;
  }
  room = SYSFILE_MAX_SIZE - f->len;
  if (room == 0) {
    return SYSCALL_EFBIG;
  }
  if (len > room) {
    len = room;                                 // short write, like a full disk
  }
  if (!paging_user_range_ok(buf, len)) {
    return SYSCALL_EFAULT;
  }
  if (f->len + len > f->cap) {
    uint32_t cap = f->cap != 0 ? f->cap : 4096U;
    uint8_t *bigger;

    while (cap < f->len + len) {
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
  memcpy(f->data + f->len, (const void *)(uintptr_t)buf, (size_t)len);
  f->len += (uint32_t)len;
  return (int64_t)len;
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
