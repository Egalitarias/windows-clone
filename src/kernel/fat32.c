#include "fat32.h"
#include "scheduler.h"
static uint64_t fat_free_bytes_impl(fat_volume_t *v);
static int fat_mount_impl(int dev, fat_volume_t *v);
static void fat_iter_open_impl(fat_volume_t *v, uint32_t dir_cluster, fat_iter_t *it);
static int fat_iter_next_impl(fat_iter_t *it, fat_dirent_t *out);
static int fat_lookup_impl(fat_volume_t *v, uint32_t dir_cluster, const char *name, fat_dirent_t *out);
static int fat_read_file_impl(fat_volume_t *v, const fat_dirent_t *ent, void *buf, uint32_t max, uint32_t *out_len);
static int fat_write_file_impl(fat_volume_t *v, uint32_t dir_cluster, const char *name, const void *data, uint32_t len);
static int fat_mkdir_impl(fat_volume_t *v, uint32_t dir_cluster, const char *name, uint32_t *new_cluster);
static int fat_delete_impl(fat_volume_t *v, const fat_dirent_t *ent);
#include "block.h"
#include "rtc.h"
#include "string.h"

#define FAT_EOC_MIN   0x0FFFFFF8u
#define FAT_EOC       0x0FFFFFFFu
#define FAT_MASK      0x0FFFFFFFu

static uint8_t g_zero_sector[512];     // always zero
static uint8_t g_scratch[512];         // data-path sector scratch

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t le32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static int str_equal(const char *a, const char *b) {
  size_t i = 0;
  while (a[i] && a[i] == b[i]) {
    i++;
  }
  return a[i] == b[i];
}

static uint32_t cluster_bytes(const fat_volume_t *v) { return v->sectors_per_cluster * 512u; }

static uint32_t cluster_lba(const fat_volume_t *v, uint32_t cluster) {
  return v->data_lba + (cluster - 2u) * v->sectors_per_cluster;
}

static int is_eoc(const fat_volume_t *v, uint32_t value) { return value >= v->eoc_min; }

// ---- FAT table -------------------------------------------------------------

static int fat_load(fat_volume_t *v, uint32_t lba) {
  if (v->fat_buf_valid && v->fat_buf_lba == lba) {
    return 1;
  }
  v->fat_buf_valid = 0;
  if (!block_read(v->dev, lba, 1, v->fat_buf)) {
    return 0;
  }
  v->fat_buf_lba = lba;
  v->fat_buf_valid = 1;
  return 1;
}

static int fat_get(fat_volume_t *v, uint32_t cluster, uint32_t *value) {
  uint32_t width = (v->fat_bits == 16) ? 2u : 4u;
  uint32_t lba = v->fat_lba + (cluster * width) / 512u;
  uint32_t off = (cluster * width) % 512u;
  if (cluster >= v->cluster_count + 2u || !fat_load(v, lba)) {
    return 0;
  }
  *value = (v->fat_bits == 16) ? le16(v->fat_buf + off) : (le32(v->fat_buf + off) & FAT_MASK);
  return 1;
}

static int fat_set(fat_volume_t *v, uint32_t cluster, uint32_t value) {
  uint32_t width = (v->fat_bits == 16) ? 2u : 4u;
  uint32_t lba = v->fat_lba + (cluster * width) / 512u;
  uint32_t off = (cluster * width) % 512u;
  uint32_t i, old;

  if (cluster >= v->cluster_count + 2u || !fat_load(v, lba)) {
    return 0;
  }
  if (v->fat_bits == 16) {
    put16(v->fat_buf + off, (uint16_t)value);
  } else {
    old = le32(v->fat_buf + off);
    put32(v->fat_buf + off, (old & ~FAT_MASK) | (value & FAT_MASK));
  }
  for (i = 0; i < v->num_fats; i++) {
    if (!block_write(v->dev, lba + i * v->fat_size, 1, v->fat_buf)) {
      return 0;
    }
  }
  return 1;
}

static int zero_cluster(fat_volume_t *v, uint32_t cluster) {
  uint32_t i, lba = cluster_lba(v, cluster);
  for (i = 0; i < v->sectors_per_cluster; i++) {
    if (!block_write(v->dev, lba + i, 1, g_zero_sector)) {
      return 0;
    }
  }
  return 1;
}

