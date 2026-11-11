#include "exfat.h"
#include "block.h"
#include "rtc.h"
#include "string.h"

#define EX_EOC      0xFFFFFFFFu
#define EX_EOC_MIN  0xFFFFFFF8u
#define EX_SET_MAX  19u            // most records in one entry set: File + Stream + 17 Name

static uint8_t g_scratch[512];     // data-path sector scratch (calls are serialized by the caller)

// ---- Little-endian helpers --------------------------------------------------------

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static uint32_t cbytes(const fat_volume_t *v) { return v->sectors_per_cluster * 512u; }
static uint32_t clba(const fat_volume_t *v, uint32_t c) { return v->data_lba + (c - 2u) * v->sectors_per_cluster; }
static int cluster_ok(const fat_volume_t *v, uint32_t c) { return c >= 2u && c < v->cluster_count + 2u; }

// ---- FAT ------------------------------------------------------------------------------

static int fat_load(fat_volume_t *v, uint32_t lba) {
  if (!v->ex_fat_valid || v->ex_fat_lba != lba) {
    if (!block_read(v->dev, lba, 1, v->ex_fat_buf)) {
      v->ex_fat_valid = 0;
      return 0;
    }
    v->ex_fat_lba = lba;
    v->ex_fat_valid = 1;
  }
  return 1;
}

static int ex_fat_get(fat_volume_t *v, uint32_t c, uint32_t *value) {
  uint64_t off = (uint64_t)c * 4u;

  if (!cluster_ok(v, c) || !fat_load(v, v->fat_lba + (uint32_t)(off / 512u))) {
    return 0;
  }
  *value = rd32(v->ex_fat_buf + (uint32_t)(off % 512u));
  return 1;
}

static int ex_fat_set(fat_volume_t *v, uint32_t c, uint32_t value) {
  uint64_t off = (uint64_t)c * 4u;
  uint32_t lba = v->fat_lba + (uint32_t)(off / 512u);

  if (!cluster_ok(v, c) || !fat_load(v, lba)) {
    return 0;
  }
  wr32(v->ex_fat_buf + (uint32_t)(off % 512u), value);
  return block_write(v->dev, lba, 1, v->ex_fat_buf);
}

// ---- Allocation bitmap ------------------------------------------------------------------

static int bm_load(fat_volume_t *v, uint32_t lba) {
  if (!v->ex_bm_valid || v->ex_bm_lba != lba) {
    if (!block_read(v->dev, lba, 1, v->ex_bm_buf)) {
      v->ex_bm_valid = 0;
      return 0;
    }
    v->ex_bm_lba = lba;
    v->ex_bm_valid = 1;
  }
  return 1;
}

static int bm_get(fat_volume_t *v, uint32_t c, int *bit) {
  uint32_t idx = c - 2u, byte = idx >> 3;

  if (!cluster_ok(v, c) || !bm_load(v, v->ex_bitmap_lba + byte / 512u)) {
    return 0;
  }
  *bit = (v->ex_bm_buf[byte % 512u] >> (idx & 7u)) & 1;
  return 1;
}

static int bm_set(fat_volume_t *v, uint32_t c, int value) {
  uint32_t idx = c - 2u, byte = idx >> 3, lba = v->ex_bitmap_lba + byte / 512u;

  if (!cluster_ok(v, c) || !bm_load(v, lba)) {
    return 0;
  }
  if (value) {
    v->ex_bm_buf[byte % 512u] |= (uint8_t)(1u << (idx & 7u));
  } else {
    v->ex_bm_buf[byte % 512u] &= (uint8_t)~(1u << (idx & 7u));
  }
  return block_write(v->dev, lba, 1, v->ex_bm_buf);
}

// A free cluster, marked used in the bitmap and as end-of-chain in the FAT. 0 if the volume is full.
static uint32_t alloc_cluster(fat_volume_t *v) {
  uint32_t total = v->cluster_count, i, c;

  if (v->next_free < 2u || v->next_free >= total + 2u) {
    v->next_free = 2;
  }
  for (i = 0; i < total; i++) {
    int used;

    c = v->next_free + i;
    if (c >= total + 2u) {
      c -= total;
    }
    if (!bm_get(v, c, &used)) {
      return 0;
    }
    if (!used) {
      if (!bm_set(v, c, 1) || !ex_fat_set(v, c, EX_EOC)) {
        return 0;
      }
      v->next_free = c + 1u;
      return c;
    }
  }
  return 0;
}

static int zero_cluster(fat_volume_t *v, uint32_t c) {
  uint32_t i, lba = clba(v, c);

  memset(g_scratch, 0, 512);
  for (i = 0; i < v->sectors_per_cluster; i++) {
    if (!block_write(v->dev, lba + i, 1, g_scratch)) {
      return 0;
    }
  }
  return 1;
}

