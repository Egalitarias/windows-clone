#include "disk.h"
#include "ahci.h"
#include "nvme.h"
#include "string.h"

static disk_info_t g_disks[DISK_MAX];
static int g_count = 0;

static void add_disk(disk_bus_t bus, int driver_index, uint64_t sectors, const char *model, int bus_number) {
  disk_info_t *d;
  int n;

  if (g_count >= DISK_MAX) {
    return;
  }
  d = &g_disks[g_count++];
  d->bus = bus;
  d->driver_index = driver_index;
  d->sectors = sectors;
  str_copy(d->model, model, sizeof(d->model));
  n = (int)str_copy(d->name, bus == DISK_BUS_NVME ? "nvme" : "sata", sizeof(d->name));
  u32_to_str((uint32_t)bus_number, d->name + n);
}

int disk_init(void) {
  int i, n;

  g_count = 0;
  n = ahci_init();
  for (i = 0; i < n; i++) {
    const ahci_disk_t *a = ahci_disk(i);
    add_disk(DISK_BUS_SATA, i, a->sectors, a->model, i);
  }
  n = nvme_init();
  for (i = 0; i < n; i++) {
    const nvme_disk_t *d = nvme_disk(i);
    add_disk(DISK_BUS_NVME, i, d->sectors, d->model, i);
  }
  return g_count;
}

int disk_count(void) { return g_count; }

const disk_info_t *disk_get(int index) {
  return (index >= 0 && index < g_count) ? &g_disks[index] : 0;
}

int disk_read(int index, uint64_t lba, uint32_t count, void *buf) {
  const disk_info_t *d = disk_get(index);
  if (!d) {
    return 0;
  }
  return d->bus == DISK_BUS_NVME ? nvme_read(d->driver_index, lba, count, buf)
                                 : ahci_read(d->driver_index, lba, count, buf);
}

int disk_write(int index, uint64_t lba, uint32_t count, const void *buf) {
  const disk_info_t *d = disk_get(index);
  if (!d) {
    return 0;
  }
  return d->bus == DISK_BUS_NVME ? nvme_write(d->driver_index, lba, count, buf)
                                 : ahci_write(d->driver_index, lba, count, buf);
}