// Allocate a free cluster, mark it end-of-chain. Returns 0 if the volume is full.
static uint32_t alloc_cluster(fat_volume_t *v) {
  uint32_t total = v->cluster_count;
  uint32_t i, c, value;

  if (v->next_free < 2 || v->next_free >= total + 2u) {
    v->next_free = 2;
  }
  for (i = 0; i < total; i++) {
    c = v->next_free + i;
    if (c >= total + 2u) {
      c -= total;
    }
    if (fat_get(v, c, &value) && value == 0) {
      if (!fat_set(v, c, v->eoc)) {
        return 0;
      }
      v->next_free = c + 1;
      return c;
    }
  }
  return 0;
}

static void free_chain(fat_volume_t *v, uint32_t cluster) {
  uint32_t guard = 0, next;
  while (cluster >= 2 && cluster < v->cluster_count + 2u && guard++ < v->cluster_count) {
    if (!fat_get(v, cluster, &next)) {
      return;
    }
    fat_set(v, cluster, 0);
    if (cluster < v->next_free) {
      v->next_free = cluster;
    }
    if (is_eoc(v, next)) {
      return;
    }
    cluster = next;
  }
}

static uint64_t fat_free_bytes_impl(fat_volume_t *v) {
  uint32_t c, value;
  uint64_t free_clusters = 0;
  for (c = 2; c < v->cluster_count + 2u; c++) {
    if (fat_get(v, c, &value) && value == 0) {
      free_clusters++;
    }
  }
  return free_clusters * cluster_bytes(v);
}

// ---- Mounting --------------------------------------------------------------

static int fat_mount_impl(int dev, fat_volume_t *v) {
  uint8_t sec[512];
  uint32_t bps, spc, total, root_entries, fat16, root_dir_sectors;
  int i, label_at;

  if (!block_read(dev, 0, 1, sec)) {
    return 0;
  }
  if (sec[510] != 0x55 || sec[511] != 0xAA) {
    return 0;
  }
  bps = le16(sec + 11);
  spc = sec[13];
  if (bps != 512 || spc == 0 || (spc & (spc - 1)) != 0) {
    return 0;
  }
  root_entries = le16(sec + 17);
  fat16 = le16(sec + 22);
  total = le16(sec + 19) ? le16(sec + 19) : le32(sec + 32);

  v->dev = dev;
  v->sectors_per_cluster = spc;
  v->reserved_sectors = le16(sec + 14);
  v->num_fats = sec[16];
  v->total_sectors = total;
  v->fat_lba = v->reserved_sectors;
  if (v->num_fats == 0 || v->num_fats > 2 || v->reserved_sectors == 0 || total == 0) {
    return 0;
  }

  if (fat16 != 0) {                          // FAT12 / FAT16: fixed root directory
    if (root_entries == 0) {
      return 0;
    }
    root_dir_sectors = (root_entries * 32u + 511u) / 512u;
    v->fat_size = fat16;
    v->root_dir_lba = v->reserved_sectors + v->num_fats * fat16;
    v->root_dir_entries = root_entries;
    v->data_lba = v->root_dir_lba + root_dir_sectors;
    if (v->data_lba >= total) {
      return 0;
    }
    v->cluster_count = (total - v->data_lba) / spc;
    if (v->cluster_count < 4085u || v->cluster_count >= 65525u) {
      return 0;                              // FAT12 (too small) is not supported
    }
    v->fat_bits = 16;
    v->eoc_min = 0xFFF8u;
    v->eoc = 0xFFFFu;
    v->root_cluster = 0;
    label_at = 43;
  } else {                                   // FAT32
    if (root_entries != 0 || le32(sec + 36) == 0) {
      return 0;
    }
    v->fat_size = le32(sec + 36);
    v->root_cluster = le32(sec + 44);
    v->root_dir_lba = 0;
    v->root_dir_entries = 0;
    v->data_lba = v->reserved_sectors + v->num_fats * v->fat_size;
    if (v->data_lba >= total || v->root_cluster < 2) {
      return 0;
    }
    v->cluster_count = (total - v->data_lba) / spc;
    v->fat_bits = 32;
    v->eoc_min = FAT_EOC_MIN;
    v->eoc = FAT_EOC;
    label_at = 71;
  }
  v->next_free = 2;
  v->fat_buf_valid = 0;
  for (i = 0; i < 11; i++) {
    v->label[i] = (char)sec[label_at + i];
  }
  v->label[11] = 0;
  for (i = 10; i >= 0 && v->label[i] == ' '; i--) {
    v->label[i] = 0;
  }
  if (str_eq_nocase(v->label, "NO NAME")) {
    v->label[0] = 0;
  }
  return 1;
}

// ---- Directory iteration ---------------------------------------------------