// Release a file's or directory's clusters: `count` contiguous ones when nofat, else the FAT chain.
static void free_clusters(fat_volume_t *v, uint32_t first, int nofat, uint32_t count) {
  uint32_t c = first, next, guard = 0, i;

  if (!cluster_ok(v, first)) {
    return;
  }
  if (nofat) {
    for (i = 0; i < count && cluster_ok(v, first + i); i++) {
      bm_set(v, first + i, 0);
    }
  } else {
    while (cluster_ok(v, c) && guard++ <= v->cluster_count) {
      if (!ex_fat_get(v, c, &next)) {
        break;
      }
      bm_set(v, c, 0);
      ex_fat_set(v, c, 0);
      if (next >= EX_EOC_MIN) {
        break;
      }
      c = next;
    }
  }
  if (first < v->next_free) {
    v->next_free = first;
  }
}

// ---- Directory geometry ------------------------------------------------------------------
// A directory is named by its first cluster. Whether its clusters are chained in the FAT or
// contiguous, and how long it is, is only in the entry that lists it; remember that when a
// listing goes past it (and where the entry is, so growing the directory can update it).

typedef struct {
  uint32_t first;
  uint8_t nofat;
  uint32_t nclusters;          // for nofat directories
} exdir_t;

static fat_exdir_t *dir_cache_find(fat_volume_t *v, uint32_t cluster) {
  uint32_t i;

  for (i = 0; i < FAT_EXDIR_CACHE; i++) {
    if (v->ex_dirs[i].cluster == cluster) {
      return &v->ex_dirs[i];
    }
  }
  return 0;
}

static void dir_remember(fat_volume_t *v, uint32_t cluster, uint32_t parent, uint32_t abs, uint32_t length, int nofat) {
  fat_exdir_t *e = dir_cache_find(v, cluster);

  if (!e) {
    e = &v->ex_dirs[v->ex_dir_next++ % FAT_EXDIR_CACHE];
  }
  e->cluster = cluster;
  e->parent = parent;
  e->abs = abs;
  e->length = length;
  e->nofat = (uint8_t)(nofat != 0);
}

static void dir_forget(fat_volume_t *v, uint32_t cluster) {
  fat_exdir_t *e = dir_cache_find(v, cluster);

  if (e) {
    memset(e, 0, sizeof(*e));
  }
}

static void dir_info(fat_volume_t *v, uint32_t cluster, exdir_t *d) {
  fat_exdir_t *e = cluster == v->root_cluster ? 0 : dir_cache_find(v, cluster);

  d->first = cluster;
  d->nofat = e ? e->nofat : 0;
  d->nclusters = e ? (e->length + cbytes(v) - 1u) / cbytes(v) : 0;
}

static int dir_cluster_at(fat_volume_t *v, const exdir_t *d, uint32_t idx, uint32_t *out) {
  uint32_t c = d->first, i, next;

  if (d->nofat) {
    if (idx >= d->nclusters) {
      return 0;
    }
    *out = d->first + idx;
    return 1;
  }
  for (i = 0; i < idx; i++) {
    if (!ex_fat_get(v, c, &next) || next >= EX_EOC_MIN || !cluster_ok(v, next)) {
      return 0;
    }
    c = next;
  }
  *out = c;
  return cluster_ok(v, c);
}

// Read or write `nrec` 32-byte records starting at record index `abs` of a directory.
static int dir_rw(fat_volume_t *v, uint32_t dir_cluster, uint32_t abs, uint32_t nrec, uint8_t *buf, int write) {
  exdir_t d;
  uint32_t per = v->sectors_per_cluster * 16u, i, cluster = 0, cur_ci = 0xFFFFFFFFu, sec_lba = 0;
  uint8_t sec[512];
  int have = 0, dirty = 0;

  dir_info(v, dir_cluster, &d);
  for (i = 0; i < nrec; i++) {
    uint32_t idx = abs + i, ci = idx / per, in = idx % per, lba, off;

    if (ci != cur_ci) {
      if (!dir_cluster_at(v, &d, ci, &cluster)) {
        return 0;
      }
      cur_ci = ci;
    }
    lba = clba(v, cluster) + (in * 32u) / 512u;
    off = (in * 32u) % 512u;
    if (!have || lba != sec_lba) {
      if (dirty) {
        if (!block_write(v->dev, sec_lba, 1, sec)) {
          return 0;
        }
        dirty = 0;
      }
      if (!block_read(v->dev, lba, 1, sec)) {
        return 0;
      }
      sec_lba = lba;
      have = 1;
    }
    if (write) {
      memcpy(sec + off, buf + (size_t)i * 32u, 32);
      dirty = 1;
    } else {
      memcpy(buf + (size_t)i * 32u, sec + off, 32);
    }
  }
  if (dirty && !block_write(v->dev, sec_lba, 1, sec)) {
    return 0;
  }
  return 1;
}

