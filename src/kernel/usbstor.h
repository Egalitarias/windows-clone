#ifndef KERNEL_USBSTOR_H
#define KERNEL_USBSTOR_H

#include "console.h"
#include "ps2.h"

// USB mass storage and HID (keyboard, mouse): an xHCI host controller driver (polled; root-hub ports and
// USB hubs up to 5 tiers deep, hot plug by polling) plus the Bulk-Only Transport with SCSI commands. Memory sticks and USB disks
// with 512-byte blocks show up as disks next to SATA and NVMe ones.

#define USB_MAX_DISKS 4

typedef struct {
  int controller;           // xHCI controller index
  int slot;                 // device slot on that controller
  int present;              // 0 once the stick has been unplugged (the entry is then reused)
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

// Hot plug. Call it often from one task (the desktop loop): about every 250 ms it looks at the
// root ports and at every hub's ports, enumerates what was plugged in and drops what was
// unplugged. Returns USB_HOT_* bits for what changed (0 almost always). After a disk change
// the caller refreshes the disk list (disk_refresh_usb) and remounts (fs_rescan).
#define USB_HOT_DISK_ADDED     0x1
#define USB_HOT_DISK_REMOVED   0x2
#define USB_HOT_INPUT_ADDED    0x4
#define USB_HOT_INPUT_REMOVED  0x8
int usbstor_hotplug_poll(void);

// Number of entries (including unplugged ones: check ->present).
int usbstor_disk_count(void);
const usb_disk_t *usbstor_disk(int index);

// Return 1 on success, 0 on error. Any `count` works (split internally).
int usbstor_read(int index, uint64_t lba, uint32_t count, void *buf);
int usbstor_write(int index, uint64_t lba, uint32_t count, const void *buf);

#endif // KERNEL_USBSTOR_H
