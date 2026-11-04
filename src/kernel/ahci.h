#ifndef KERNEL_AHCI_H
#define KERNEL_AHCI_H

#include "console.h"

// AHCI (SATA) host controller driver: polled, one command at a time, DMA
// through a physically contiguous bounce buffer.

#define AHCI_MAX_DISKS 8
#define AHCI_SECTOR_SIZE 512

typedef struct {
  int port;              // HBA port number
  uint64_t sectors;      // capacity in 512-byte sectors
  char model[41];
} ahci_disk_t;

// Find the controller, bring up every SATA disk and IDENTIFY it. Returns the
// number of disks found (0 if there is no AHCI controller).
int ahci_init(void);

int ahci_disk_count(void);
const ahci_disk_t *ahci_disk(int index);

// Returns 1 on success, 0 on error. `count` is in sectors; any size works (it
// is split into bounce-buffer sized commands).
int ahci_read(int index, uint64_t lba, uint32_t count, void *buf);
int ahci_write(int index, uint64_t lba, uint32_t count, const void *buf);

#endif // KERNEL_AHCI_H