// ---- Directory iteration ---------------------------------------------------------------------

void exfat_iter_open(fat_volume_t *v, uint32_t dir_cluster, fat_iter_t *it) {
  exdir_t d;

  dir_info(v, dir_cluster, &d);
  it->v = v;
  it->dir_cluster = dir_cluster;
  it->cluster = dir_cluster;
  it->index = 0;
  it->abs = 0;
  it->sec_valid = 0;
  it->fixed_root = 0;
  it->nofat = d.nofat;
  it->clusters_left = d.nofat && d.nclusters > 0 ? d.nclusters - 1u : 0;
  it->done = !cluster_ok(v, dir_cluster) || (d.nofat && d.nclusters == 0);
}

// Next raw 32-byte record (free ones and the end marker included). 0 at the end of the directory.
static int iter_raw(fat_iter_t *it, uint8_t out[32], uint32_t *abs_index) {
  fat_volume_t *v = it->v;
  uint32_t per = v->sectors_per_cluster * 16u, lba, off, next;

  if (it->done) {
    return 0;
  }
  lba = clba(v, it->cluster) + (it->index * 32u) / 512u;
  off = (it->index * 32u) % 512u;
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
  if (it->index >= per) {
    if (it->nofat) {
      if (it->clusters_left == 0) {
        it->done = 1;
      } else {
        it->clusters_left--;
        it->cluster++;
        it->index = 0;
      }
    } else if (!ex_fat_get(v, it->cluster, &next) || next >= EX_EOC_MIN || !cluster_ok(v, next)) {
      it->done = 1;
    } else {
      it->cluster = next;
      it->index = 0;
    }
  }
  return 1;
}

static uint16_t set_checksum(const uint8_t *set, uint32_t nrec) {
  uint16_t cs = 0;
  uint32_t i;

  for (i = 0; i < nrec * 32u; i++) {
    if (i == 2 || i == 3) {
      continue;
    }
    cs = (uint16_t)(((cs << 15) | (cs >> 1)) + set[i]);
  }
  return cs;
}

static uint16_t name_hash(const char *name, uint32_t n) {
  uint16_t h = 0;
  uint32_t i;

  for (i = 0; i < n; i++) {
    uint8_t c = (uint8_t)char_upper(name[i]);

    h = (uint16_t)(((h << 15) | (h >> 1)) + c);          // low byte of the UTF-16 unit
    h = (uint16_t)(((h << 15) | (h >> 1)) + 0);          // high byte
  }
  return h;
}

int exfat_iter_next(fat_iter_t *it, fat_dirent_t *out) {
  uint8_t e[32], set[EX_SET_MAX * 32u];
  uint32_t abs_index, dummy, count, k, nlen, i, first, size, valid;

  while (iter_raw(it, e, &abs_index)) {
    if (e[0] == 0x00) {
      it->done = 1;
      return 0;
    }
    if (e[0] != 0x85) {
      continue;                                            // free, bitmap, upcase, label, ...
    }
    count = e[1];
    if (count < 2 || count > EX_SET_MAX - 1u) {
      continue;
    }
    memcpy(set, e, 32);
    for (k = 0; k < count; k++) {
      if (!iter_raw(it, set + (size_t)(k + 1u) * 32u, &dummy)) {
        return 0;
      }
    }
    if (set_checksum(set, count + 1u) != rd16(set + 2) || set[32] != 0xC0) {
      continue;                                            // damaged or not a file entry set
    }
    nlen = set[32 + 3];
    if (nlen == 0 || count - 1u < (nlen + 14u) / 15u) {
      continue;
    }
    memset(out, 0, sizeof(*out));
    for (i = 0; i < nlen && i < FAT_NAME_MAX - 1u; i++) {
      uint16_t u = rd16(set + 32u * (2u + i / 15u) + 2u + 2u * (i % 15u));

      out->name[i] = (u >= 0x20 && u < 0x7F) ? (char)u : '?';
    }
    out->attr = (uint8_t)(rd16(set + 4) & 0x37u);
    out->mtime = rd16(set + 12);
    out->mdate = rd16(set + 14);
    out->exflags = set[32 + 1];
    first = rd32(set + 32 + 20);
    size = rd32(set + 32 + 24);
    if (rd32(set + 32 + 28) != 0) {
      size = 0xFFFFFFFFu;                                  // over 4 GiB: not representable here
    }
    valid = rd32(set + 32 + 8);
    if (rd32(set + 32 + 12) != 0 || valid > size) {
      valid = size;
    }
    out->first_cluster = first;
    out->size = size;
    out->valid = valid;
    out->dir_cluster = it->dir_cluster;
    out->sfn_index = abs_index;
    out->lfn_count = count;
    if (out->attr & FAT_ATTR_DIR) {
      dir_remember(it->v, first, it->dir_cluster, abs_index, size, (out->exflags & 2u) != 0);
    }
    return 1;
  }
  return 0;
}

