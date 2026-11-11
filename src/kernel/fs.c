#include "fs.h"
#include "block.h"
#include "string.h"

static fat_volume_t g_volumes[FS_MAX_VOLUMES];
static char g_names[FS_MAX_VOLUMES][24];
static int g_count = 0;

static unsigned int g_generation = 0;

static int mount_all(void) {
  int devs = block_count(), i;

  g_count = 0;
  for (i = 0; i < devs && g_count < FS_MAX_VOLUMES; i++) {
    fat_volume_t *v = &g_volumes[g_count];
    if (!fat_mount(i, v)) {
      continue;
    }
    if (v->label[0]) {
      str_copy(g_names[g_count], v->label, sizeof(g_names[g_count]));
    } else {
      int n = (int)str_copy(g_names[g_count], "Disk ", sizeof(g_names[g_count]));
      u32_to_str((uint32_t)g_count, g_names[g_count] + n);
    }
    g_count++;
  }
  g_generation++;
  return g_count;
}

int fs_init(void) {
  block_init();
  return mount_all();
}

int fs_rescan(void) {
  block_rescan();
  return mount_all();
}

unsigned int fs_generation(void) { return g_generation; }

int fs_volume_count(void) { return g_count; }

fat_volume_t *fs_volume(int index) {
  return (index >= 0 && index < g_count) ? &g_volumes[index] : 0;
}

fat_volume_t *fs_default_volume(void) {
  int i;
  fat_dirent_t efi;
  for (i = 0; i < g_count; i++) {
    // Skip boot volumes (ESPs and the installation medium): they hold \EFI.
    if (!fat_lookup(&g_volumes[i], g_volumes[i].root_cluster, "EFI", &efi)) {
      return &g_volumes[i];
    }
  }
  return g_count > 0 ? &g_volumes[0] : 0;
}

const char *fs_volume_name(int index) {
  return (index >= 0 && index < g_count) ? g_names[index] : "";
}

#ifdef KERNEL_FS_SELFTEST
// ---- Self test (build with: make FS_SELFTEST=1) ----------------------------
// Stress-tests the FAT32 driver on volume 0 and logs PASS/FAIL to the console.
// Leaves a known tree behind so scripts can cross-check it from the host:
//   /BIG.BIN        200000 bytes, byte i = (i * 7 + i / 251) & 0xFF
//   /SMALL.BIN      5000 bytes, same pattern, overwritten from a larger file
//   /TESTDIR/       30 files "File number N with a long name.txt" (odd N deleted)
//   /TESTDIR/SUB/DEEP.TXT

#include "console.h"

static uint8_t g_test_buf[200000];
static uint8_t g_test_back[200000];

static void fill_pattern(uint8_t *buf, uint32_t len) {
  uint32_t i;
  for (i = 0; i < len; i++) {
    buf[i] = (uint8_t)(i * 7u + i / 251u);
  }
}

static int g_failures = 0;

static void check(int ok, const char *what) {
  console_printf("[fstest] %s: %s\r\n", ok ? "PASS" : "FAIL", what);
  if (!ok) {
    g_failures++;
  }
}

static int strcmp_label(const char *a, const char *b) {
  while (*a && *a == *b) {
    a++;
    b++;
  }
  return *a == *b;
}

