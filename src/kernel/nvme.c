#include "nvme.h"
#include "paging.h"
#include "pci.h"
#include "pmm.h"
#include "string.h"
#include "timer.h"

// ---- Controller registers --------------------------------------------------
#define REG_CAP    0x00      // 64-bit
#define REG_INTMS  0x0C
#define REG_CC     0x14
#define REG_CSTS   0x1C
#define REG_AQA    0x24
#define REG_ASQ    0x28      // 64-bit
#define REG_ACQ    0x30      // 64-bit
#define REG_DOORBELLS 0x1000

#define CC_EN      (1u << 0)
#define CSTS_RDY   (1u << 0)
#define CSTS_CFS   (1u << 1)

// Admin opcodes
#define ADMIN_CREATE_SQ   0x01
#define ADMIN_CREATE_CQ   0x05
#define ADMIN_IDENTIFY    0x06
#define ADMIN_SET_FEATURES 0x09
// I/O opcodes
#define IO_WRITE 0x01
#define IO_READ  0x02

#define ADMIN_QUEUE_ENTRIES 16
#define IO_QUEUE_ENTRIES    32
#define SQ_ENTRY_BYTES      64
#define CQ_ENTRY_BYTES      16

#define BOUNCE_PAGES    16
#define BOUNCE_BYTES    (BOUNCE_PAGES * 4096u)
#define BOUNCE_SECTORS  (BOUNCE_BYTES / 512u)

#define TIMEOUT_MS 5000

typedef struct {
  uint32_t *sq;              // submission queue memory (physical == virtual)
  uint32_t *cq;              // completion queue memory
  uint32_t entries;
  uint32_t sq_tail;
  uint32_t cq_head;
  uint32_t phase;
  uint32_t qid;
} queue_t;

typedef struct {
  uintptr_t base;
  uint32_t stride;           // doorbell stride in bytes
  queue_t admin;
  queue_t io;
  uint16_t next_cid;
  uint32_t max_chunk_sectors;
  uint8_t *bounce;
  uint64_t bounce_phys;
  uint64_t *prp_list;        // one page
  uint64_t prp_list_phys;
  int ready;
} controller_t;

static controller_t g_ctrl[NVME_MAX_DISKS];
static int g_ctrl_count = 0;
static nvme_disk_t g_disks[NVME_MAX_DISKS];
static int g_disk_count = 0;

static inline uint32_t rd32(const controller_t *c, uint32_t off) {
  return *(volatile uint32_t *)(c->base + off);
}

static inline void wr32(const controller_t *c, uint32_t off, uint32_t value) {
  *(volatile uint32_t *)(c->base + off) = value;
}

static inline void wr64(const controller_t *c, uint32_t off, uint64_t value) {
  wr32(c, off, (uint32_t)(value & 0xFFFFFFFFu));
  wr32(c, off + 4, (uint32_t)(value >> 32));
}

static uint64_t rd64(const controller_t *c, uint32_t off) {
  return (uint64_t)rd32(c, off) | ((uint64_t)rd32(c, off + 4) << 32);
}

static int wait_ready(const controller_t *c, int want_ready, uint32_t timeout_ms) {
  uint64_t start = timer_get_ms();
  for (;;) {
    uint32_t csts = rd32(c, REG_CSTS);
    if (((csts & CSTS_RDY) != 0) == (want_ready != 0)) {
      return 1;
    }
    if (csts & CSTS_CFS) {
      return 0;
    }
    if (timer_get_ms() - start > timeout_ms) {
      return 0;
    }
  }
}

static uint64_t alloc_mapped(uint32_t pages) {
  uint64_t phys = pmm_alloc_pages(pages);
  if (phys == 0 || !paging_map_range(phys, (uint64_t)pages * 4096u)) {
    return 0;
  }
  memset((void *)(uintptr_t)phys, 0, (size_t)pages * 4096u);
  return phys;
}

static void ring_sq(const controller_t *c, const queue_t *q) {
  wr32(c, REG_DOORBELLS + (2u * q->qid) * c->stride, q->sq_tail);
}