int exfat_lookup(fat_volume_t *v, uint32_t dir_cluster, const char *name, fat_dirent_t *out) {
  fat_iter_t it;

  exfat_iter_open(v, dir_cluster, &it);
  while (exfat_iter_next(&it, out)) {
    if (str_eq_nocase(out->name, name)) {
      return 1;
    }
  }
  return 0;
}

// ---- Entry sets (create, update, grow) ------------------------------------------------------------

static uint32_t now32(void) {
  int y = 2026, mo = 1, d = 1, h = 0, mi = 0, s = 0;

  rtc_read_date(&y, &mo, &d);
  rtc_read_time(&h, &mi, &s);
  if (y < 1980) {
    y = 1980;
  }
  return ((uint32_t)(((y - 1980) << 9) | (mo << 5) | d) << 16) | (uint32_t)((h << 11) | (mi << 5) | (s / 2));
}

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

    if ((uint8_t)c < 0x20 || (uint8_t)c >= 0x7F || c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
        c == '"' || c == '<' || c == '>' || c == '|') {
      return 0;
    }
  }
  return 1;
}

// Read the whole entry set that starts at record `abs`. Returns the number of records or 0.
static uint32_t set_read(fat_volume_t *v, uint32_t dir_cluster, uint32_t abs, uint8_t *buf) {
  uint32_t n;

  if (!dir_rw(v, dir_cluster, abs, 1, buf, 0) || buf[0] != 0x85) {
    return 0;
  }
  n = (uint32_t)buf[1] + 1u;
  if (n < 3 || n > EX_SET_MAX || !dir_rw(v, dir_cluster, abs + 1u, n - 1u, buf + 32, 0)) {
    return 0;
  }
  return n;
}

// Rewrite the stream record (size, first cluster, flags) and the checksum of an entry set.
static int set_update(fat_volume_t *v, uint32_t dir_cluster, uint32_t abs, uint32_t first, uint32_t size, int nofat,
                      int touch) {
  uint8_t set[EX_SET_MAX * 32u];
  uint32_t n = set_read(v, dir_cluster, abs, set), ts;

  if (n == 0) {
    return 0;
  }
  ts = now32();
  set[32 + 1] = (uint8_t)(0x01u | (nofat ? 0x02u : 0u));
  wr32(set + 32 + 8, size);
  wr32(set + 32 + 12, 0);
  wr32(set + 32 + 20, first);
  wr32(set + 32 + 24, size);
  wr32(set + 32 + 28, 0);
  if (touch) {
    wr32(set + 12, ts);
    wr32(set + 16, ts);
  }
  wr16(set + 2, set_checksum(set, n));
  return dir_rw(v, dir_cluster, abs, 2, set, 1);
}

static int update_entry(fat_volume_t *v, const fat_dirent_t *ent) {
  return set_update(v, ent->dir_cluster, ent->sfn_index, ent->first_cluster, ent->size, (ent->exflags & 2u) != 0, 1);
}

// Give a directory one more cluster (and record the new length in its own entry).
static int dir_grow(fat_volume_t *v, uint32_t dir_cluster) {
  exdir_t d;
  fat_exdir_t *ce = dir_cluster == v->root_cluster ? 0 : dir_cache_find(v, dir_cluster);
  uint32_t last, count = 1, next, c, i, guard = 0;

  if (dir_cluster != v->root_cluster && !ce) {
    return 0;                                              // unknown directory: cannot update its entry
  }
  dir_info(v, dir_cluster, &d);
  if (d.nofat) {                                           // make the chain explicit first
    if (d.nclusters == 0) {
      return 0;
    }
    for (i = 0; i + 1u < d.nclusters; i++) {
      if (!ex_fat_set(v, d.first + i, d.first + i + 1u)) {
        return 0;
      }
    }
    last = d.first + d.nclusters - 1u;
    if (!ex_fat_set(v, last, EX_EOC)) {
      return 0;
    }
    count = d.nclusters;
  } else {
    last = d.first;
    while (guard++ <= v->cluster_count) {
      if (!ex_fat_get(v, last, &next) || next >= EX_EOC_MIN || !cluster_ok(v, next)) {
        break;
      }
      last = next;
      count++;
    }
  }
  c = alloc_cluster(v);
  if (c == 0 || !zero_cluster(v, c) || !ex_fat_set(v, last, c)) {
    if (c) {
      bm_set(v, c, 0);
      ex_fat_set(v, c, 0);
    }
    return 0;
  }
  if (ce) {
    uint32_t length = (count + 1u) * cbytes(v);

    if (!set_update(v, ce->parent, ce->abs, ce->cluster, length, 0, 0)) {
      return 0;
    }
    ce->length = length;
    ce->nofat = 0;
  }
  return 1;
}