// exFAT: files and directories made by Linux (out/run/ex.img, see scripts/make-exfat-image.sh)
// are read back, then the common tests below run on the exFAT volume.
static void exfat_pre_tests(fat_volume_t *v) {
  fat_dirent_t ent, dir;
  fat_file_t f;
  uint32_t got = 0, off, i, count = 0, predir = 0;
  int ok = 1;
  fat_iter_t it;
  char name[32];

  check(strcmp_label(v->label, "EXTEST"), "exFAT volume label");
  check(fat_lookup(v, v->root_cluster, "hello.txt", &ent) && ent.size == 17, "exFAT: HELLO.TXT found, 17 bytes");
  check(fat_lookup(v, v->root_cluster, "A fairly long file name that needs several name entries.txt", &ent) && ent.size == 5,
        "exFAT: long name (4 name records)");
  if (fat_lookup(v, v->root_cluster, "PRE.BIN", &ent) && ent.size == 3000000 && fat_file_open(v, &ent, &f)) {
    for (off = 0; off < 3000000 && ok; off += 200000) {
      if (!fat_file_read(&f, off, g_test_buf, 200000, &got) || got != 200000) {
        ok = 0;
        break;
      }
      for (i = 0; i < 200000; i++) {
        uint32_t p = off + i;

        if (g_test_buf[i] != (uint8_t)(p * 13u + p / 97u)) {
          ok = 0;
          break;
        }
      }
    }
    check(ok, "exFAT: PRE.BIN (3 MB, written by Linux) reads back correctly");
  } else {
    check(0, "exFAT: PRE.BIN present");
  }
  // Grow and patch a file Linux wrote (contiguous clusters: the driver must switch it to a FAT chain).
  if (fat_lookup(v, v->root_cluster, "PRE.BIN", &ent) && fat_file_open(v, &ent, &f)) {
    uint32_t w = 0;

    for (i = 0; i < 100000; i++) {
      uint32_t p = 3000000u + i;

      g_test_buf[i] = (uint8_t)(p * 13u + p / 97u);
    }
    check(fat_file_write(&f, 3000000, g_test_buf, 100000, &w) && w == 100000 && f.ent.size == 3100000,
          "exFAT: appended 100000 bytes to PRE.BIN");
    memset(g_test_buf, 0xAA, 5000);
    check(fat_file_write(&f, 1000003, g_test_buf, 5000, &w) && w == 5000, "exFAT: overwrote 5000 bytes inside PRE.BIN");
    ok = fat_lookup(v, v->root_cluster, "PRE.BIN", &ent) && ent.size == 3100000 && fat_file_open(v, &ent, &f) &&
         fat_file_read(&f, 2999990, g_test_buf, 200, &got) && got == 200;
    for (i = 0; ok && i < 200; i++) {
      uint32_t p = 2999990u + i;

      ok = g_test_buf[i] == (uint8_t)(p * 13u + p / 97u);
    }
    check(ok, "exFAT: PRE.BIN reads back across the old end");
  }
  check(fat_lookup(v, v->root_cluster, "PREDIR", &dir) && (dir.attr & FAT_ATTR_DIR), "exFAT: PREDIR is a directory");
  predir = dir.first_cluster;
  fat_iter_open(v, predir, &it);
  while (fat_iter_next(&it, &ent)) {
    count++;
  }
  check(count == 200, "exFAT: PREDIR lists 200 entries (several directory clusters)");
  ok = 1;
  for (i = 1; i <= 200 && ok; i += 17) {
    int n = (int)str_copy(name, "f", sizeof(name));
    uint32_t j;

    n += u32_to_str(i, name + n);
    str_copy(name + n, ".txt", sizeof(name) - (size_t)n);
    if (!fat_lookup(v, predir, name, &ent) || ent.size != i || !fat_read_file(v, &ent, g_test_back, 256, &got) ||
        got != i) {
      ok = 0;
      break;
    }
    for (j = 0; j < i; j++) {
      if (g_test_back[j] != (uint8_t)(j % 251u)) {
        ok = 0;
      }
    }
  }
  check(ok, "exFAT: spot-checked files in PREDIR");
  for (i = 201; i <= 330; i++) {                                  // grow the directory by writing into it
    int n = (int)str_copy(name, "g", sizeof(name));

    n += u32_to_str(i, name + n);
    str_copy(name + n, ".txt", sizeof(name) - (size_t)n);
    if (!fat_write_file(v, predir, name, g_test_buf, i)) {
      break;
    }
  }
  check(i == 331, "exFAT: 130 files added to PREDIR (directory grew, its entry was updated)");
}

