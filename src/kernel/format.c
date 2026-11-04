#include "format.h"
#include "block.h"
#include "disk.h"
#include "rtc.h"
#include "string.h"
#include "timer.h"

// GUIDs in on-disk (mixed-endian) byte order.
// C12A7328-F81F-11D2-BA4B-00A0C93EC93B
const uint8_t GPT_TYPE_ESP[16] = {
  0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11, 0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B
};
// EBD0A0A2-B9E5-4433-87C0-68B6B72699C7
const uint8_t GPT_TYPE_BASIC_DATA[16] = {
  0xA2, 0xA0, 0xD0, 0xEB, 0xE5, 0xB9, 0x33, 0x44, 0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7
};

#define GPT_ENTRIES      128u
#define GPT_ENTRY_SIZE   128u
#define GPT_ENTRY_SECTORS ((GPT_ENTRIES * GPT_ENTRY_SIZE) / 512u)   // 32
#define GPT_FIRST_USABLE 34u
#define ALIGN_SECTORS    2048u      // 1 MiB

#define ZERO_CHUNK_SECTORS 128u

static uint8_t g_zero[ZERO_CHUNK_SECTORS * 512u];
static uint8_t g_gpt_entries[GPT_ENTRIES * GPT_ENTRY_SIZE];

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void put64(uint8_t *p, uint64_t v) {
  put32(p, (uint32_t)v);
  put32(p + 4, (uint32_t)(v >> 32));
}

// ---- CRC-32 and GUIDs ------------------------------------------------------

uint32_t crc32(const void *data, uint32_t len) {
  const uint8_t *p = (const uint8_t *)data;
  uint32_t crc = 0xFFFFFFFFu, i;
  int bit;
  for (i = 0; i < len; i++) {
    crc ^= p[i];
    for (bit = 0; bit < 8; bit++) {
      crc = (crc & 1u) ? (crc >> 1) ^ 0xEDB88320u : (crc >> 1);
    }
  }
  return ~crc;
}

static uint64_t rdtsc(void) {
  uint32_t lo, hi;
  __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
  return ((uint64_t)hi << 32) | lo;
}

static uint64_t g_rng = 0;

static uint64_t next_random(void) {
  if (g_rng == 0) {
    int h = 0, m = 0, s = 0;
    rtc_read_time(&h, &m, &s);
    g_rng = rdtsc() ^ (timer_get_ms() << 20) ^ ((uint64_t)(h * 3600 + m * 60 + s) << 40) ^
            0x9E3779B97F4A7C15ull;
  }
  g_rng ^= rdtsc();
  g_rng ^= g_rng >> 12;           // xorshift64*
  g_rng ^= g_rng << 25;
  g_rng ^= g_rng >> 27;
  return g_rng * 0x2545F4914F6CDD1Dull;
}

void make_guid(uint8_t out[16]) {
  uint64_t a = next_random(), b = next_random();
  int i;
  for (i = 0; i < 8; i++) {
    out[i] = (uint8_t)(a >> (8 * i));
    out[8 + i] = (uint8_t)(b >> (8 * i));
  }
  out[7] = (uint8_t)((out[7] & 0x0F) | 0x40);     // version 4
  out[8] = (uint8_t)((out[8] & 0x3F) | 0x80);     // RFC 4122 variant
}

// ---- Raw writes ------------------------------------------------------------

static int zero_sectors(int disk, uint64_t lba, uint64_t count) {
  while (count > 0) {
    uint32_t chunk = count > ZERO_CHUNK_SECTORS ? ZERO_CHUNK_SECTORS : (uint32_t)count;
    if (!disk_write(disk, lba, chunk, g_zero)) {
      return 0;
    }
    lba += chunk;
    count -= chunk;
  }
  return 1;
}

static int zero_block_sectors(int dev, uint64_t lba, uint64_t count) {
  while (count > 0) {
    uint32_t chunk = count > ZERO_CHUNK_SECTORS ? ZERO_CHUNK_SECTORS : (uint32_t)count;
    if (!block_write(dev, lba, chunk, g_zero)) {
      return 0;
    }
    lba += chunk;
    count -= chunk;
  }
  return 1;
}