// Write a new entry set for `name` into the directory and describe it in *out.
static int create_entry(fat_volume_t *v, uint32_t dir_cluster, const char *name, uint8_t attr, uint32_t first,
                        uint32_t size, fat_dirent_t *out) {
  uint8_t set[EX_SET_MAX * 32u], rec[32];
  uint32_t n = (uint32_t)str_len(name), names = (n + 14u) / 15u, total = 2u + names;
  uint32_t abs, run_start = 0, run_len = 0, scanned = 0, i, ts, per = v->sectors_per_cluster * 16u;
  int found = 0;
  fat_iter_t it;

  exfat_iter_open(v, dir_cluster, &it);
  while (iter_raw(&it, rec, &abs)) {
    scanned = abs + 1u;
    if (!(rec[0] & 0x80u)) {                                // free or deleted (or the end marker)
      if (run_len == 0) {
        run_start = abs;
      }
      if (++run_len >= total) {
        found = 1;
        break;
      }
    } else {
      run_len = 0;
    }
  }
  if (!found) {
    if (!dir_grow(v, dir_cluster)) {
      return 0;
    }
    if (run_len == 0) {
      run_start = scanned;
    }
    run_len += per;
  }

  ts = now32();
  memset(set, 0, sizeof(set));
  set[0] = 0x85;
  set[1] = (uint8_t)(total - 1u);
  wr16(set + 4, attr);
  wr32(set + 8, ts);
  wr32(set + 12, ts);
  wr32(set + 16, ts);
  set[32] = 0xC0;
  set[32 + 1] = 0x01;                                       // allocation possible, FAT chain
  set[32 + 3] = (uint8_t)n;
  wr16(set + 32 + 4, name_hash(name, n));
  wr32(set + 32 + 8, size);
  wr32(set + 32 + 20, first);
  wr32(set + 32 + 24, size);
  for (i = 0; i < names; i++) {
    uint8_t *r = set + 32u * (2u + i);
    uint32_t k;

    r[0] = 0xC1;
    for (k = 0; k < 15u && i * 15u + k < n; k++) {
      wr16(r + 2u + 2u * k, (uint16_t)(uint8_t)name[i * 15u + k]);
    }
  }
  wr16(set + 2, set_checksum(set, total));
  if (!dir_rw(v, dir_cluster, run_start + 1u, total - 1u, set + 32, 1) ||    // secondaries first, then the primary
      !dir_rw(v, dir_cluster, run_start, 1, set, 1)) {
    return 0;
  }

  memset(out, 0, sizeof(*out));
  str_copy(out->name, name, FAT_NAME_MAX);
  out->attr = attr;
  out->first_cluster = first;
  out->size = size;
  out->valid = size;
  out->mdate = (uint16_t)(ts >> 16);
  out->mtime = (uint16_t)ts;
  out->exflags = 0x01;
  out->dir_cluster = dir_cluster;
  out->sfn_index = run_start;
  out->lfn_count = total - 1u;
  return 1;
}

// ---- Mounting ------------------------------------------------------------------------------------

int exfat_probe(const uint8_t *s) {
  return memcmp(s + 3, "EXFAT   ", 8) == 0;
}

