#include "pci.h"

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC

static inline void outl(uint16_t port, uint32_t value) {
  __asm__ volatile("outl %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint32_t inl(uint16_t port) {
  uint32_t value;
  __asm__ volatile("inl %1, %0" : "=a"(value) : "Nd"(port));
  return value;
}

static uint32_t config_address(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t offset) {
  return 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)(dev & 0x1F) << 11) |
         ((uint32_t)(fn & 0x07) << 8) | (uint32_t)(offset & 0xFC);
}

uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t offset) {
  outl(PCI_CONFIG_ADDRESS, config_address(bus, dev, fn, offset));
  return inl(PCI_CONFIG_DATA);
}

void pci_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t offset, uint32_t value) {
  outl(PCI_CONFIG_ADDRESS, config_address(bus, dev, fn, offset));
  outl(PCI_CONFIG_DATA, value);
}

int pci_find_class(uint8_t class_code, uint8_t subclass, uint8_t prog_if,
                   int index, pci_dev_t *out) {
  int bus, dev, fn;
  int seen = 0;

  for (bus = 0; bus < 256; bus++) {
    for (dev = 0; dev < 32; dev++) {
      uint32_t id = pci_read32((uint8_t)bus, (uint8_t)dev, 0, 0x00);
      int functions = 1;

      if ((id & 0xFFFFu) == 0xFFFFu) {
        continue;   // no device in this slot
      }
      if (pci_read32((uint8_t)bus, (uint8_t)dev, 0, 0x0C) & 0x00800000u) {
        functions = 8;   // multi-function device
      }
      for (fn = 0; fn < functions; fn++) {
        uint32_t cls;
        id = pci_read32((uint8_t)bus, (uint8_t)dev, (uint8_t)fn, 0x00);
        if ((id & 0xFFFFu) == 0xFFFFu) {
          continue;
        }
        cls = pci_read32((uint8_t)bus, (uint8_t)dev, (uint8_t)fn, 0x08);
        if ((uint8_t)(cls >> 24) == class_code && (uint8_t)(cls >> 16) == subclass &&
            (prog_if == 0xFF || (uint8_t)(cls >> 8) == prog_if)) {
          if (seen == index) {
            out->bus = (uint8_t)bus;
            out->dev = (uint8_t)dev;
            out->fn = (uint8_t)fn;
            out->vendor = (uint16_t)(id & 0xFFFFu);
            out->device = (uint16_t)(id >> 16);
            out->class_code = class_code;
            out->subclass = subclass;
            out->prog_if = (uint8_t)(cls >> 8);
            return 1;
          }
          seen++;
        }
      }
    }
  }
  return 0;
}

void pci_enable_device(const pci_dev_t *d) {
  uint32_t cmd = pci_read32(d->bus, d->dev, d->fn, 0x04);
  cmd |= 0x06;       // memory space + bus master
  cmd &= ~0x400u;    // make sure legacy INTx is not disabled by firmware
  pci_write32(d->bus, d->dev, d->fn, 0x04, cmd);
}