static void fat_iter_open_impl(fat_volume_t *v, uint32_t dir_cluster, fat_iter_t *it) {
  it->v = v;
  it->dir_cluster = dir_cluster;
  it->cluster = dir_cluster;
  it->index = 0;
  it->abs = 0;
  it->sec_valid = 0;
  it->fixed_root = (dir_cluster == 0 && v->fat_bits == 16);
  it->done = (dir_cluster < 2 && !it->fixed_root);
}

// Read the next raw 32-byte record (including deleted and end markers). Returns
// 0 at the end of the cluster chain.
static int iter_raw(fat_iter_t *it, uint8_t out[32], uint32_t *abs_index) {
  fat_volume_t *v = it->v;
  uint32_t per_cluster = v->sectors_per_cluster * 16u;
  uint32_t lba, off, next;

  if (it->done) {
    return 0;
  }
  if (it->fixed_root) {
    if (it->abs >= v->root_dir_entries) {
      it->done = 1;
      return 0;
    }
    lba = v->root_dir_lba + (it->abs * 32u) / 512u;
    off = (it->abs * 32u) % 512u;
  } else {
    lba = cluster_lba(v, it->cluster) + (it->index * 32u) / 512u;
    off = (it->index * 32u) % 512u;
  }
  if (!it->sec_valid || it->sec_lba != lba) {
    if (!block_read(v->dev, lba, 1, it->sec)) {
      it->done = 1;
      return 0;
    }
    it->sec_lba = lba;
    it->sec_valid = 1;
  }
  memcpy(out, it->sec + off, 32);
  *abs_index = it->abs;

  it->abs++;
  it->index++;
  if (it->fixed_root) {
    return 1;                                  // no chain to follow
  }
  if (it->index >= per_cluster) {
    if (!fat_get(v, it->cluster, &next) || is_eoc(v, next) || next < 2) {
      it->done = 1;
    } else {
      it->cluster = next;
      it->index = 0;
    }
  }
  return 1;
}

static uint8_t sfn_checksum(const uint8_t *sfn) {
  uint8_t sum = 0;
  int i;
  for (i = 0; i < 11; i++) {
    sum = (uint8_t)(((sum & 1) ? 0x80 : 0) + (sum >> 1) + sfn[i]);
  }
  return sum;
}

