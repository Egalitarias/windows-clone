#ifndef KERNEL_NVME_H
#define KERNEL_NVME_H

#include "console.h"

// NVMe controller driver: one admin queue and one I/O queue pair, polled, one
// command in flight, DMA through a physically contiguous bounce buffer. Only
// namespaces with 512-byte logical blocks are exposed.

#define NVME_MAX_DISKS 4

typedef struct {
  int controller;
  uint32_t nsid;
  uint64_t sectors;       // capacity in 512-byte sectors
  char model[41];
} nvme_disk_t;

// Find controllers (PCI class 01:08:02), bring them up and identify their first
// namespace. Returns the number of usable disks.
int nvme_init(void);

int nvme_disk_count(void);
const nvme_disk_t *nvme_disk(int index);

// Return 1 on success, 0 on error. Any `count` works (split internally).
int nvme_read(int index, uint64_t lba, uint32_t count, void *buf);
int nvme_write(int index, uint64_t lba, uint32_t count, const void *buf);

#endif // KERNEL_NVME_H