static void ring_cq(const controller_t *c, const queue_t *q) {
  wr32(c, REG_DOORBELLS + (2u * q->qid + 1u) * c->stride, q->cq_head);
}

// Submit one command (16 dwords) and wait for its completion. Returns the
// status code field (0 = success), or 0xFFFF on timeout.
static uint32_t submit(controller_t *c, queue_t *q, const uint32_t cmd[16]) {
  uint32_t *slot = q->sq + q->sq_tail * (SQ_ENTRY_BYTES / 4);
  uint32_t cid = c->next_cid++;
  uint64_t start;
  int i;

  for (i = 0; i < 16; i++) {
    slot[i] = cmd[i];
  }
  slot[0] = (slot[0] & 0x0000FFFFu) | (cid << 16);
  q->sq_tail = (q->sq_tail + 1) % q->entries;
  __asm__ volatile("" : : : "memory");
  ring_sq(c, q);

  start = timer_get_ms();
  for (;;) {
    volatile uint32_t *entry = q->cq + q->cq_head * (CQ_ENTRY_BYTES / 4);
    uint32_t dw3 = entry[3];
    if (((dw3 >> 16) & 1u) == q->phase) {
      uint32_t status = (dw3 >> 17) & 0x7FFFu;
      q->cq_head++;
      if (q->cq_head == q->entries) {
        q->cq_head = 0;
        q->phase ^= 1u;
      }
      ring_cq(c, q);
      return status;
    }
    if (timer_get_ms() - start > TIMEOUT_MS) {
      return 0xFFFFu;
    }
  }
}

static uint32_t admin_cmd(controller_t *c, uint8_t opcode, uint32_t nsid, uint64_t prp1,
                          uint32_t cdw10, uint32_t cdw11) {
  uint32_t cmd[16];
  memset(cmd, 0, sizeof(cmd));
  cmd[0] = opcode;
  cmd[1] = nsid;
  cmd[6] = (uint32_t)(prp1 & 0xFFFFFFFFu);
  cmd[7] = (uint32_t)(prp1 >> 32);
  cmd[10] = cdw10;
  cmd[11] = cdw11;
  return submit(c, &c->admin, cmd);
}

static void trim_text(char *s) {
  int n = (int)str_len(s);
  while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == 0)) {
    s[--n] = 0;
  }
}