static void lfn_collect(const uint8_t *e, char *name, uint32_t *length) {
  static const uint8_t offsets[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
  uint32_t base = (uint32_t)((e[0] & 0x1F) - 1) * 13u;
  int i;

  for (i = 0; i < 13; i++) {
    uint16_t ch = le16(e + offsets[i]);
    if (ch == 0x0000 || ch == 0xFFFF) {
      break;
    }
    if (base + (uint32_t)i < FAT_NAME_MAX - 1) {
      name[base + i] = (ch < 0x80) ? (char)ch : '?';
      if (base + (uint32_t)i + 1 > *length) {
        *length = base + (uint32_t)i + 1;
      }
    }
  }
}

static int fat_iter_next_impl(fat_iter_t *it, fat_dirent_t *out) {
  uint8_t e[32];
  uint32_t abs_index;
  char lfn[FAT_NAME_MAX];
  uint32_t lfn_len = 0, lfn_count = 0;
  int lfn_valid = 0;
  uint8_t lfn_sum = 0;

  while (iter_raw(it, e, &abs_index)) {
    int i, n;
    uint8_t attr;

    if (e[0] == 0x00) {
      it->done = 1;
      return 0;
    }
    if (e[0] == 0xE5) {
      lfn_valid = 0;
      lfn_count = 0;
      lfn_len = 0;
      continue;
    }
    attr = e[11];
    if ((attr & 0x3F) == 0x0F) {                  // long-name record
      if (e[0] & 0x40) {                          // last (highest) piece comes first
        lfn_valid = 1;
        lfn_count = 0;
        lfn_len = 0;
        lfn_sum = e[13];
        memset(lfn, 0, sizeof(lfn));
      }
      if (lfn_valid) {
        lfn_collect(e, lfn, &lfn_len);
        lfn_count++;
      }
      continue;
    }
    if (attr & FAT_ATTR_VOLUME) {
      lfn_valid = 0;
      lfn_count = 0;
      continue;
    }

    // 8.3 record. Use the long name if its checksum matches.
    memcpy(out->sfn, e, 11);
    if (lfn_valid && lfn_sum == sfn_checksum(e) && lfn_len > 0) {
      lfn[lfn_len] = 0;
      str_copy(out->name, lfn, FAT_NAME_MAX);
    } else {
      char base[9], ext[4];
      lfn_count = 0;
      for (i = 0; i < 8; i++) base[i] = (char)e[i];
      base[8] = 0;
      for (i = 0; i < 3; i++) ext[i] = (char)e[8 + i];
      ext[3] = 0;
      if ((uint8_t)base[0] == 0x05) {
        base[0] = (char)0xE5;
      }
      for (i = 7; i >= 0 && base[i] == ' '; i--) base[i] = 0;
      for (i = 2; i >= 0 && ext[i] == ' '; i--) ext[i] = 0;
      if (e[12] & 0x08) {
        for (i = 0; base[i]; i++) base[i] = char_lower(base[i]);
      }
      if (e[12] & 0x10) {
        for (i = 0; ext[i]; i++) ext[i] = char_lower(ext[i]);
      }
      n = (int)str_copy(out->name, base, FAT_NAME_MAX);
      if (ext[0]) {
        out->name[n++] = '.';
        str_copy(out->name + n, ext, (size_t)(FAT_NAME_MAX - n));
      }
    }
    if (str_eq_nocase(out->name, ".") || str_eq_nocase(out->name, "..")) {
      lfn_valid = 0;
      lfn_count = 0;
      continue;
    }
    out->attr = attr;
    out->first_cluster = ((uint32_t)le16(e + 20) << 16) | le16(e + 26);
    out->size = le32(e + 28);
    out->mtime = le16(e + 22);
    out->mdate = le16(e + 24);
    out->dir_cluster = it->dir_cluster;
    out->sfn_index = abs_index;
    out->lfn_count = lfn_count;
    return 1;
  }
  return 0;
}

static int fat_lookup_impl(fat_volume_t *v, uint32_t dir_cluster, const char *name, fat_dirent_t *out) {
  fat_iter_t it;
  fat_iter_open(v, dir_cluster, &it);
  while (fat_iter_next(&it, out)) {
    if (str_eq_nocase(out->name, name)) {
      return 1;
    }
  }
  return 0;
}

// ---- Reading files ---------------------------------------------------------

static int fat_read_file_impl(fat_volume_t *v, const fat_dirent_t *ent, void *buf, uint32_t max, uint32_t *out_len) {
  uint8_t *dst = (uint8_t *)buf;
  uint32_t remaining = ent->size < max ? ent->size : max;
  uint32_t cluster = ent->first_cluster;
  uint32_t total = remaining;
  uint32_t cbytes = cluster_bytes(v);
  uint32_t guard = 0, next, i;

  while (remaining > 0 && cluster >= 2 && guard++ <= v->cluster_count) {
    uint32_t lba = cluster_lba(v, cluster);
    if (remaining >= cbytes) {
      if (!block_read(v->dev, lba, v->sectors_per_cluster, dst)) {
        return 0;
      }
      dst += cbytes;
      remaining -= cbytes;
    } else {
      for (i = 0; i < v->sectors_per_cluster && remaining > 0; i++) {
        uint32_t take = remaining < 512 ? remaining : 512;
        if (!block_read(v->dev, lba + i, 1, g_scratch)) {
          return 0;
        }
        memcpy(dst, g_scratch, take);
        dst += take;
        remaining -= take;
      }
    }
    if (remaining == 0 || !fat_get(v, cluster, &next) || is_eoc(v, next)) {
      break;
    }
    cluster = next;
  }
  *out_len = total - remaining;
  return 1;
}

// ---- Directory records -----------------------------------------------------

// Locate record `abs` of a directory. With `extend`, grow the directory by a
// zeroed cluster when the index is just past its end.
static int dir_locate(fat_volume_t *v, uint32_t dir_cluster, uint32_t abs, int extend,
                      uint32_t *lba, uint32_t *offset) {
  uint32_t per_cluster = v->sectors_per_cluster * 16u;
  uint32_t hops = abs / per_cluster;
  uint32_t rem = abs % per_cluster;
  uint32_t cluster = dir_cluster, next;

  if (dir_cluster == 0 && v->fat_bits == 16) {  // fixed root directory: no growth
    if (abs >= v->root_dir_entries) {
      return 0;
    }
    *lba = v->root_dir_lba + (abs * 32u) / 512u;
    *offset = (abs * 32u) % 512u;
    return 1;
  }
  while (hops > 0) {
    if (!fat_get(v, cluster, &next)) {
      return 0;
    }
    if (is_eoc(v, next) || next < 2) {
      uint32_t fresh;
      if (!extend) {
        return 0;
      }
      fresh = alloc_cluster(v);
      if (fresh == 0 || !zero_cluster(v, fresh) || !fat_set(v, cluster, fresh)) {
        return 0;
      }
      next = fresh;
    }
    cluster = next;
    hops--;
  }
  *lba = cluster_lba(v, cluster) + (rem * 32u) / 512u;
  *offset = (rem * 32u) % 512u;
  return 1;
}

static int dir_write_record(fat_volume_t *v, uint32_t dir_cluster, uint32_t abs, const uint8_t rec[32]) {
  uint32_t lba, off;
  uint8_t sec[512];
  if (!dir_locate(v, dir_cluster, abs, 1, &lba, &off) || !block_read(v->dev, lba, 1, sec)) {
    return 0;
  }
  memcpy(sec + off, rec, 32);
  return block_write(v->dev, lba, 1, sec);
}

static void dos_now(uint16_t *date, uint16_t *time) {
  int y = 2026, mo = 1, d = 1, h = 0, mi = 0, s = 0;
  rtc_read_date(&y, &mo, &d);
  rtc_read_time(&h, &mi, &s);
  if (y < 1980) {
    y = 1980;
  }
  *date = (uint16_t)(((y - 1980) << 9) | (mo << 5) | d);
  *time = (uint16_t)((h << 11) | (mi << 5) | (s / 2));
}

static int update_record(fat_volume_t *v, const fat_dirent_t *ent) {
  uint32_t lba, off;
  uint8_t sec[512];
  uint16_t date, time;

  if (!dir_locate(v, ent->dir_cluster, ent->sfn_index, 0, &lba, &off) ||
      !block_read(v->dev, lba, 1, sec)) {
    return 0;
  }
  dos_now(&date, &time);
  put16(sec + off + 20, (uint16_t)(ent->first_cluster >> 16));
  put16(sec + off + 26, (uint16_t)(ent->first_cluster & 0xFFFF));
  put32(sec + off + 28, ent->size);
  put16(sec + off + 22, time);
  put16(sec + off + 24, date);
  put16(sec + off + 18, date);
  return block_write(v->dev, lba, 1, sec);
}

// ---- Names -----------------------------------------------------------------

static int name_ok(const char *name) {
  size_t i, n = str_len(name);
  if (n == 0 || n >= FAT_NAME_MAX) {
    return 0;
  }
  if ((n == 1 && name[0] == '.') || (n == 2 && name[0] == '.' && name[1] == '.')) {
    return 0;
  }
  if (name[n - 1] == ' ' || name[n - 1] == '.') {
    return 0;
  }
  for (i = 0; i < n; i++) {
    char c = name[i];
    if ((uint8_t)c < 0x20 || c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
        c == '"' || c == '<' || c == '>' || c == '|') {
      return 0;
    }
  }
  return 1;
}

static int sfn_char_ok(char c) {
  if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
    return 1;
  }
  switch (c) {
    case '$': case '%': case '\'': case '-': case '_': case '@': case '~': case '`':
    case '!': case '(': case ')': case '{': case '}': case '^': case '#': case '&':
      return 1;
    default:
      return 0;
  }
}

