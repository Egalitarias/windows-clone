#include "ahci.h"
#include "paging.h"
#include "pci.h"
#include "pmm.h"
#include "string.h"
#include "timer.h"

// ---- HBA registers (offsets from ABAR) ------------------------------------
#define HBA_CAP   0x00
#define HBA_GHC   0x04
#define HBA_IS    0x08
#define HBA_PI    0x0C
#define HBA_BOHC  0x28

#define GHC_AE    (1u << 31)

// Port registers, relative to 0x100 + port * 0x80
#define PX_CLB    0x00
#define PX_CLBU   0x04
#define PX_FB     0x08
#define PX_FBU    0x0C
#define PX_IS     0x10
#define PX_CMD    0x18
#define PX_TFD    0x20
#define PX_SIG    0x24
#define PX_SSTS   0x28
#define PX_SERR   0x30
#define PX_SACT   0x34
#define PX_CI     0x38

#define CMD_ST    (1u << 0)
#define CMD_FRE   (1u << 4)
#define CMD_FR    (1u << 14)
#define CMD_CR    (1u << 15)

#define TFD_ERR   (1u << 0)
#define TFD_DRQ   (1u << 3)
#define TFD_BSY   (1u << 7)
#define IS_TFES   (1u << 30)

#define SIG_ATA   0x00000101u

#define ATA_IDENTIFY      0xEC
#define ATA_READ_DMA_EXT  0x25
#define ATA_WRITE_DMA_EXT 0x35

#define BOUNCE_PAGES   16
#define BOUNCE_BYTES   (BOUNCE_PAGES * 4096)
#define BOUNCE_SECTORS (BOUNCE_BYTES / AHCI_SECTOR_SIZE)

#define TIMEOUT_MS     5000

// One page per port: command list (1 KB) + received FIS (256 B) + command table.
#define PORT_PAGE_CMD_LIST 0x000
#define PORT_PAGE_FIS      0x400
#define PORT_PAGE_CMD_TBL  0x600

static uintptr_t g_abar = 0;
static int g_disk_count = 0;
static ahci_disk_t g_disks[AHCI_MAX_DISKS];
static uint64_t g_port_page[AHCI_MAX_DISKS];      // physical == virtual (identity mapped)
static uint8_t *g_bounce = 0;
static uint64_t g_bounce_phys = 0;

static inline uint32_t rd(uint32_t off) {
  return *(volatile uint32_t *)(g_abar + off);
}

static inline void wr(uint32_t off, uint32_t value) {
  *(volatile uint32_t *)(g_abar + off) = value;
}

static inline uint32_t prd(int port, uint32_t reg) {
  return rd(0x100u + (uint32_t)port * 0x80u + reg);
}

static inline void pwr(int port, uint32_t reg, uint32_t value) {
  wr(0x100u + (uint32_t)port * 0x80u + reg, value);
}

static int wait_clear(int port, uint32_t reg, uint32_t mask, uint32_t ms) {
  uint64_t start = timer_get_ms();
  while (prd(port, reg) & mask) {
    if (timer_get_ms() - start > ms) {
      return 0;
    }
  }
  return 1;
}

static int stop_port(int port) {
  uint32_t cmd = prd(port, PX_CMD);
  cmd &= ~CMD_ST;
  pwr(port, PX_CMD, cmd);
  if (!wait_clear(port, PX_CMD, CMD_CR, 1000)) {
    return 0;
  }
  cmd = prd(port, PX_CMD);
  cmd &= ~CMD_FRE;
  pwr(port, PX_CMD, cmd);
  return wait_clear(port, PX_CMD, CMD_FR, 1000);
}

static int start_port(int port) {
  uint32_t cmd;
  if (!wait_clear(port, PX_TFD, TFD_BSY | TFD_DRQ, 2000)) {
    return 0;
  }
  cmd = prd(port, PX_CMD);
  cmd |= CMD_FRE;
  pwr(port, PX_CMD, cmd);
  cmd |= CMD_ST;
  pwr(port, PX_CMD, cmd);
  return 1;
}

