#include "block.h"
#include "disk.h"
#include "string.h"

static block_dev_t g_devs[BLOCK_MAX_DEVICES];
static int g_count = 0;

static uint32_t le32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t le64(const uint8_t *p) {
  return (uint64_t)le32(p) | ((uint64_t)le32(p + 4) << 32);
}

static void make_name(char *out, int disk, int partition) {
  const disk_info_t *info = disk_get(disk);
  int n = (int)str_copy(out, info ? info->name : "disk", 16);
  if (partition > 0) {
    out[n++] = 'p';
    n += u32_to_str((uint32_t)partition, out + n);
  }
  out[n] = 0;
}

static void add_device(int disk, uint64_t start, uint64_t sectors, int partition) {
  block_dev_t *d;
  if (g_count >= BLOCK_MAX_DEVICES || sectors == 0) {
    return;
  }
  d = &g_devs[g_count++];
  d->disk = disk;
  d->start = start;
  d->sectors = sectors;
  d->partition = partition;
  make_name(d->name, disk, partition);
}

// A FAT boot sector (partition-less "superfloppy" disk) has a sane BPB where an
// MBR would have boot code and a partition table.
static int looks_like_fat_boot_sector(const uint8_t *s) {
  uint32_t bps = (uint32_t)s[11] | ((uint32_t)s[12] << 8);
  uint32_t spc = s[13];
  uint32_t reserved = (uint32_t)s[14] | ((uint32_t)s[15] << 8);
  uint32_t fats = s[16];
  if (s[510] != 0x55 || s[511] != 0xAA) {
    return 0;
  }
  if (bps != 512 || spc == 0 || (spc & (spc - 1)) != 0 || reserved == 0 || fats == 0 || fats > 2) {
    return 0;
  }
  return 1;
}

static void scan_gpt(int disk, uint64_t disk_sectors) {
  static uint8_t header[512];
  static uint8_t entries[512];
  uint64_t entries_lba;
  uint32_t count, entry_size, i;
  int part_no = 0;

  if (!disk_read(disk, 1, 1, header) || memcmp(header, "EFI PART", 8) != 0) {
    return;
  }
  entries_lba = le64(header + 72);
  count = le32(header + 80);
  entry_size = le32(header + 84);
  if (entry_size < 128 || entry_size > 512 || (512 % entry_size) != 0 || count > 128) {
    return;
  }
  for (i = 0; i < count; i++) {
    uint32_t per_sector = 512 / entry_size;
    uint32_t idx = i % per_sector;
    const uint8_t *e;
    uint64_t first, last;
    int empty = 1, k;

    if (idx == 0 && !disk_read(disk, entries_lba + i / per_sector, 1, entries)) {
      return;
    }
    e = entries + idx * entry_size;
    for (k = 0; k < 16; k++) {
      if (e[k]) {
        empty = 0;
      }
    }
    part_no++;
    if (empty) {
      continue;
    }
    first = le64(e + 32);
    last = le64(e + 40);
    if (last >= first && last < disk_sectors) {
      add_device(disk, first, last - first + 1, part_no);
    }
  }
}

static void scan_disk(int disk) {
  static uint8_t sector[512];
  const disk_info_t *d = disk_get(disk);
  int p, found = 0;

  if (!d || !disk_read(disk, 0, 1, sector)) {
    return;
  }
  if (looks_like_fat_boot_sector(sector)) {
    add_device(disk, 0, d->sectors, 0);
    return;
  }
  if (sector[510] == 0x55 && sector[511] == 0xAA) {
    if (sector[446 + 4] == 0xEE) {            // protective MBR: GPT follows
      scan_gpt(disk, d->sectors);
      return;
    }
    for (p = 0; p < 4; p++) {
      const uint8_t *e = sector + 446 + p * 16;
      uint8_t type = e[4];
      uint32_t first = le32(e + 8), len = le32(e + 12);
      if (type == 0 || len == 0 || type == 0x05 || type == 0x0F || type == 0x85) {
        continue;                              // empty or extended container
      }
      add_device(disk, first, len, p + 1);
      found = 1;
    }
    if (found) {
      return;
    }
  }
  add_device(disk, 0, d->sectors, 0);          // unknown layout: expose the raw disk
}

int block_rescan(void) {
  int i;

  g_count = 0;
  for (i = 0; i < disk_count(); i++) {
    scan_disk(i);
  }
  return g_count;
}

int block_init(void) {
  disk_init();
  return block_rescan();
}

int block_count(void) { return g_count; }

const block_dev_t *block_get(int index) {
  if (index < 0 || index >= g_count) {
    return 0;
  }
  return &g_devs[index];
}

int block_read(int dev, uint64_t lba, uint32_t count, void *buf) {
  const block_dev_t *d = block_get(dev);
  if (!d || lba + count > d->sectors) {
    return 0;
  }
  return disk_read(d->disk, d->start + lba, count, buf);
}

int block_write(int dev, uint64_t lba, uint32_t count, const void *buf) {
  const block_dev_t *d = block_get(dev);
  if (!d || lba + count > d->sectors) {
    return 0;
  }
  return disk_write(d->disk, d->start + lba, count, buf);
}