// Build the 8.3 form of `name`. *lossy is set when characters were dropped or
// replaced, or the name was truncated (a numeric tail is then required).
static void make_sfn(const char *name, uint8_t sfn[11], int *lossy) {
  size_t n = str_len(name), dot = n, i;
  int b = 0, e = 0;

  *lossy = 0;
  memset(sfn, ' ', 11);
  for (i = n; i > 1; i--) {            // last dot that is not the first character
    if (name[i - 1] == '.') {
      dot = i - 1;
      break;
    }
  }
  for (i = 0; i < dot; i++) {
    char c = char_upper(name[i]);
    if (c == ' ' || c == '.') {
      *lossy = 1;
      continue;
    }
    if (!sfn_char_ok(c)) {
      c = '_';
      *lossy = 1;
    }
    if (b < 8) {
      sfn[b++] = (uint8_t)c;
    } else {
      *lossy = 1;
    }
  }
  if (dot < n) {
    for (i = dot + 1; i < n; i++) {
      char c = char_upper(name[i]);
      if (c == ' ') {
        *lossy = 1;
        continue;
      }
      if (!sfn_char_ok(c)) {
        c = '_';
        *lossy = 1;
      }
      if (e < 3) {
        sfn[8 + e++] = (uint8_t)c;
      } else {
        *lossy = 1;
      }
    }
  }
  if (b == 0) {
    sfn[0] = '_';
    *lossy = 1;
  }
}