int disk_wipe_metadata(int disk) {
  const disk_info_t *d = disk_get(disk);
  if (!d || d->sectors < 8192) {
    return 0;
  }
  return zero_sectors(disk, 0, ALIGN_SECTORS) &&
         zero_sectors(disk, d->sectors - ALIGN_SECTORS, ALIGN_SECTORS);
}

// ---- GPT -------------------------------------------------------------------

static void build_header(uint8_t hdr[512], uint64_t my_lba, uint64_t alt_lba, uint64_t entries_lba,
                         uint64_t last_usable, const uint8_t disk_guid[16], uint32_t entries_crc) {
  memset(hdr, 0, 512);
  memcpy(hdr, "EFI PART", 8);
  put32(hdr + 8, 0x00010000u);
  put32(hdr + 12, 92);
  put64(hdr + 24, my_lba);
  put64(hdr + 32, alt_lba);
  put64(hdr + 40, GPT_FIRST_USABLE);
  put64(hdr + 48, last_usable);
  memcpy(hdr + 56, disk_guid, 16);
  put64(hdr + 72, entries_lba);
  put32(hdr + 80, GPT_ENTRIES);
  put32(hdr + 84, GPT_ENTRY_SIZE);
  put32(hdr + 88, entries_crc);
  put32(hdr + 16, 0);
  put32(hdr + 16, crc32(hdr, 92));
}

int gpt_write(int disk, const gpt_part_t *parts, int count) {
  const disk_info_t *d = disk_get(disk);
  uint8_t sector[512];
  uint8_t disk_guid[16];
  uint64_t last_lba, last_usable;
  uint32_t entries_crc;
  int i, j;

  if (!d || count < 1 || count > GPT_MAX_PARTS || d->sectors < 8192) {
    return 0;
  }
  last_lba = d->sectors - 1;
  last_usable = last_lba - GPT_ENTRY_SECTORS - 1;

  memset(g_gpt_entries, 0, sizeof(g_gpt_entries));
  for (i = 0; i < count; i++) {
    uint8_t *e = g_gpt_entries + (uint32_t)i * GPT_ENTRY_SIZE;
    uint64_t end = parts[i].start + parts[i].sectors - 1;
    if (parts[i].sectors == 0 || parts[i].start < GPT_FIRST_USABLE || end > last_usable) {
      return 0;
    }
    for (j = 0; j < i; j++) {                  // no overlaps
      uint64_t other_end = parts[j].start + parts[j].sectors - 1;
      if (parts[i].start <= other_end && parts[j].start <= end) {
        return 0;
      }
    }
    memcpy(e, parts[i].type_guid, 16);
    make_guid(e + 16);
    put64(e + 32, parts[i].start);
    put64(e + 40, end);
    for (j = 0; j < 35 && parts[i].name[j]; j++) {
      put16(e + 56 + j * 2, (uint16_t)(uint8_t)parts[i].name[j]);
    }
  }
  entries_crc = crc32(g_gpt_entries, sizeof(g_gpt_entries));
  make_guid(disk_guid);

  // Backup first (entries, then header), so an interrupted write leaves the old
  // primary table in place until the very end.
  if (!disk_write(disk, last_lba - GPT_ENTRY_SECTORS, GPT_ENTRY_SECTORS, g_gpt_entries)) {
    return 0;
  }
  build_header(sector, last_lba, 1, last_lba - GPT_ENTRY_SECTORS, last_usable, disk_guid, entries_crc);
  if (!disk_write(disk, last_lba, 1, sector)) {
    return 0;
  }
  if (!disk_write(disk, 2, GPT_ENTRY_SECTORS, g_gpt_entries)) {
    return 0;
  }
  build_header(sector, 1, last_lba, 2, last_usable, disk_guid, entries_crc);
  if (!disk_write(disk, 1, 1, sector)) {
    return 0;
  }

  // Protective MBR
  memset(sector, 0, 512);
  sector[446 + 0] = 0x00;
  sector[446 + 1] = 0x00;
  sector[446 + 2] = 0x02;
  sector[446 + 3] = 0x00;
  sector[446 + 4] = 0xEE;
  sector[446 + 5] = 0xFF;
  sector[446 + 6] = 0xFF;
  sector[446 + 7] = 0xFF;
  put32(sector + 446 + 8, 1);
  put32(sector + 446 + 12, last_lba > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)last_lba);
  sector[510] = 0x55;
  sector[511] = 0xAA;
  return disk_write(disk, 0, 1, sector);
}

