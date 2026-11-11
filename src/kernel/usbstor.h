#ifndef KERNEL_USBSTOR_H
#define KERNEL_USBSTOR_H

#include "console.h"
#include "ps2.h"

// USB mass storage and HID (keyboard, mouse): an xHCI host controller driver (polled; root-hub ports and
// USB hubs up to 5 tiers deep, no hot plug) plus the Bulk-Only Transport with SCSI commands. Memory sticks and USB disks
// with 512-byte blocks show up as disks next to SATA and NVMe ones.

#define USB_MAX_DISKS 4

typedef struct {
  int controller;           // xHCI controller index
  int slot;                 // device slot on that controller
  uint64_t sectors;         // capacity in 512-byte sectors
  char model[41];
} usb_disk_t;

// Find xHCI controllers (PCI class 0C:03:30), enumerate what is plugged into their
// ports and register every mass-storage device. Returns the number of usable disks.
int usbstor_init(void);

// Keyboards and mice found by usbstor_init (boot protocol, polled): fetch one decoded
// event, in the same form ps2_poll gives. Returns 1 if there was one. Cheap when no USB
// keyboard or mouse is plugged in.
int usbhid_poll(ps2_event_t *ev);

int usbstor_disk_count(void);
const usb_disk_t *usbstor_disk(int index);

// Return 1 on success, 0 on error. Any `count` works (split internally).
int usbstor_read(int index, uint64_t lba, uint32_t count, void *buf);
int usbstor_write(int index, uint64_t lba, uint32_t count, const void *buf);

#endif // KERNEL_USBSTOR_H
