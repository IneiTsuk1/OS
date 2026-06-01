#pragma once
#include <stdint.h>

// ---------------------------------------------------------------------------
// ATA PIO driver
//
// Supports both primary and secondary channels, master and slave per channel
// (4 drives total).  All I/O is synchronous PIO — no DMA, no interrupts.
//
// Drive addressing:
//   ATA_PRIMARY_MASTER   = 0
//   ATA_PRIMARY_SLAVE    = 1
//   ATA_SECONDARY_MASTER = 2
//   ATA_SECONDARY_SLAVE  = 3
// ---------------------------------------------------------------------------

#define ATA_PRIMARY_MASTER    0
#define ATA_PRIMARY_SLAVE     1
#define ATA_SECONDARY_MASTER  2
#define ATA_SECONDARY_SLAVE   3
#define ATA_MAX_DRIVES        4

#define ATA_SECTOR_SIZE       512

typedef struct {
    uint8_t  present;               // 1 = drive detected and identified
    uint8_t  channel;               // 0 = primary, 1 = secondary
    uint8_t  slave;                 // 0 = master, 1 = slave
    uint32_t sector_count;          // total 28-bit LBA sectors
    char     model[41];             // model string from IDENTIFY (null-terminated)
} ata_drive_t;

// Initialise the ATA subsystem.  Detects and identifies all drives.
// Must be called after interrupts are enabled but before FAT32 init.
void ata_init(void);

// Returns a pointer to the drive descriptor for `drive_index` (0-3),
// or NULL if the index is out of range or the drive is not present.
const ata_drive_t* ata_get_drive(uint8_t drive_index);

// Read `count` sectors starting at `lba` from `drive_index` into `buf`.
// `buf` must be at least count * ATA_SECTOR_SIZE bytes.
// Returns 0 on success, negative on error.
int ata_read_sectors(uint8_t drive_index, uint32_t lba,
                     uint8_t count, void* buf);

// Write `count` sectors starting at `lba` to `drive_index` from `buf`.
// Returns 0 on success, negative on error.
int ata_write_sectors(uint8_t drive_index, uint32_t lba,
                      uint8_t count, const void* buf);