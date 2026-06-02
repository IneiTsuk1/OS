#pragma once
#include <stdint.h>
#include "../../kernel/vfs/vfs.h"

// ---------------------------------------------------------------------------
// FAT32 filesystem driver
//
// Implements the vfs_ops_t vtable for a FAT32 volume on a given ATA drive.
// Supports: open, read, write, close, unlink, readdir, mkdir, rmdir, create.
//
// Limitations (M7):
//   - Single volume, single partition (reads partition 0 from MBR)
//   - 8.3 filenames only for create/open; long filenames are read-only
//   - No file locking
//   - Sector size must be 512 bytes
//   - Cluster size must not exceed 4096 bytes (8 sectors)
// ---------------------------------------------------------------------------

// Initialise the FAT32 driver on `drive_index` (ATA drive 0-3).
// Reads the MBR, locates partition 0, reads and validates the BPB.
// Returns 0 on success and fills `*ops` with the vfs_ops_t vtable.
// Returns negative on error (drive not present, not FAT32, etc).
int fat32_init(uint8_t drive_index, vfs_ops_t* ops);