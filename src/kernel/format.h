#ifndef KERNEL_FORMAT_H
#define KERNEL_FORMAT_H

#include "console.h"

// Disk partitioning (GPT) and FAT32 formatting.

#define GPT_MAX_PARTS 8

typedef struct {
  const uint8_t *type_guid;   // 16 bytes, on-disk (mixed-endian) form
  uint64_t start;             // first sector
  uint64_t sectors;           // length in sectors
  char name[36];              // shown by other tools (ASCII only)
} gpt_part_t;

// Well-known partition type GUIDs in on-disk form.
extern const uint8_t GPT_TYPE_ESP[16];          // EFI System Partition
extern const uint8_t GPT_TYPE_BASIC_DATA[16];   // "Microsoft basic data" (FAT32 data)

// CRC-32 (IEEE 802.3), as used by GPT.
uint32_t crc32(const void *data, uint32_t len);

// Fill 16 bytes with a random-looking GUID (version 4 layout).
void make_guid(uint8_t out[16]);

// Erase the partition tables of `disk` and write a fresh GPT (protective MBR,
// primary and backup headers and entry arrays). Partitions must be 1 MiB aligned
// and inside the usable area. Returns 1 on success.
int gpt_write(int disk, const gpt_part_t *parts, int count);

// Convenience layout used by the installer: an EFI System Partition of `esp_mib`
// MiB followed by one data partition filling the rest of the disk. On success
// fills the start/length of both.
int gpt_layout_install(int disk, uint32_t esp_mib,
                       uint64_t *esp_start, uint64_t *esp_sectors,
                       uint64_t *data_start, uint64_t *data_sectors);

// Overwrite the first and last sectors where old partition tables or file system
// signatures could survive (MBR/GPT areas), without touching the rest.
int disk_wipe_metadata(int disk);

// Format a block device (partition) as FAT32. `label` is up to 11 characters.
// `progress`, if set, is called with a 0-100 percentage while the FATs are
// cleared (runtime-assigned function pointer). Returns 1 on success.
int fat32_format(int block_dev, const char *label, void (*progress)(int percent));

#endif // KERNEL_FORMAT_H
