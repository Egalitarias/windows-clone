#ifndef KERNEL_PCI_H
#define KERNEL_PCI_H

#include "console.h"

// PCI configuration space access through the legacy I/O ports (0xCF8/0xCFC).

typedef struct {
  uint8_t bus;
  uint8_t dev;
  uint8_t fn;
  uint16_t vendor;
  uint16_t device;
  uint8_t class_code;
  uint8_t subclass;
  uint8_t prog_if;
} pci_dev_t;

uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t offset);
void pci_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t offset, uint32_t value);

// Find the index-th device with this class/subclass. prog_if 0xFF matches any.
// Returns 1 and fills `out` when found.
int pci_find_class(uint8_t class_code, uint8_t subclass, uint8_t prog_if,
                   int index, pci_dev_t *out);

// Turn on memory-space decoding and bus mastering (needed for DMA).
void pci_enable_device(const pci_dev_t *d);

#endif // KERNEL_PCI_H