int exfat_mount(int dev, fat_volume_t *v, const uint8_t *s) {
  uint32_t fat_off = rd32(s + 80), fat_len = rd32(s + 84), heap_off = rd32(s + 88), count = rd32(s + 92);
  uint32_t root = rd32(s + 96), vol_hi = rd32(s + 76), vol_lo = rd32(s + 72);
  uint32_t bshift = s[108], sshift = s[109], nfats = s[110], i;
  uint64_t vol = ((uint64_t)vol_hi << 32) | vol_lo;
  fat_iter_t it;
  uint8_t rec[32];
  uint32_t abs, bitmap = 0, bitmap_bytes = 0;

  if (s[510] != 0x55 || s[511] != 0xAA) {
    return 0;
  }
  for (i = 11; i < 64; i++) {                               // MustBeZero
    if (s[i] != 0) {
      return 0;
    }
  }
  if (bshift != 9 || sshift > 16 || nfats < 1 || nfats > 2 || count == 0 || fat_off == 0 || heap_off == 0 ||
      root < 2 || root >= count + 2u) {
    return 0;
  }
  if ((uint64_t)heap_off + ((uint64_t)count << sshift) > vol || fat_off + (uint64_t)fat_len > heap_off ||
      (uint64_t)fat_len * 128u < (uint64_t)count + 2u) {
    return 0;
  }

  memset(v, 0, sizeof(*v));
  v->exfat = 1;
  v->dev = dev;
  v->sectors_per_cluster = 1u << sshift;
  v->reserved_sectors = fat_off;
  v->num_fats = nfats;
  v->fat_size = fat_len;
  v->fat_bits = 32;
  v->eoc_min = EX_EOC_MIN;
  v->eoc = EX_EOC;
  v->root_cluster = root;
  v->total_sectors = vol > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)vol;
  v->fat_lba = fat_off;
  v->data_lba = heap_off;
  v->cluster_count = count;
  v->next_free = 2;

  exfat_iter_open(v, root, &it);                            // the bitmap and the label are in the root directory
  while (iter_raw(&it, rec, &abs)) {
    if (rec[0] == 0x00) {
      break;
    }
    if (rec[0] == 0x81 && (rec[1] & 1u) == 0 && bitmap == 0) {
      bitmap = rd32(rec + 20);
      bitmap_bytes = rd32(rec + 24);
    } else if (rec[0] == 0x83) {
      uint32_t n = rec[1] > 11 ? 11 : rec[1], k;

      for (k = 0; k < n; k++) {
        uint16_t u = rd16(rec + 2 + 2 * k);

        v->label[k] = (u >= 0x20 && u < 0x7F) ? (char)u : '?';
      }
      v->label[n] = 0;
    }
  }
  if (!cluster_ok(v, bitmap) || bitmap_bytes < (count + 7u) / 8u) {
    return 0;
  }
  v->ex_bitmap_lba = clba(v, bitmap);                       // the bitmap is contiguous (as every formatter makes it)
  v->ex_bitmap_bytes = bitmap_bytes;
  return 1;
}

uint64_t exfat_free_bytes(fat_volume_t *v) {
  uint32_t c, free_clusters = 0;
  uint32_t lba = 0;
  int have = 0;

  for (c = 2; c < v->cluster_count + 2u; c++) {
    uint32_t idx = c - 2u, byte = idx >> 3, want = v->ex_bitmap_lba + byte / 512u;

    if (!have || want != lba) {
      if (!block_read(v->dev, want, 1, g_scratch)) {
        break;
      }
      lba = want;
      have = 1;
    }
    if (!((g_scratch[byte % 512u] >> (idx & 7u)) & 1u)) {
      free_clusters++;
    }
  }
  return (uint64_t)free_clusters * cbytes(v);
}

// ---- Files ------------------------------------------------------------------------------------

static uint32_t file_clusters(const fat_volume_t *v, uint32_t size) {
  return (size + cbytes(v) - 1u) / cbytes(v);
}

// Put the cursor on cluster number `idx` of the file.
static int file_seek(fat_file_t *f, uint32_t idx, uint32_t *cluster) {
  fat_volume_t *v = f->v;
  uint32_t c, i, next;

  if (!cluster_ok(v, f->ent.first_cluster)) {
    return 0;
  }
  if (f->ent.exflags & 2u) {                                // contiguous
    c = f->ent.first_cluster + idx;
    if (!cluster_ok(v, c)) {
      return 0;
    }
    f->cur_cluster = c;
    f->cur_index = idx;
    *cluster = c;
    return 1;
  }
  if (f->cur_cluster >= 2 && f->cur_index <= idx) {
    c = f->cur_cluster;
    i = f->cur_index;
  } else {
    c = f->ent.first_cluster;
    i = 0;
  }
  while (i < idx) {
    if (!ex_fat_get(v, c, &next) || next >= EX_EOC_MIN || !cluster_ok(v, next)) {
      return 0;
    }
    c = next;
    ++i;
  }
  f->cur_cluster = c;
  f->cur_index = i;
  *cluster = c;
  return 1;
}

int exfat_file_open(fat_volume_t *v, const fat_dirent_t *ent, fat_file_t *f) {
  if ((ent->attr & FAT_ATTR_DIR) != 0) {
    return 0;
  }
  f->v = v;
  f->ent = *ent;
  f->cur_index = 0;
  f->cur_cluster = 0;
  return 1;
}

int exfat_file_create(fat_volume_t *v, uint32_t dir_cluster, const char *name, fat_file_t *f) {
  fat_dirent_t ent;

  if (!name_ok(name)) {
    return 0;
  }
  if (exfat_lookup(v, dir_cluster, name, &ent)) {
    if (ent.attr & (FAT_ATTR_DIR | FAT_ATTR_READONLY)) {
      return 0;
    }
    free_clusters(v, ent.first_cluster, (ent.exflags & 2u) != 0, file_clusters(v, ent.size));
    ent.first_cluster = 0;
    ent.size = 0;
    ent.valid = 0;
    ent.exflags = 0x01;
    if (!update_entry(v, &ent)) {
      return 0;
    }
  } else if (!create_entry(v, dir_cluster, name, FAT_ATTR_ARCHIVE, 0, 0, &ent)) {
    return 0;
  }
  return exfat_file_open(v, &ent, f);
}