int gpt_layout_install(int disk, uint32_t esp_mib,
                       uint64_t *esp_start, uint64_t *esp_sectors,
                       uint64_t *data_start, uint64_t *data_sectors) {
  const disk_info_t *d = disk_get(disk);
  gpt_part_t parts[2];
  uint64_t last_usable, end_aligned;

  if (!d || esp_mib < 34) {
    return 0;
  }
  last_usable = d->sectors - 1 - GPT_ENTRY_SECTORS - 1;
  *esp_start = ALIGN_SECTORS;
  *esp_sectors = (uint64_t)esp_mib * 2048u;
  *data_start = *esp_start + *esp_sectors;
  end_aligned = ((last_usable + 1) / ALIGN_SECTORS) * ALIGN_SECTORS;
  if (end_aligned <= *data_start + 70000u) {
    return 0;                                   // too small for a FAT32 data partition
  }
  *data_sectors = end_aligned - *data_start;

  memset(parts, 0, sizeof(parts));
  parts[0].type_guid = GPT_TYPE_ESP;
  parts[0].start = *esp_start;
  parts[0].sectors = *esp_sectors;
  str_copy(parts[0].name, "EFI System Partition", sizeof(parts[0].name));
  parts[1].type_guid = GPT_TYPE_BASIC_DATA;
  parts[1].start = *data_start;
  parts[1].sectors = *data_sectors;
  str_copy(parts[1].name, "WinClone", sizeof(parts[1].name));
  return gpt_write(disk, parts, 2);
}

// ---- FAT32 formatter -------------------------------------------------------

static uint32_t pick_cluster_sectors(uint64_t total) {
  if (total <= 532480ull) return 1;             // up to 260 MB
  if (total <= 16777216ull) return 8;           // up to 8 GB
  if (total <= 33554432ull) return 16;          // up to 16 GB
  if (total <= 67108864ull) return 32;          // up to 32 GB
  return 64;
}