// Bring one controller up and register its first 512-byte-block namespace.
static int init_controller(int ctrl_index, const pci_dev_t *dev) {
  controller_t *c = &g_ctrl[ctrl_index];
  uint32_t bar0, bar1;
  uint64_t bar, cap, admin_sq, admin_cq, io_sq, io_cq;
  uint32_t dstrd, mqes, timeout_ms, mpsmin;
  uint32_t nsid = 1, lbads, lbaf_index;
  uint8_t *id;
  uint32_t status;
  nvme_disk_t *disk;
  int i;

  memset(c, 0, sizeof(*c));
  pci_enable_device(dev);
  bar0 = pci_read32(dev->bus, dev->dev, dev->fn, 0x10);
  bar1 = pci_read32(dev->bus, dev->dev, dev->fn, 0x14);
  if (bar0 & 1u) {
    return 0;                                   // I/O BAR: not an NVMe register window
  }
  bar = (uint64_t)(bar0 & 0xFFFFFFF0u);
  if (((bar0 >> 1) & 3u) == 2u) {               // 64-bit BAR
    bar |= (uint64_t)bar1 << 32;
  }
  if (bar == 0 || !paging_map_mmio(bar, 0x4000)) {
    return 0;
  }
  c->base = (uintptr_t)bar;

  cap = rd64(c, REG_CAP);
  mqes = (uint32_t)(cap & 0xFFFFu);
  timeout_ms = (uint32_t)(((cap >> 24) & 0xFFu) + 1u) * 500u;
  dstrd = (uint32_t)((cap >> 32) & 0xFu);
  mpsmin = (uint32_t)((cap >> 48) & 0xFu);
  c->stride = 4u << dstrd;
  if (mpsmin != 0) {
    return 0;                                   // needs pages larger than 4 KiB
  }
  if (mqes + 1u < IO_QUEUE_ENTRIES) {
    return 0;
  }

  // Reset: disable, wait for not-ready, mask pin interrupts (we poll).
  wr32(c, REG_CC, rd32(c, REG_CC) & ~CC_EN);
  if (!wait_ready(c, 0, timeout_ms)) {
    return 0;
  }
  wr32(c, REG_INTMS, 0xFFFFFFFFu);

  admin_sq = alloc_mapped(1);
  admin_cq = alloc_mapped(1);
  io_sq = alloc_mapped(1);
  io_cq = alloc_mapped(1);
  c->bounce_phys = alloc_mapped(BOUNCE_PAGES);
  c->prp_list_phys = alloc_mapped(1);
  if (!admin_sq || !admin_cq || !io_sq || !io_cq || !c->bounce_phys || !c->prp_list_phys) {
    return 0;
  }
  c->bounce = (uint8_t *)(uintptr_t)c->bounce_phys;
  c->prp_list = (uint64_t *)(uintptr_t)c->prp_list_phys;

  c->admin.sq = (uint32_t *)(uintptr_t)admin_sq;
  c->admin.cq = (uint32_t *)(uintptr_t)admin_cq;
  c->admin.entries = ADMIN_QUEUE_ENTRIES;
  c->admin.phase = 1;
  c->admin.qid = 0;
  c->io.sq = (uint32_t *)(uintptr_t)io_sq;
  c->io.cq = (uint32_t *)(uintptr_t)io_cq;
  c->io.entries = IO_QUEUE_ENTRIES;
  c->io.phase = 1;
  c->io.qid = 1;

  wr32(c, REG_AQA, ((ADMIN_QUEUE_ENTRIES - 1u) << 16) | (ADMIN_QUEUE_ENTRIES - 1u));
  wr64(c, REG_ASQ, admin_sq);
  wr64(c, REG_ACQ, admin_cq);
  // Enable: 64-byte SQ entries, 16-byte CQ entries, 4 KiB pages, NVM command set.
  wr32(c, REG_CC, CC_EN | (6u << 16) | (4u << 20));
  if (!wait_ready(c, 1, timeout_ms)) {
    return 0;
  }

  // One I/O queue pair.
  status = admin_cmd(c, ADMIN_SET_FEATURES, 0, 0, 0x07, 0);       // number of queues: 1 + 1
  (void)status;
  if (admin_cmd(c, ADMIN_CREATE_CQ, 0, io_cq, ((IO_QUEUE_ENTRIES - 1u) << 16) | 1u, 1u) != 0) {
    return 0;
  }
  if (admin_cmd(c, ADMIN_CREATE_SQ, 0, io_sq, ((IO_QUEUE_ENTRIES - 1u) << 16) | 1u, (1u << 16) | 1u) != 0) {
    return 0;
  }

  // Identify controller.
  id = c->bounce;
  memset(id, 0, 4096);
  if (admin_cmd(c, ADMIN_IDENTIFY, 0, c->bounce_phys, 1, 0) != 0) {
    return 0;
  }
  {
    uint8_t mdts = id[77];
    c->max_chunk_sectors = BOUNCE_SECTORS;
    if (mdts != 0 && mdts < 12) {
      uint32_t limit = (1u << mdts) * 4096u / 512u;
      if (limit < c->max_chunk_sectors) {
        c->max_chunk_sectors = limit;
      }
    }
  }
  disk = &g_disks[g_disk_count];
  for (i = 0; i < 40; i++) {
    disk->model[i] = (char)id[24 + i];
  }
  disk->model[40] = 0;
  trim_text(disk->model);

  // First active namespace (fall back to namespace 1).
  memset(id, 0, 4096);
  if (admin_cmd(c, ADMIN_IDENTIFY, 0, c->bounce_phys, 2, 0) == 0) {
    uint32_t first = (uint32_t)id[0] | ((uint32_t)id[1] << 8) | ((uint32_t)id[2] << 16) |
                     ((uint32_t)id[3] << 24);
    if (first != 0) {
      nsid = first;
    }
  }
  memset(id, 0, 4096);
  if (admin_cmd(c, ADMIN_IDENTIFY, nsid, c->bounce_phys, 0, 0) != 0) {
    return 0;
  }
  lbaf_index = id[26] & 0x0Fu;
  lbads = id[128 + lbaf_index * 4 + 2];
  if (lbads != 9) {
    return 0;                                   // 4 KiB-sector namespaces are not supported yet
  }
  disk->controller = ctrl_index;
  disk->nsid = nsid;
  disk->sectors = (uint64_t)id[0] | ((uint64_t)id[1] << 8) | ((uint64_t)id[2] << 16) |
                  ((uint64_t)id[3] << 24) | ((uint64_t)id[4] << 32) | ((uint64_t)id[5] << 40) |
                  ((uint64_t)id[6] << 48) | ((uint64_t)id[7] << 56);
  if (disk->sectors == 0) {
    return 0;
  }
  c->ready = 1;
  g_disk_count++;
  return 1;
}