int exfat_file_read(fat_file_t *f, uint32_t offset, void *buf, uint32_t len, uint32_t *out_len) {
  fat_volume_t *v = f->v;
  uint32_t cb = cbytes(v), pos = offset, total, real = 0;
  uint8_t *dst = (uint8_t *)buf;

  *out_len = 0;
  if (offset >= f->ent.size) {
    return 1;
  }
  if (len > f->ent.size - offset) {
    len = f->ent.size - offset;
  }
  total = len;
  if (offset < f->ent.valid) {                              // bytes past the valid data length read as zeros
    real = f->ent.valid - offset < len ? f->ent.valid - offset : len;
  }
  len = real;
  while (len > 0) {
    uint32_t c, off = pos % cb, chunk = cb - off, done = 0, lba, sec_off, full;

    if (chunk > len) {
      chunk = len;
    }
    if (!file_seek(f, pos / cb, &c)) {
      return *out_len > 0;
    }
    lba = clba(v, c) + off / 512u;
    sec_off = off % 512u;
    if (sec_off != 0) {                                     // the first, partial sector
      uint32_t take = 512u - sec_off < chunk ? 512u - sec_off : chunk;

      if (!block_read(v->dev, lba, 1, g_scratch)) {
        return *out_len > 0;
      }
      memcpy(dst, g_scratch + sec_off, take);
      dst += take;
      done += take;
      ++lba;
    }
    full = (chunk - done) / 512u;                           // whole sectors in one go
    if (full > 0) {
      if (!block_read(v->dev, lba, full, dst)) {
        return *out_len > 0;
      }
      dst += (size_t)full * 512u;
      done += full * 512u;
      lba += full;
    }
    if (done < chunk) {                                     // the last, partial sector
      if (!block_read(v->dev, lba, 1, g_scratch)) {
        return *out_len > 0;
      }
      memcpy(dst, g_scratch, chunk - done);
      dst += chunk - done;
    }
    pos += chunk;
    len -= chunk;
    *out_len += chunk;
  }
  if (total > real) {
    memset(dst, 0, total - real);
    *out_len += total - real;
  }
  return 1;
}

// A contiguous file is about to grow: write its chain into the FAT and drop the contiguous flag.
static int file_make_chain(fat_file_t *f) {
  fat_volume_t *v = f->v;
  uint32_t n = file_clusters(v, f->ent.size), i;

  for (i = 0; i + 1u < n; i++) {
    if (!ex_fat_set(v, f->ent.first_cluster + i, f->ent.first_cluster + i + 1u)) {
      return 0;
    }
  }
  if (n > 0 && !ex_fat_set(v, f->ent.first_cluster + n - 1u, EX_EOC)) {
    return 0;
  }
  f->ent.exflags &= (uint8_t)~2u;
  f->cur_cluster = 0;
  f->cur_index = 0;
  return 1;
}