int fat32_format(int dev, const char *label, void (*progress)(int percent)) {
  const block_dev_t *blk = block_get(dev);
  uint8_t sector[512];
  uint8_t guid[16];
  uint32_t total, spc, reserved = 32, nfats = 2, fat_size, clusters, i;
  uint64_t done = 0, work;
  char lab[11];
  uint16_t date = 0x5A21, time = 0;
  int y, mo, d, h, mi, s;

  if (!blk || blk->sectors < 66600ull || blk->sectors > 0xFFFFFFFFull) {
    return 0;                                   // too small for FAT32, or beyond 32-bit sector counts
  }
  total = (uint32_t)blk->sectors;
  spc = pick_cluster_sectors(total);

  fat_size = 1;
  for (;;) {
    uint32_t data = total - reserved - nfats * fat_size;
    uint32_t needed;
    clusters = data / spc;
    needed = (uint32_t)((((uint64_t)clusters + 2u) * 4u + 511u) / 512u);
    if (needed <= fat_size) {
      break;
    }
    fat_size = needed;
  }
  if (clusters < 65525u) {
    return 0;
  }

  for (i = 0; i < 11; i++) {
    lab[i] = ' ';
  }
  for (i = 0; i < 11 && label && label[i]; i++) {
    lab[i] = char_upper(label[i]);
  }
  if (rtc_read_date(&y, &mo, &d) && rtc_read_time(&h, &mi, &s) && y >= 1980) {
    date = (uint16_t)(((y - 1980) << 9) | (mo << 5) | d);
    time = (uint16_t)((h << 11) | (mi << 5) | (s / 2));
  }

  work = (uint64_t)reserved + (uint64_t)nfats * fat_size + spc;
  if (progress) progress(0);

  // Reserved area: clear, then write boot sector, FSInfo and their backups.
  if (!zero_block_sectors(dev, 0, reserved)) {
    return 0;
  }
  done += reserved;

  make_guid(guid);
  memset(sector, 0, 512);
  sector[0] = 0xEB; sector[1] = 0x58; sector[2] = 0x90;
  memcpy(sector + 3, "WINCLONE", 8);
  put16(sector + 11, 512);
  sector[13] = (uint8_t)spc;
  put16(sector + 14, (uint16_t)reserved);
  sector[16] = (uint8_t)nfats;
  put16(sector + 17, 0);
  put16(sector + 19, 0);
  sector[21] = 0xF8;
  put16(sector + 22, 0);
  put16(sector + 24, 63);
  put16(sector + 26, 255);
  put32(sector + 28, (uint32_t)blk->start);
  put32(sector + 32, total);
  put32(sector + 36, fat_size);
  put16(sector + 40, 0);
  put16(sector + 42, 0);
  put32(sector + 44, 2);                        // root directory cluster
  put16(sector + 48, 1);                        // FSInfo sector
  put16(sector + 50, 6);                        // backup boot sector
  sector[64] = 0x80;
  sector[66] = 0x29;
  memcpy(sector + 67, guid, 4);                 // volume serial
  memcpy(sector + 71, lab, 11);
  memcpy(sector + 82, "FAT32   ", 8);
  sector[510] = 0x55;
  sector[511] = 0xAA;
  if (!block_write(dev, 0, 1, sector) || !block_write(dev, 6, 1, sector)) {
    return 0;
  }

  memset(sector, 0, 512);
  put32(sector + 0, 0x41615252u);
  put32(sector + 484, 0x61417272u);
  put32(sector + 488, clusters - 1u);           // free clusters (root uses one)
  put32(sector + 492, 3u);                      // next free hint
  put32(sector + 508, 0xAA550000u);
  if (!block_write(dev, 1, 1, sector) || !block_write(dev, 7, 1, sector)) {
    return 0;
  }

  // FATs: clear everything, then mark the reserved entries and the root chain.
  for (i = 0; i < nfats; i++) {
    uint32_t start = reserved + i * fat_size, left = fat_size;
    while (left > 0) {
      uint32_t chunk = left > ZERO_CHUNK_SECTORS ? ZERO_CHUNK_SECTORS : left;
      if (!block_write(dev, start, chunk, g_zero)) {
        return 0;
      }
      start += chunk;
      left -= chunk;
      done += chunk;
      if (progress) progress((int)((done * 100u) / work));
    }
    memset(sector, 0, 512);
    put32(sector + 0, 0x0FFFFFF8u);
    put32(sector + 4, 0x0FFFFFFFu);
    put32(sector + 8, 0x0FFFFFFFu);             // cluster 2: root directory, end of chain
    if (!block_write(dev, reserved + i * fat_size, 1, sector)) {
      return 0;
    }
  }

  // Root directory: one cluster with the volume label record.
  {
    uint32_t root_lba = reserved + nfats * fat_size;
    memset(sector, 0, 512);
    for (i = 0; i < spc; i++) {
      if (!block_write(dev, root_lba + i, 1, i == 0 ? sector : g_zero)) {
        return 0;
      }
    }
    memset(sector, 0, 512);
    memcpy(sector, lab, 11);
    sector[11] = 0x08;                          // volume label attribute
    put16(sector + 14, time);
    put16(sector + 16, date);
    put16(sector + 18, date);
    put16(sector + 22, time);
    put16(sector + 24, date);
    if (!block_write(dev, root_lba, 1, sector)) {
      return 0;
    }
  }
  if (progress) progress(100);
  return 1;
}