// Run one DMA command on `slot` 0 of the port. Returns 1 on success.
static int run_command(int disk, uint8_t ata_cmd, uint64_t lba, uint32_t count,
                       int write, uint32_t bytes) {
  int port = g_disks[disk].port;
  uint8_t *page = (uint8_t *)(uintptr_t)g_port_page[disk];
  uint32_t *header = (uint32_t *)(page + PORT_PAGE_CMD_LIST);
  uint8_t *table = page + PORT_PAGE_CMD_TBL;
  uint8_t *fis = table;                       // command FIS is at the start of the table
  uint32_t *prdt = (uint32_t *)(table + 0x80);
  uint64_t start;

  if (!wait_clear(port, PX_TFD, TFD_BSY | TFD_DRQ, 2000)) {
    return 0;
  }

  memset(table, 0, 0x90);
  fis[0] = 0x27;                              // register host-to-device FIS
  fis[1] = 0x80;                              // command bit
  fis[2] = ata_cmd;
  fis[7] = (ata_cmd == ATA_IDENTIFY) ? 0 : 0x40;   // LBA mode
  fis[4] = (uint8_t)(lba & 0xFF);
  fis[5] = (uint8_t)((lba >> 8) & 0xFF);
  fis[6] = (uint8_t)((lba >> 16) & 0xFF);
  fis[8] = (uint8_t)((lba >> 24) & 0xFF);
  fis[9] = (uint8_t)((lba >> 32) & 0xFF);
  fis[10] = (uint8_t)((lba >> 40) & 0xFF);
  fis[12] = (uint8_t)(count & 0xFF);
  fis[13] = (uint8_t)((count >> 8) & 0xFF);

  prdt[0] = (uint32_t)(g_bounce_phys & 0xFFFFFFFFu);
  prdt[1] = (uint32_t)(g_bounce_phys >> 32);
  prdt[2] = 0;
  prdt[3] = (bytes - 1u) & 0x3FFFFFu;

  header[0] = 5u | (write ? (1u << 6) : 0u) | (1u << 16);   // 5 dwords FIS, 1 PRD entry
  header[1] = 0;
  header[2] = (uint32_t)((uint64_t)(uintptr_t)table & 0xFFFFFFFFu);
  header[3] = (uint32_t)((uint64_t)(uintptr_t)table >> 32);

  pwr(port, PX_IS, 0xFFFFFFFFu);
  pwr(port, PX_SERR, 0xFFFFFFFFu);
  __asm__ volatile("" : : : "memory");
  pwr(port, PX_CI, 1u);

  start = timer_get_ms();
  for (;;) {
    if (!(prd(port, PX_CI) & 1u)) {
      break;
    }
    if (prd(port, PX_IS) & IS_TFES) {
      return 0;
    }
    if (timer_get_ms() - start > TIMEOUT_MS) {
      return 0;
    }
  }
  if (prd(port, PX_IS) & IS_TFES) {
    return 0;
  }
  return !(prd(port, PX_TFD) & TFD_ERR);
}

static void identify(int disk) {
  uint16_t *id = (uint16_t *)g_bounce;
  uint64_t sectors;
  int i;

  g_disks[disk].sectors = 0;
  g_disks[disk].model[0] = 0;
  if (!run_command(disk, ATA_IDENTIFY, 0, 1, 0, 512)) {
    return;
  }
  sectors = (uint64_t)id[100] | ((uint64_t)id[101] << 16) | ((uint64_t)id[102] << 32) |
            ((uint64_t)id[103] << 48);
  if (sectors == 0) {
    sectors = (uint64_t)id[60] | ((uint64_t)id[61] << 16);
  }
  g_disks[disk].sectors = sectors;
  for (i = 0; i < 20; i++) {          // words 27-46, two chars per word, high byte first
    g_disks[disk].model[i * 2] = (char)(id[27 + i] >> 8);
    g_disks[disk].model[i * 2 + 1] = (char)(id[27 + i] & 0xFF);
  }
  g_disks[disk].model[40] = 0;
  for (i = 39; i >= 0 && g_disks[disk].model[i] == ' '; i--) {
    g_disks[disk].model[i] = 0;
  }
}