int exfat_file_write(fat_file_t *f, uint32_t offset, const void *data, uint32_t len, uint32_t *written) {
  fat_volume_t *v = f->v;
  uint32_t cb = cbytes(v), pos = offset;
  const uint8_t *src = (const uint8_t *)data;
  uint32_t old_first = f->ent.first_cluster, old_size = f->ent.size, old_flags = f->ent.exflags;
  uint32_t chain = file_clusters(v, f->ent.size);
  int stopped = 0;

  *written = 0;
  if (offset > f->ent.size || f->ent.valid < f->ent.size) {
    return 0;
  }
  if (len > 0xFFFFFFFFu - offset) {
    len = 0xFFFFFFFFu - offset;
  }
  while (len > 0 && !stopped) {
    uint32_t idx = pos / cb, off = pos % cb, chunk = cb - off, done = 0, c, lba;
    int fresh = 0;

    if (chunk > len) {
      chunk = len;
    }
    if (idx < chain) {
      if (!file_seek(f, idx, &c)) {
        break;
      }
    } else {                                                // grow by one cluster
      uint32_t prev = 0;

      if ((f->ent.exflags & 2u) && chain > 0 && !file_make_chain(f)) {
        break;
      }
      if (idx > 0 && !file_seek(f, idx - 1u, &prev)) {
        break;
      }
      c = alloc_cluster(v);
      if (c == 0) {
        break;                                              // volume full
      }
      if (idx == 0) {
        f->ent.first_cluster = c;
        f->ent.exflags &= (uint8_t)~2u;
      } else if (!ex_fat_set(v, prev, c)) {
        free_clusters(v, c, 0, 1);
        break;
      }
      f->cur_cluster = c;
      f->cur_index = idx;
      chain = idx + 1u;
      fresh = 1;
    }
    lba = clba(v, c) + off / 512u;
    while (done < chunk) {
      uint32_t sec_off = (off + done) % 512u, left = chunk - done;

      if (sec_off == 0 && left >= 512u) {                   // whole sectors, written together
        uint32_t full = left / 512u;

        if (!block_write(v->dev, lba, full, src)) {
          stopped = 1;
          break;
        }
        src += (size_t)full * 512u;
        done += full * 512u;
        lba += full;
      } else {                                              // part of a sector: keep the rest
        uint32_t take = 512u - sec_off < left ? 512u - sec_off : left;
        uint32_t sec_start = idx * cb + ((off + done) / 512u) * 512u;

        if (!fresh && sec_start < f->ent.size) {
          if (!block_read(v->dev, lba, 1, g_scratch)) {
            stopped = 1;
            break;
          }
        } else {
          memset(g_scratch, 0, 512);
        }
        memcpy(g_scratch + sec_off, src, take);
        if (!block_write(v->dev, lba, 1, g_scratch)) {
          stopped = 1;
          break;
        }
        src += take;
        done += take;
        ++lba;
      }
    }
    pos += done;
    len -= done;
    *written += done;
    if (pos > f->ent.size) {
      f->ent.size = pos;
    }
    if (done < chunk) {
      break;
    }
  }
  f->ent.valid = f->ent.size;
  if (f->ent.size != old_size || f->ent.first_cluster != old_first || f->ent.exflags != old_flags) {
    if (!update_entry(v, &f->ent)) {
      return 0;
    }
  }
  return *written > 0 || len == 0;
}

int exfat_read_file(fat_volume_t *v, const fat_dirent_t *ent, void *buf, uint32_t max, uint32_t *out_len) {
  fat_file_t f;

  if (!exfat_file_open(v, ent, &f)) {
    return 0;
  }
  return exfat_file_read(&f, 0, buf, max, out_len);
}

int exfat_write_file(fat_volume_t *v, uint32_t dir_cluster, const char *name, const void *data, uint32_t len) {
  fat_file_t f;
  uint32_t written = 0;

  if (!exfat_file_create(v, dir_cluster, name, &f)) {
    return 0;
  }
  if (len > 0 && (!exfat_file_write(&f, 0, data, len, &written) || written < len)) {
    free_clusters(v, f.ent.first_cluster, (f.ent.exflags & 2u) != 0, file_clusters(v, f.ent.size));   // volume full: roll back
    f.ent.first_cluster = 0;
    f.ent.size = 0;
    f.ent.valid = 0;
    f.ent.exflags = 0x01;
    update_entry(v, &f.ent);
    return 0;
  }
  return 1;
}

int exfat_mkdir(fat_volume_t *v, uint32_t dir_cluster, const char *name, uint32_t *new_cluster) {
  fat_dirent_t existing, ent;
  uint32_t c;

  if (!name_ok(name) || exfat_lookup(v, dir_cluster, name, &existing)) {
    return 0;
  }
  c = alloc_cluster(v);
  if (c == 0 || !zero_cluster(v, c)) {
    if (c) {
      free_clusters(v, c, 0, 1);
    }
    return 0;
  }
  if (!create_entry(v, dir_cluster, name, FAT_ATTR_DIR, c, cbytes(v), &ent)) {
    free_clusters(v, c, 0, 1);
    return 0;
  }
  dir_remember(v, c, dir_cluster, ent.sfn_index, cbytes(v), 0);
  if (new_cluster) {
    *new_cluster = c;
  }
  return 1;
}

int exfat_delete(fat_volume_t *v, const fat_dirent_t *ent) {
  uint8_t set[EX_SET_MAX * 32u];
  uint32_t n, i;
  int nofat = (ent->exflags & 2u) != 0;

  if (ent->attr & FAT_ATTR_DIR) {
    fat_iter_t it;
    fat_dirent_t child;

    exfat_iter_open(v, ent->first_cluster, &it);
    if (exfat_iter_next(&it, &child)) {
      return 0;                                             // not empty
    }
  }
  n = set_read(v, ent->dir_cluster, ent->sfn_index, set);
  if (n == 0) {
    return 0;
  }
  for (i = 0; i < n; i++) {
    set[i * 32u] &= 0x7Fu;                                  // clear "in use"
  }
  if (!dir_rw(v, ent->dir_cluster, ent->sfn_index, n, set, 1)) {
    return 0;
  }
  free_clusters(v, ent->first_cluster, nofat, file_clusters(v, ent->size));
  if (ent->attr & FAT_ATTR_DIR) {
    dir_forget(v, ent->first_cluster);
  }
  return 1;
}