static int sfn_in_use(fat_volume_t *v, uint32_t dir_cluster, const uint8_t sfn[11]) {
  fat_iter_t it;
  uint8_t e[32];
  uint32_t abs_index;
  fat_iter_open(v, dir_cluster, &it);
  while (iter_raw(&it, e, &abs_index)) {
    if (e[0] == 0x00) {
      return 0;
    }
    if (e[0] != 0xE5 && (e[11] & 0x3F) != 0x0F && memcmp(e, sfn, 11) == 0) {
      return 1;
    }
  }
  return 0;
}

// Add the "~N" tail to a base name, keeping the whole thing within 8 characters.
static void apply_tail(uint8_t sfn[11], uint32_t n) {
  char digits[11];
  int dlen = u32_to_str(n, digits);
  int keep = 8 - (dlen + 1);
  int base_len = 0, i;
  while (base_len < 8 && sfn[base_len] != ' ') {
    base_len++;
  }
  if (base_len > keep) {
    base_len = keep;
  }
  for (i = base_len; i < 8; i++) {
    sfn[i] = ' ';
  }
  sfn[base_len] = '~';
  for (i = 0; i < dlen; i++) {
    sfn[base_len + 1 + i] = (uint8_t)digits[i];
  }
}

// ---- Creating records ------------------------------------------------------

