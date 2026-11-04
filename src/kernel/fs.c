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

void fs_selftest(void) {
  fat_volume_t *v = fs_volume(0);
  fat_dirent_t ent;
  uint32_t got = 0, testdir = 0, sub = 0, i;
  char name[64];

  if (!v) {
    console_write_string("[fstest] no volume\r\n");
    return;
  }
  console_write_string("[fstest] start\r\n");

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