int ahci_init(void) {
  pci_dev_t dev;
  uint32_t bar5, pi;
  int port;
  uint64_t bounce;

  g_disk_count = 0;
  if (!pci_find_class(0x01, 0x06, 0x01, 0, &dev)) {
    return 0;
  }
  pci_enable_device(&dev);
  bar5 = pci_read32(dev.bus, dev.dev, dev.fn, 0x24);
  if (bar5 & 1u) {
    return 0;                                   // I/O BAR: not AHCI
  }
  g_abar = (uintptr_t)(bar5 & 0xFFFFFFF0u);
  if (g_abar == 0 || !paging_map_mmio(g_abar, 0x2000)) {
    return 0;
  }

  bounce = pmm_alloc_pages(BOUNCE_PAGES);
  if (bounce == 0 || !paging_map_range(bounce, BOUNCE_BYTES)) {
    return 0;
  }
  g_bounce_phys = bounce;
  g_bounce = (uint8_t *)(uintptr_t)bounce;

  wr(HBA_GHC, rd(HBA_GHC) | GHC_AE);
  wr(HBA_IS, 0xFFFFFFFFu);
  pi = rd(HBA_PI);

  for (port = 0; port < 32 && g_disk_count < AHCI_MAX_DISKS; port++) {
    uint32_t ssts, sig;
    uint64_t page_phys;
    uint8_t *page;
    int disk;

    if (!(pi & (1u << port))) {
      continue;
    }
    ssts = prd(port, PX_SSTS);
    if ((ssts & 0xF) != 3 || ((ssts >> 8) & 0xF) != 1) {
      continue;                                 // nothing attached or link not active
    }
    sig = prd(port, PX_SIG);
    if (sig != SIG_ATA) {
      continue;                                 // ATAPI, port multiplier, ...
    }
    if (!stop_port(port)) {
      continue;
    }

    page_phys = pmm_alloc_page();
    if (page_phys == 0 || !paging_map_range(page_phys, 4096)) {
      continue;
    }
    page = (uint8_t *)(uintptr_t)page_phys;
    memset(page, 0, 4096);

    disk = g_disk_count;
    g_port_page[disk] = page_phys;
    g_disks[disk].port = port;
    pwr(port, PX_CLB, (uint32_t)(page_phys + PORT_PAGE_CMD_LIST));
    pwr(port, PX_CLBU, (uint32_t)((page_phys + PORT_PAGE_CMD_LIST) >> 32));
    pwr(port, PX_FB, (uint32_t)(page_phys + PORT_PAGE_FIS));
    pwr(port, PX_FBU, (uint32_t)((page_phys + PORT_PAGE_FIS) >> 32));
    pwr(port, PX_SERR, 0xFFFFFFFFu);
    pwr(port, PX_IS, 0xFFFFFFFFu);
    pwr(port, 0x14, 0);                         // PxIE: we poll
    if (!start_port(port)) {
      continue;
    }

    identify(disk);
    if (g_disks[disk].sectors != 0) {
      g_disk_count++;
    }
  }
  return g_disk_count;
}

int ahci_disk_count(void) { return g_disk_count; }

const ahci_disk_t *ahci_disk(int index) {
  if (index < 0 || index >= g_disk_count) {
    return 0;
  }
  return &g_disks[index];
}

static int transfer(int index, uint64_t lba, uint32_t count, uint8_t *buf, int write) {
  if (index < 0 || index >= g_disk_count || !g_bounce) {
    return 0;
  }
  if (lba + count > g_disks[index].sectors) {
    return 0;
  }
  while (count > 0) {
    uint32_t chunk = count > BOUNCE_SECTORS ? BOUNCE_SECTORS : count;
    uint32_t bytes = chunk * AHCI_SECTOR_SIZE;

    if (write) {
      memcpy(g_bounce, buf, bytes);
    }
    if (!run_command(index, write ? ATA_WRITE_DMA_EXT : ATA_READ_DMA_EXT, lba, chunk, write, bytes)) {
      return 0;
    }
    if (!write) {
      memcpy(buf, g_bounce, bytes);
    }
    lba += chunk;
    buf += bytes;
    count -= chunk;
  }
  return 1;
}

int ahci_read(int index, uint64_t lba, uint32_t count, void *buf) {
  return transfer(index, lba, count, (uint8_t *)buf, 0);
}

int ahci_write(int index, uint64_t lba, uint32_t count, const void *buf) {
  return transfer(index, lba, count, (uint8_t *)(uintptr_t)buf, 1);
}