// Fill `out` (and write to disk) with a new record for `name`.
static int create_record(fat_volume_t *v, uint32_t dir_cluster, const char *name, uint8_t attr,
                         uint32_t first_cluster, uint32_t size, fat_dirent_t *out) {
  uint8_t sfn[11];
  uint8_t rec[32];
  char shown[FAT_NAME_MAX + 4];
  int lossy, need_lfn, i;
  size_t name_len = str_len(name);
  uint32_t lfn_pieces, slots, slot_start = 0, run_start = 0, run = 0, abs_index;
  fat_iter_t it;
  uint8_t e[32];
  uint16_t date, time;
  int found = 0;

  make_sfn(name, sfn, &lossy);
  {
    // Rebuild "BASE.EXT" to see whether the 8.3 form reproduces the name exactly.
    int n = 0;
    for (i = 0; i < 8 && sfn[i] != ' '; i++) shown[n++] = (char)sfn[i];
    if (sfn[8] != ' ') {
      shown[n++] = '.';
      for (i = 8; i < 11 && sfn[i] != ' '; i++) shown[n++] = (char)sfn[i];
    }
    shown[n] = 0;
    need_lfn = lossy || !str_equal(shown, name);
  }
  if (lossy || sfn_in_use(v, dir_cluster, sfn)) {
    uint32_t n;
    if (!lossy && !need_lfn) {
      return 0;                      // exact 8.3 name already present
    }
    for (n = 1; n < 100000; n++) {
      uint8_t cand[11];
      memcpy(cand, sfn, 11);
      apply_tail(cand, n);
      if (!sfn_in_use(v, dir_cluster, cand)) {
        memcpy(sfn, cand, 11);
        break;
      }
    }
    need_lfn = 1;
  }

  lfn_pieces = need_lfn ? (uint32_t)((name_len + 12) / 13) : 0;
  slots = lfn_pieces + 1;

  // Find a run of free records (deleted or past the end marker).
  fat_iter_open(v, dir_cluster, &it);
  while (iter_raw(&it, e, &abs_index)) {
    if (e[0] == 0x00) {
      slot_start = run > 0 ? run_start : abs_index;
      found = 1;
      break;
    }
    if (e[0] == 0xE5) {
      if (run == 0) {
        run_start = abs_index;
      }
      run++;
      if (run >= slots) {
        slot_start = run_start;
        found = 1;
        break;
      }
    } else {
      run = 0;
    }
  }
  if (!found) {
    slot_start = run > 0 ? run_start : it.abs;     // append at the very end
  }

  // Long-name records, highest piece first.
  for (i = (int)lfn_pieces; i >= 1; i--) {
    static const uint8_t offsets[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
    uint32_t base = (uint32_t)(i - 1) * 13u;
    int k;
    memset(rec, 0xFF, 32);
    rec[0] = (uint8_t)(i | ((uint32_t)i == lfn_pieces ? 0x40 : 0));
    rec[11] = 0x0F;
    rec[12] = 0;
    rec[13] = sfn_checksum(sfn);
    put16(rec + 26, 0);
    for (k = 0; k < 13; k++) {
      uint32_t pos = base + (uint32_t)k;
      uint16_t ch;
      if (pos < name_len) {
        ch = (uint8_t)name[pos];
      } else if (pos == name_len) {
        ch = 0x0000;
      } else {
        ch = 0xFFFF;
      }
      put16(rec + offsets[k], ch);
    }
    if (!dir_write_record(v, dir_cluster, slot_start + (lfn_pieces - (uint32_t)i), rec)) {
      return 0;
    }
  }

  dos_now(&date, &time);
  memset(rec, 0, 32);
  memcpy(rec, sfn, 11);
  rec[11] = attr;
  put16(rec + 14, time);
  put16(rec + 16, date);
  put16(rec + 18, date);
  put16(rec + 20, (uint16_t)(first_cluster >> 16));
  put16(rec + 22, time);
  put16(rec + 24, date);
  put16(rec + 26, (uint16_t)(first_cluster & 0xFFFF));
  put32(rec + 28, size);
  if (!dir_write_record(v, dir_cluster, slot_start + lfn_pieces, rec)) {
    return 0;
  }

  memset(out, 0, sizeof(*out));
  str_copy(out->name, name, FAT_NAME_MAX);
  memcpy(out->sfn, sfn, 11);
  out->attr = attr;
  out->size = size;
  out->first_cluster = first_cluster;
  out->mdate = date;
  out->mtime = time;
  out->dir_cluster = dir_cluster;
  out->sfn_index = slot_start + lfn_pieces;
  out->lfn_count = lfn_pieces;
  return 1;
}

// ---- Writing files ---------------------------------------------------------

static int fat_write_file_impl(fat_volume_t *v, uint32_t dir_cluster, const char *name,
                   const void *data, uint32_t len) {
  fat_dirent_t ent;
  const uint8_t *src = (const uint8_t *)data;
  uint32_t cbytes = cluster_bytes(v);
  uint32_t needed = (len + cbytes - 1) / cbytes;
  uint32_t first = 0, prev = 0, i, j;

  if (!name_ok(name)) {
    return 0;
  }
  if (fat_lookup(v, dir_cluster, name, &ent)) {
    if (ent.attr & (FAT_ATTR_DIR | FAT_ATTR_READONLY)) {
      return 0;
    }
    free_chain(v, ent.first_cluster);
    ent.first_cluster = 0;
    ent.size = 0;
  } else if (!create_record(v, dir_cluster, name, FAT_ATTR_ARCHIVE, 0, 0, &ent)) {
    return 0;
  }

  for (i = 0; i < needed; i++) {
    uint32_t c = alloc_cluster(v);
    uint32_t lba, remaining;

    if (c == 0) {
      free_chain(v, first);                   // volume full: roll back
      ent.first_cluster = 0;
      ent.size = 0;
      update_record(v, &ent);
      return 0;
    }
    if (prev) {
      fat_set(v, prev, c);
    } else {
      first = c;
    }
    prev = c;

    lba = cluster_lba(v, c);
    remaining = len - i * cbytes;
    if (remaining >= cbytes) {
      if (!block_write(v->dev, lba, v->sectors_per_cluster, src + (size_t)i * cbytes)) {
        return 0;
      }
    } else {
      for (j = 0; j < v->sectors_per_cluster; j++) {
        uint32_t take = remaining > 512 ? 512 : remaining;
        memset(g_scratch, 0, 512);
        if (take > 0) {
          memcpy(g_scratch, src + (size_t)i * cbytes + (size_t)j * 512, take);
        }
        if (!block_write(v->dev, lba + j, 1, g_scratch)) {
          return 0;
        }
        remaining -= take;
      }
    }
  }
  ent.first_cluster = first;
  ent.size = len;
  return update_record(v, &ent);
}

static int fat_mkdir_impl(fat_volume_t *v, uint32_t dir_cluster, const char *name, uint32_t *new_cluster) {
  fat_dirent_t existing, ent;
  uint32_t c;
  uint8_t sec[512];
  uint16_t date, time;
  uint32_t parent_ref = (dir_cluster == v->root_cluster) ? 0 : dir_cluster;

  if (!name_ok(name) || fat_lookup(v, dir_cluster, name, &existing)) {
    return 0;
  }
  c = alloc_cluster(v);
  if (c == 0 || !zero_cluster(v, c)) {
    return 0;
  }
  // "." and ".." live in the first two records of the new directory.
  dos_now(&date, &time);
  memset(sec, 0, 512);
  memset(sec, ' ', 11);
  sec[0] = '.';
  sec[11] = FAT_ATTR_DIR;
  put16(sec + 20, (uint16_t)(c >> 16));
  put16(sec + 26, (uint16_t)(c & 0xFFFF));
  put16(sec + 22, time);
  put16(sec + 24, date);
  memset(sec + 32, ' ', 11);
  sec[32] = '.';
  sec[33] = '.';
  sec[32 + 11] = FAT_ATTR_DIR;
  put16(sec + 32 + 20, (uint16_t)(parent_ref >> 16));
  put16(sec + 32 + 26, (uint16_t)(parent_ref & 0xFFFF));
  put16(sec + 32 + 22, time);
  put16(sec + 32 + 24, date);
  if (!block_write(v->dev, cluster_lba(v, c), 1, sec)) {
    free_chain(v, c);
    return 0;
  }
  if (!create_record(v, dir_cluster, name, FAT_ATTR_DIR, c, 0, &ent)) {
    free_chain(v, c);
    return 0;
  }
  if (new_cluster) {
    *new_cluster = c;
  }
  return 1;
}

static int fat_delete_impl(fat_volume_t *v, const fat_dirent_t *ent) {
  uint32_t i;

  if (ent->attr & FAT_ATTR_DIR) {
    fat_iter_t it;
    fat_dirent_t child;
    fat_iter_open(v, ent->first_cluster, &it);
    if (fat_iter_next(&it, &child)) {
      return 0;                                 // not empty
    }
  }
  for (i = 0; i <= ent->lfn_count; i++) {
    uint32_t abs_index = ent->sfn_index - ent->lfn_count + i;
    uint32_t lba, off;
    uint8_t sec[512];
    if (!dir_locate(v, ent->dir_cluster, abs_index, 0, &lba, &off) ||
        !block_read(v->dev, lba, 1, sec)) {
      return 0;
    }
    sec[off] = 0xE5;
    if (!block_write(v->dev, lba, 1, sec)) {
      return 0;
    }
  }
  free_chain(v, ent->first_cluster);
  return 1;
}

// ---- Public entry points -----------------------------------------------------
// The driver keeps per-volume caches and is not reentrant, but the scheduler can
// preempt a task between any two instructions. Each public call therefore runs
// with preemption off, which makes every call atomic with respect to other tasks.
// (Interrupts stay enabled; only task switches are held back.)

uint64_t fat_free_bytes(fat_volume_t *v) {
  uint64_t result;

  scheduler_preempt_disable();
  result = fat_free_bytes_impl(v);
  scheduler_preempt_enable();
  return result;
}

int fat_mount(int dev, fat_volume_t *v) {
  int result;

  scheduler_preempt_disable();
  result = fat_mount_impl(dev, v);
  scheduler_preempt_enable();
  return result;
}

void fat_iter_open(fat_volume_t *v, uint32_t dir_cluster, fat_iter_t *it) {
  scheduler_preempt_disable();
  fat_iter_open_impl(v, dir_cluster, it);
  scheduler_preempt_enable();
}

int fat_iter_next(fat_iter_t *it, fat_dirent_t *out) {
  int result;

  scheduler_preempt_disable();
  result = fat_iter_next_impl(it, out);
  scheduler_preempt_enable();
  return result;
}

int fat_lookup(fat_volume_t *v, uint32_t dir_cluster, const char *name, fat_dirent_t *out) {
  int result;

  scheduler_preempt_disable();
  result = fat_lookup_impl(v, dir_cluster, name, out);
  scheduler_preempt_enable();
  return result;
}

int fat_read_file(fat_volume_t *v, const fat_dirent_t *ent, void *buf, uint32_t max, uint32_t *out_len) {
  int result;

  scheduler_preempt_disable();
  result = fat_read_file_impl(v, ent, buf, max, out_len);
  scheduler_preempt_enable();
  return result;
}

int fat_write_file(fat_volume_t *v, uint32_t dir_cluster, const char *name, const void *data, uint32_t len) {
  int result;

  scheduler_preempt_disable();
  result = fat_write_file_impl(v, dir_cluster, name, data, len);
  scheduler_preempt_enable();
  return result;
}

int fat_mkdir(fat_volume_t *v, uint32_t dir_cluster, const char *name, uint32_t *new_cluster) {
  int result;

  scheduler_preempt_disable();
  result = fat_mkdir_impl(v, dir_cluster, name, new_cluster);
  scheduler_preempt_enable();
  return result;
}

int fat_delete(fat_volume_t *v, const fat_dirent_t *ent) {
  int result;

  scheduler_preempt_disable();
  result = fat_delete_impl(v, ent);
  scheduler_preempt_enable();
  return result;
}