int nvme_init(void) {
  pci_dev_t dev;
  int index;

  g_ctrl_count = 0;
  g_disk_count = 0;
  for (index = 0; index < NVME_MAX_DISKS; index++) {
    if (!pci_find_class(0x01, 0x08, 0x02, index, &dev)) {
      break;
    }
    init_controller(g_ctrl_count, &dev);
    g_ctrl_count++;
  }
  return g_disk_count;
}

int nvme_disk_count(void) { return g_disk_count; }

const nvme_disk_t *nvme_disk(int index) {
  return (index >= 0 && index < g_disk_count) ? &g_disks[index] : 0;
}

static int transfer(int index, uint64_t lba, uint32_t count, uint8_t *buf, int write) {
  const nvme_disk_t *d = nvme_disk(index);
  controller_t *c;

  if (!d) {
    return 0;
  }
  c = &g_ctrl[d->controller];
  if (!c->ready || lba + count > d->sectors) {
    return 0;
  }
  while (count > 0) {
    uint32_t chunk = count > c->max_chunk_sectors ? c->max_chunk_sectors : count;
    uint32_t bytes = chunk * 512u;
    uint32_t pages = (bytes + 4095u) / 4096u;
    uint32_t cmd[16];
    uint32_t i;

    if (write) {
      memcpy(c->bounce, buf, bytes);
    }
    memset(cmd, 0, sizeof(cmd));
    cmd[0] = write ? IO_WRITE : IO_READ;
    cmd[1] = d->nsid;
    cmd[6] = (uint32_t)(c->bounce_phys & 0xFFFFFFFFu);
    cmd[7] = (uint32_t)(c->bounce_phys >> 32);
    if (pages == 2) {
      cmd[8] = (uint32_t)((c->bounce_phys + 4096u) & 0xFFFFFFFFu);
      cmd[9] = (uint32_t)((c->bounce_phys + 4096u) >> 32);
    } else if (pages > 2) {
      for (i = 0; i + 1 < pages; i++) {
        c->prp_list[i] = c->bounce_phys + (uint64_t)(i + 1) * 4096u;
      }
      cmd[8] = (uint32_t)(c->prp_list_phys & 0xFFFFFFFFu);
      cmd[9] = (uint32_t)(c->prp_list_phys >> 32);
    }
    cmd[10] = (uint32_t)(lba & 0xFFFFFFFFu);
    cmd[11] = (uint32_t)(lba >> 32);
    cmd[12] = chunk - 1u;
    if (submit(c, &c->io, cmd) != 0) {
      return 0;
    }
    if (!write) {
      memcpy(buf, c->bounce, bytes);
    }
    lba += chunk;
    buf += bytes;
    count -= chunk;
  }
  return 1;
}

int nvme_read(int index, uint64_t lba, uint32_t count, void *buf) {
  return transfer(index, lba, count, (uint8_t *)buf, 0);
}

int nvme_write(int index, uint64_t lba, uint32_t count, const void *buf) {
  return transfer(index, lba, count, (uint8_t *)(uintptr_t)buf, 1);
}