void fs_selftest(void) {
  fat_volume_t *v = 0;
  fat_dirent_t ent;
  uint32_t got = 0, testdir = 0, sub = 0, i;
  char name[64];
  int vi;

  for (vi = 0; vi < fs_volume_count(); vi++) {
    if (fs_volume(vi)->exfat) {
      v = fs_volume(vi);
      break;
    }
  }
  if (!v) {
    v = fs_volume(0);
  }

  if (!v) {
    console_write_string("[fstest] no volume\r\n");
    return;
  }
  console_write_string("[fstest] start\r\n");
  if (v->exfat) {
    exfat_pre_tests(v);
  }

  // Large multi-cluster file
  fill_pattern(g_test_buf, 200000);
  check(fat_write_file(v, v->root_cluster, "BIG.BIN", g_test_buf, 200000), "write 200000-byte file");
  check(fat_lookup(v, v->root_cluster, "big.bin", &ent) && ent.size == 200000, "lookup is case-insensitive, size ok");
  memset(g_test_back, 0, sizeof(g_test_back));
  check(fat_read_file(v, &ent, g_test_back, sizeof(g_test_back), &got) && got == 200000 &&
        memcmp(g_test_buf, g_test_back, 200000) == 0, "read back matches");

  // Overwrite a large file with a smaller one (frees clusters)
  check(fat_write_file(v, v->root_cluster, "SMALL.BIN", g_test_buf, 200000), "write file to be shrunk");
  check(fat_write_file(v, v->root_cluster, "SMALL.BIN", g_test_buf, 5000), "overwrite with 5000 bytes");
  check(fat_lookup(v, v->root_cluster, "SMALL.BIN", &ent) && ent.size == 5000, "shrunk size ok");
  memset(g_test_back, 0, 5100);
  check(fat_read_file(v, &ent, g_test_back, 5100, &got) && got == 5000 &&
        memcmp(g_test_buf, g_test_back, 5000) == 0, "shrunk contents match");

  // Empty file and exact-cluster-multiple file
  check(fat_write_file(v, v->root_cluster, "EMPTY.TXT", g_test_buf, 0), "empty file");
  check(fat_write_file(v, v->root_cluster, "EXACT.BIN", g_test_buf, 2048), "file of exactly 2 clusters");
  check(fat_lookup(v, v->root_cluster, "EXACT.BIN", &ent) && ent.size == 2048, "exact-size entry");

  // Directory growth: many long-named files
  check(fat_mkdir(v, v->root_cluster, "TESTDIR", &testdir), "mkdir TESTDIR");
  check(!fat_mkdir(v, v->root_cluster, "testdir", 0), "mkdir of existing name is refused");
  for (i = 1; i <= 30; i++) {
    int n = (int)str_copy(name, "File number ", sizeof(name));
    n += u32_to_str(i, name + n);
    str_copy(name + n, " with a long name.txt", sizeof(name) - (size_t)n);
    if (!fat_write_file(v, testdir, name, g_test_buf, 100 + i)) {
      break;
    }
  }
  check(i == 31, "created 30 long-named files (directory grew past one cluster)");
  {
    int count = 0;
    fat_iter_t it;
    fat_iter_open(v, testdir, &it);
    while (fat_iter_next(&it, &ent)) {
      count++;
    }
    check(count == 30, "directory lists 30 entries");
  }
  for (i = 1; i <= 30; i += 2) {
    int n = (int)str_copy(name, "File number ", sizeof(name));
    n += u32_to_str(i, name + n);
    str_copy(name + n, " with a long name.txt", sizeof(name) - (size_t)n);
    if (!fat_lookup(v, testdir, name, &ent) || !fat_delete(v, &ent)) {
      break;
    }
  }
  check(i == 31, "deleted the odd-numbered files");
  {
    // Recreate one: should reuse freed directory records
    check(fat_write_file(v, testdir, "File number 1 with a long name.txt", g_test_buf, 11), "recreate a deleted name");
  }

  // Nested directory
  check(fat_mkdir(v, testdir, "SUB", &sub), "mkdir TESTDIR/SUB");
  check(fat_write_file(v, sub, "DEEP.TXT", "deep\r\n", 6), "file inside nested dir");
  check(fat_lookup(v, testdir, "SUB", &ent) && !fat_delete(v, &ent), "non-empty directory delete is refused");

  // Names
  check(!fat_write_file(v, v->root_cluster, "bad:name.txt", g_test_buf, 1), "invalid character refused");
  check(fat_write_file(v, v->root_cluster, "Mixed Case.Txt", g_test_buf, 3), "name needing a ~1 alias");
  check(fat_write_file(v, v->root_cluster, "Mixed Case 2.Txt", g_test_buf, 3), "second name with same alias base");

  console_printf("[fstest] done, %d failure(s)\r\n", g_failures);
}
#endif
