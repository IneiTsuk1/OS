#include "fat32.h"
#include "../ata/ata.h"
#include "../../kernel/vfs/vfs.h"
#include "../../kernel/kheap.h"
#include "../../kernel/klog.h"
#include "../../kernel/panic.h"
#include <stdint.h>
#include <stddef.h>

// ---------------------------------------------------------------------------
// On-disk structures (packed, little-endian)
// ---------------------------------------------------------------------------

typedef struct {
    uint8_t  boot_jump[3];
    uint8_t  oem_name[8];
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t  fat_count;
    uint16_t root_entry_count;      // 0 for FAT32
    uint16_t total_sectors_16;      // 0 for FAT32
    uint8_t  media_type;
    uint16_t fat_size_16;           // 0 for FAT32
    uint16_t sectors_per_track;
    uint16_t head_count;
    uint32_t hidden_sectors;
    uint32_t total_sectors_32;
    // FAT32 extended BPB
    uint32_t fat_size_32;
    uint16_t ext_flags;
    uint16_t fs_version;
    uint32_t root_cluster;
    uint16_t fs_info_sector;
    uint16_t backup_boot_sector;
    uint8_t  reserved[12];
    uint8_t  drive_number;
    uint8_t  reserved1;
    uint8_t  boot_signature;
    uint32_t volume_id;
    uint8_t  volume_label[11];
    uint8_t  fs_type[8];            // "FAT32   "
} __attribute__((packed)) fat32_bpb_t;

typedef struct {
    uint8_t  name[8];               // 8.3 name, space-padded
    uint8_t  ext[3];
    uint8_t  attributes;
    uint8_t  reserved;
    uint8_t  create_time_tenths;
    uint16_t create_time;
    uint16_t create_date;
    uint16_t access_date;
    uint16_t cluster_hi;            // high 16 bits of first cluster
    uint16_t write_time;
    uint16_t write_date;
    uint16_t cluster_lo;            // low 16 bits of first cluster
    uint32_t file_size;
} __attribute__((packed)) fat32_dirent_t;

// Long Filename entry — attributes == FAT_ATTR_LFN (0x0F).
// Appears before its 8.3 entry, in reverse order (highest seq first on disk).
typedef struct {
    uint8_t  seq;           // sequence number; bit 6 set on last entry (first on disk)
    uint8_t  name1[10];     // chars 1-5  (UTF-16LE, 2 bytes each)
    uint8_t  attributes;    // always 0x0F
    uint8_t  type;          // always 0
    uint8_t  checksum;      // checksum of the 8.3 name
    uint8_t  name2[12];     // chars 6-11
    uint16_t cluster;       // always 0
    uint8_t  name3[4];      // chars 12-13
} __attribute__((packed)) fat32_lfn_t;

#define LFN_MAX_CHARS  255  // maximum LFN length

// MBR partition entry
typedef struct {
    uint8_t  status;
    uint8_t  chs_first[3];
    uint8_t  type;
    uint8_t  chs_last[3];
    uint32_t lba_start;
    uint32_t sector_count;
} __attribute__((packed)) mbr_partition_t;

// FAT32 directory entry attributes
#define FAT_ATTR_READ_ONLY  0x01
#define FAT_ATTR_HIDDEN     0x02
#define FAT_ATTR_SYSTEM     0x04
#define FAT_ATTR_VOLUME_ID  0x08
#define FAT_ATTR_DIRECTORY  0x10
#define FAT_ATTR_ARCHIVE    0x20
#define FAT_ATTR_LFN        0x0F    // long filename entry (skip these)

// Cluster values
#define FAT32_EOC       0x0FFFFFF8u  // end-of-chain marker
#define FAT32_FREE      0x00000000u
#define FAT32_BAD       0x0FFFFFF7u

// ---------------------------------------------------------------------------
// Volume state (one global instance for M7's single-volume design)
// ---------------------------------------------------------------------------

typedef struct {
    uint8_t  drive;
    uint32_t partition_lba;         // LBA of partition start (from MBR)
    uint32_t fat_lba;               // LBA of FAT region
    uint32_t data_lba;              // LBA of data region (cluster 2)
    uint32_t root_cluster;          // cluster of root directory
    uint32_t sectors_per_cluster;
    uint32_t bytes_per_cluster;
    uint32_t fat_size_sectors;      // size of one FAT in sectors
    uint8_t  fat_count;
    uint32_t max_cluster;           // FIX #9: derived from BPB, replaces hardcoded 65536
} fat32_vol_t;

// ---------------------------------------------------------------------------
// Per-open-file state stored in vfs_file_t.fs_data
// ---------------------------------------------------------------------------

typedef struct {
    char*    path;              // kmalloc'd path copy for dirent flush on close
                                // NULL for the root directory (no dirent to flush)
    uint32_t cur_cluster;       // cached cluster number
    uint32_t cur_cluster_idx;   // logical index of cur_cluster in the chain
                                // (cur_cluster == first_cluster when idx == 0)
} fat32_file_state_t;

static fat32_vol_t vol;
static uint8_t     sector_buf[512];  // scratch buffer for single-sector reads

// ---------------------------------------------------------------------------
// Utility: string helpers (no libc available)
// ---------------------------------------------------------------------------

static int str_eq(const char* a, const char* b)
{
    while (*a && *b)
        if (*a++ != *b++) return 0;
    return *a == *b;
}

static uint32_t str_len(const char* s)
{
    uint32_t n = 0;
    while (s[n]) n++;
    return n;
}

static void mem_copy(void* dst, const void* src, uint32_t n)
{
    uint8_t*       d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    while (n--) *d++ = *s++;
}

static void mem_set(void* dst, uint8_t val, uint32_t n)
{
    uint8_t* d = (uint8_t*)dst;
    while (n--) *d++ = val;
}

// ---------------------------------------------------------------------------
// 8.3 filename helpers
// ---------------------------------------------------------------------------

// Convert a null-terminated path component like "HELLO.TXT" to FAT 8.3
// format: 8 chars name + 3 chars ext, space-padded, upper-case.
static void to_83(const char* name, uint8_t out[11])
{
    mem_set(out, ' ', 11);
    int i = 0;
    int j = 0;
    while (name[i] && name[i] != '.' && j < 8) {
        char c = name[i++];
        if (c >= 'a' && c <= 'z') c -= 32;
        out[j++] = (uint8_t)c;
    }
    if (name[i] == '.') {
        i++;
        j = 8;
        while (name[i] && j < 11) {
            char c = name[i++];
            if (c >= 'a' && c <= 'z') c -= 32;
            out[j++] = (uint8_t)c;
        }
    }
}

// Convert a FAT 8.3 entry to a null-terminated filename string.
static void from_83(const uint8_t name[8], const uint8_t ext[3], char out[13])
{
    int i = 0;
    int j = 0;
    while (j < 8 && name[j] != ' ')
        out[i++] = name[j++];
    if (ext[0] != ' ') {
        out[i++] = '.';
        j = 0;
        while (j < 3 && ext[j] != ' ')
            out[i++] = ext[j++];
    }
    out[i] = '\0';
}

// Append the 13 UTF-16LE characters from one LFN entry into `buf` at
// the position indicated by `seq` (1-based).  Strips the high byte —
// works correctly for ASCII and Latin-1; non-ASCII chars become '?'.
// `buf` must be at least LFN_MAX_CHARS+1 bytes.
static void lfn_append(const fat32_lfn_t* lfn, char* buf)
{
    // seq & 0x3F gives the 1-based position of this entry's 13-char block.
    int seq = (lfn->seq & 0x3F);
    if (seq < 1 || seq > 20) return;  // guard against corrupt entries

    int base = (seq - 1) * 13;  // 0-based char offset in the full name

    // The three name fields each hold UTF-16LE pairs.
    // We extract the low byte of each pair (ASCII-safe).
    const uint8_t* src[3]  = { lfn->name1, lfn->name2, lfn->name3 };
    const int      lens[3] = { 10, 12, 4 };  // bytes in each field
    int pos = base;

    for (int f = 0; f < 3; f++) {
        for (int i = 0; i < lens[f]; i += 2) {
            uint8_t lo = src[f][i];
            uint8_t hi = src[f][i+1];
            if (lo == 0xFF && hi == 0xFF) return;  // padding sentinel
            if (lo == 0x00 && hi == 0x00) return;  // null terminator
            if (pos < LFN_MAX_CHARS) {
                buf[pos++] = (hi == 0) ? (char)lo : '?';
            }
        }
    }
    // Note: buf is null-terminated by the caller after all entries processed.
}

// ---------------------------------------------------------------------------
// Cluster / LBA arithmetic
// ---------------------------------------------------------------------------

static uint32_t cluster_to_lba(uint32_t cluster)
{
    return vol.data_lba + (cluster - 2) * vol.sectors_per_cluster;
}

// Read the FAT entry for `cluster`.  Returns the value (next cluster or EOC).
static uint32_t fat_read(uint32_t cluster)
{
    // Each FAT32 entry is 4 bytes; 128 entries per 512-byte sector.
    uint32_t fat_offset  = cluster * 4;
    uint32_t fat_sector  = vol.fat_lba + fat_offset / 512;
    uint32_t fat_index   = (fat_offset % 512) / 4;

    if (ata_read_sectors(vol.drive, fat_sector, 1, sector_buf) != 0)
        return FAT32_EOC;  // read error — treat as end of chain

    uint32_t* entries = (uint32_t*)sector_buf;
    return entries[fat_index] & 0x0FFFFFFF;
}

// Write a FAT entry for `cluster` with value `val` (to both FAT copies).
static int fat_write(uint32_t cluster, uint32_t val)
{
    uint32_t fat_offset = cluster * 4;
    uint32_t fat_sector = vol.fat_lba + fat_offset / 512;
    uint32_t fat_index  = (fat_offset % 512) / 4;

    // Update FAT1
    if (ata_read_sectors(vol.drive, fat_sector, 1, sector_buf) != 0)
        return -1;
    ((uint32_t*)sector_buf)[fat_index] = (val & 0x0FFFFFFF) |
        (((uint32_t*)sector_buf)[fat_index] & 0xF0000000);
    if (ata_write_sectors(vol.drive, fat_sector, 1, sector_buf) != 0)
        return -1;

    // Update FAT2 (if present)
    if (vol.fat_count >= 2) {
        uint32_t fat2_sector = fat_sector + vol.fat_size_sectors;
        if (ata_read_sectors(vol.drive, fat2_sector, 1, sector_buf) != 0)
            return -1;
        ((uint32_t*)sector_buf)[fat_index] = (val & 0x0FFFFFFF) |
            (((uint32_t*)sector_buf)[fat_index] & 0xF0000000);
        if (ata_write_sectors(vol.drive, fat2_sector, 1, sector_buf) != 0)
            return -1;
    }

    return 0;
}

// Allocate a free cluster.  Returns cluster number or 0 on failure.
static uint32_t fat_alloc_cluster(void)
{
    // FIX #9: use vol.max_cluster derived from BPB instead of hardcoded 65536
    for (uint32_t c = 2; c < vol.max_cluster; c++) {
        if (fat_read(c) == FAT32_FREE) {
            fat_write(c, FAT32_EOC);
            // Zero the cluster data
            uint8_t zero[512];
            mem_set(zero, 0, 512);
            uint32_t lba = cluster_to_lba(c);
            for (uint32_t s = 0; s < vol.sectors_per_cluster; s++)
                ata_write_sectors(vol.drive, lba + s, 1, zero);
            return c;
        }
    }
    return 0;  // out of space
}

// Append a cluster to the chain ending at `last_cluster`.
// Returns the new cluster, or 0 on failure.
static uint32_t fat_extend_chain(uint32_t last_cluster)
{
    uint32_t new_cluster = fat_alloc_cluster();
    if (!new_cluster)
        return 0;
    fat_write(last_cluster, new_cluster);
    return new_cluster;
}

// Free the entire cluster chain starting at `start`.
static void fat_free_chain(uint32_t start)
{
    uint32_t c = start;
    while (c && c < FAT32_EOC) {
        uint32_t next = fat_read(c);
        fat_write(c, FAT32_FREE);
        c = next;
    }
}

// ---------------------------------------------------------------------------
// Directory operations
// ---------------------------------------------------------------------------

// Read a specific directory entry from cluster chain `dir_cluster` at
// entry index `entry_index`.  Skips deleted entries when counting.
// Accumulates LFN chars from preceding LFN entries into `lfn_out`
// (must be LFN_MAX_CHARS+1 bytes, or NULL to ignore).
// Returns 0 on success, VFS_ENOENT if index is past end.
static int dir_read_entry(uint32_t dir_cluster, uint32_t entry_index,
                           fat32_dirent_t* out,
                           uint32_t* out_cluster, uint32_t* out_sector_offset,
                           char* lfn_out)
{
    uint32_t cluster = dir_cluster;
    uint32_t real_idx = 0;

    // FIX #5: lfn_buf moved off static storage to avoid non-reentrant hazard.
    char lfn_buf[LFN_MAX_CHARS + 1];
    lfn_buf[0] = '\0';

    while (cluster < FAT32_EOC) {
        uint32_t lba = cluster_to_lba(cluster);

        for (uint32_t s = 0; s < vol.sectors_per_cluster; s++) {
            if (ata_read_sectors(vol.drive, lba + s, 1, sector_buf) != 0)
                return VFS_EGENERIC;

            fat32_dirent_t* entries = (fat32_dirent_t*)sector_buf;
            uint32_t entries_per_sector = 512 / sizeof(fat32_dirent_t);

            for (uint32_t e = 0; e < entries_per_sector; e++) {
                fat32_dirent_t* ent = &entries[e];

                if (ent->name[0] == 0x00)  // end of directory
                    return VFS_ENOENT;
                if (ent->name[0] == 0xE5) {// deleted — reset LFN accumulator
                    lfn_buf[0] = '\0';
                    continue;
                }
                if (ent->attributes == FAT_ATTR_LFN) {
                    // LFN entry — accumulate chars
                    fat32_lfn_t* lfn = (fat32_lfn_t*)ent;
                    if (lfn->seq & 0x40) {
                        // First LFN entry on disk (last in logical order) —
                        // clear the buffer and set up for this name's length.
                        mem_set(lfn_buf, 0, LFN_MAX_CHARS + 1);
                    }
                    lfn_append(lfn, lfn_buf);
                    continue;
                }
                if (ent->attributes & FAT_ATTR_VOLUME_ID) {
                    lfn_buf[0] = '\0';
                    continue;
                }

                // Regular 8.3 entry
                if (real_idx == entry_index) {
                    *out = *ent;
                    if (out_cluster)       *out_cluster      = cluster;
                    if (out_sector_offset) *out_sector_offset = s;
                    // Return LFN if we have one, otherwise empty string
                    if (lfn_out) {
                        if (lfn_buf[0])
                            mem_copy(lfn_out, lfn_buf, LFN_MAX_CHARS + 1);
                        else
                            lfn_out[0] = '\0';
                    }
                    lfn_buf[0] = '\0';  // reset for next call
                    return VFS_EOK;
                }
                real_idx++;
                lfn_buf[0] = '\0';  // reset after consuming entry
            }
        }

        cluster = fat_read(cluster);
    }

    return VFS_ENOENT;
}

// Find a directory entry by 8.3 name within `dir_cluster`.
// Returns 0 and fills `out` on success.  Returns VFS_ENOENT if not found.
// Optionally returns the physical cluster + sector + entry offset for writes.
static int dir_find(uint32_t dir_cluster, const uint8_t name83[11],
                    fat32_dirent_t* out,
                    uint32_t* out_cluster, uint32_t* out_sector,
                    uint32_t* out_entry_idx)
{
    uint32_t cluster = dir_cluster;

    while (cluster < FAT32_EOC) {
        uint32_t lba = cluster_to_lba(cluster);

        for (uint32_t s = 0; s < vol.sectors_per_cluster; s++) {
            if (ata_read_sectors(vol.drive, lba + s, 1, sector_buf) != 0)
                return VFS_EGENERIC;

            fat32_dirent_t* entries = (fat32_dirent_t*)sector_buf;
            uint32_t eps = 512 / sizeof(fat32_dirent_t);

            for (uint32_t e = 0; e < eps; e++) {
                fat32_dirent_t* ent = &entries[e];

                if (ent->name[0] == 0x00) return VFS_ENOENT;
                if (ent->name[0] == 0xE5) continue;
                if (ent->attributes == FAT_ATTR_LFN) continue;
                if (ent->attributes & FAT_ATTR_VOLUME_ID) continue;

                // Compare 11-byte 8.3 name
                int match = 1;
                for (int i = 0; i < 11; i++)
                    if (ent->name[i] != name83[i]) { match = 0; break; }

                if (match) {
                    if (out)           *out           = *ent;
                    if (out_cluster)   *out_cluster   = cluster;
                    if (out_sector)    *out_sector    = s;
                    if (out_entry_idx) *out_entry_idx = e;
                    return VFS_EOK;
                }
            }
        }

        cluster = fat_read(cluster);
    }

    return VFS_ENOENT;
}

// Case-insensitive string comparison for 8.3 collision detection and LFN lookup.
// Returns 1 if equal ignoring case, 0 otherwise.
static int str_eq_nocase(const char* a, const char* b)
{
    while (*a && *b) {
        char ca = *a >= 'a' && *a <= 'z' ? *a - 32 : *a;
        char cb = *b >= 'a' && *b <= 'z' ? *b - 32 : *b;
        if (ca != cb) return 0;
        a++; b++;
    }
    char ca = *a >= 'a' && *a <= 'z' ? *a - 32 : *a;
    char cb = *b >= 'a' && *b <= 'z' ? *b - 32 : *b;
    return ca == cb;
}

// Like dir_find, but also matches against the entry's long filename.
// `name` is a null-terminated string as the user typed it (e.g. "My Long File.txt").
// Falls back to 8.3 comparison if no LFN is present for an entry.
// Returns VFS_EOK and fills `out` on success.
static int dir_find_lfn(uint32_t dir_cluster, const char* name,
                         fat32_dirent_t* out,
                         uint32_t* out_cluster, uint32_t* out_sector,
                         uint32_t* out_entry_idx)
{
    // Precompute the 8.3 form for the fallback comparison.
    uint8_t name83[11];
    to_83(name, name83);

    uint32_t cluster = dir_cluster;

    // Per-scan LFN accumulator — same pattern as dir_read_entry.
    char lfn_buf[LFN_MAX_CHARS + 1];
    lfn_buf[0] = '\0';

    while (cluster < FAT32_EOC) {
        uint32_t lba = cluster_to_lba(cluster);

        for (uint32_t s = 0; s < vol.sectors_per_cluster; s++) {
            if (ata_read_sectors(vol.drive, lba + s, 1, sector_buf) != 0)
                return VFS_EGENERIC;

            fat32_dirent_t* entries = (fat32_dirent_t*)sector_buf;
            uint32_t eps = 512 / sizeof(fat32_dirent_t);

            for (uint32_t e = 0; e < eps; e++) {
                fat32_dirent_t* ent = &entries[e];

                if (ent->name[0] == 0x00) return VFS_ENOENT;  // end of directory
                if (ent->name[0] == 0xE5) {                    // deleted
                    lfn_buf[0] = '\0';
                    continue;
                }
                if (ent->attributes == FAT_ATTR_LFN) {
                    fat32_lfn_t* lfn = (fat32_lfn_t*)ent;
                    if (lfn->seq & 0x40)
                        mem_set(lfn_buf, 0, LFN_MAX_CHARS + 1);
                    lfn_append(lfn, lfn_buf);
                    continue;
                }
                if (ent->attributes & FAT_ATTR_VOLUME_ID) {
                    lfn_buf[0] = '\0';
                    continue;
                }

                // We have a regular 8.3 entry.  Match against LFN first,
                // then fall back to 8.3.
                int match = 0;

                if (lfn_buf[0]) {
                    // Case-insensitive LFN comparison.
                    match = str_eq_nocase(lfn_buf, name);
                }

                if (!match) {
                    // 8.3 fallback: compare packed 11-byte names.
                    match = 1;
                    for (int i = 0; i < 11; i++)
                        if (ent->name[i] != name83[i]) { match = 0; break; }
                }

                lfn_buf[0] = '\0';  // reset accumulator after consuming entry

                if (match) {
                    if (out)           *out           = *ent;
                    if (out_cluster)   *out_cluster   = cluster;
                    if (out_sector)    *out_sector    = s;
                    if (out_entry_idx) *out_entry_idx = e;
                    return VFS_EOK;
                }
            }
        }

        cluster = fat_read(cluster);
    }

    return VFS_ENOENT;
}

// Write a directory entry back to disk.  out_cluster, out_sector, out_entry_idx
// must come from a previous dir_find call.
static int dir_write_entry(uint32_t dir_cluster, uint32_t sector_off,
                            uint32_t entry_idx, fat32_dirent_t* ent)
{
    uint32_t lba = cluster_to_lba(dir_cluster) + sector_off;
    if (ata_read_sectors(vol.drive, lba, 1, sector_buf) != 0)
        return VFS_EGENERIC;

    fat32_dirent_t* entries = (fat32_dirent_t*)sector_buf;
    entries[entry_idx] = *ent;

    if (ata_write_sectors(vol.drive, lba, 1, sector_buf) != 0)
        return VFS_EGENERIC;

    return VFS_EOK;
}

// Add a new directory entry to `dir_cluster`.  Reuses a deleted slot or
// extends the chain if necessary.  Returns 0 on success.
static int dir_add_entry(uint32_t dir_cluster, fat32_dirent_t* new_ent)
{
    uint32_t cluster = dir_cluster;
    uint32_t last_cluster = cluster;

    while (cluster < FAT32_EOC) {
        uint32_t lba = cluster_to_lba(cluster);
        last_cluster = cluster;

        for (uint32_t s = 0; s < vol.sectors_per_cluster; s++) {
            if (ata_read_sectors(vol.drive, lba + s, 1, sector_buf) != 0)
                return VFS_EGENERIC;

            fat32_dirent_t* entries = (fat32_dirent_t*)sector_buf;
            uint32_t eps = 512 / sizeof(fat32_dirent_t);

            for (uint32_t e = 0; e < eps; e++) {
                uint8_t first = entries[e].name[0];
                if (first == 0x00 || first == 0xE5) {
                    // Free slot — write the new entry here.
                    entries[e] = *new_ent;

                    if (first == 0x00) {
                        // We consumed an end-of-directory marker; the new
                        // marker must go in the next slot.
                        if ((e + 1) < eps) {
                            // Same sector — easy.
                            entries[e + 1].name[0] = 0x00;
                            return ata_write_sectors(vol.drive, lba + s, 1, sector_buf);
                        }
                        // FIX #5: the free slot was the last entry in this
                        // sector.  Write the current sector first, then place
                        // the end marker in the first slot of the next sector.
                        if (ata_write_sectors(vol.drive, lba + s, 1,
                                              sector_buf) != 0)
                            return VFS_EGENERIC;
                        // Move to the next sector in the cluster (or extend).
                        uint32_t next_s = s + 1;
                        uint32_t next_lba;
                        uint32_t next_cluster = cluster;
                        if (next_s < vol.sectors_per_cluster) {
                            next_lba = lba + next_s;
                        } else {
                            // Need the next cluster in the chain.
                            next_cluster = fat_read(cluster);
                            if (next_cluster >= FAT32_EOC) {
                                next_cluster = fat_extend_chain(last_cluster);
                                if (!next_cluster)
                                    return VFS_ENOSPC;
                            }
                            next_lba = cluster_to_lba(next_cluster);
                        }
                        if (ata_read_sectors(vol.drive, next_lba, 1,
                                             sector_buf) != 0)
                            return VFS_EGENERIC;
                        // Place end marker in slot 0 of that sector.
                        ((fat32_dirent_t*)sector_buf)[0].name[0] = 0x00;
                        return ata_write_sectors(vol.drive, next_lba, 1,
                                                 sector_buf);
                    }

                    // Reused a deleted (0xE5) slot — no marker needed.
                    return ata_write_sectors(vol.drive, lba + s, 1, sector_buf);
                }
            }
        }

        cluster = fat_read(cluster);
    }

    // No free slot — extend the directory chain
    uint32_t new_cluster = fat_extend_chain(last_cluster);
    if (!new_cluster)
        return VFS_ENOSPC;

    // Write entry into the first slot of the new cluster
    uint32_t lba = cluster_to_lba(new_cluster);
    if (ata_read_sectors(vol.drive, lba, 1, sector_buf) != 0)
        return VFS_EGENERIC;

    fat32_dirent_t* entries = (fat32_dirent_t*)sector_buf;
    entries[0] = *new_ent;
    entries[1].name[0] = 0x00;  // end of directory

    return ata_write_sectors(vol.drive, lba, 1, sector_buf);
}

// ---------------------------------------------------------------------------
// Path resolution
//
// Paths are Unix-style: "/foo/bar/baz.txt"
// Returns the cluster of the parent directory, the raw final component name,
// and its 8.3 form.  Returns VFS_ENOENT if any intermediate component
// is missing.
// ---------------------------------------------------------------------------

static int path_resolve_parent(const char* path,
                                uint32_t* out_dir_cluster,
                                char      out_name[VFS_MAX_NAME],
                                uint8_t   out_name83[11])
{
    // Accept relative paths by treating them as rooted
    char abspath[VFS_MAX_PATH];
    if (path[0] != '/') {
        abspath[0] = '/';
        int i = 1;
        // FIX #8: copy up to VFS_MAX_PATH-2 chars to leave room for '/' and '\0'
        while (path[i-1] && i < (int)VFS_MAX_PATH - 1) {
            abspath[i] = path[i-1];
            i++;
        }
        abspath[i] = '\0';
        path = abspath;
    }

    uint32_t dir_cluster = vol.root_cluster;

    // Walk components
    const char* p = path + 1;  // skip leading '/'
    while (*p) {
        // Extract next component
        char component[VFS_MAX_NAME];
        int  len = 0;
        while (p[len] && p[len] != '/' && len < (int)VFS_MAX_NAME - 1)
            len++;

        for (int i = 0; i < len; i++)
            component[i] = p[i];
        component[len] = '\0';

        p += len;
        if (*p == '/') p++;

        if (*p == '\0') {
            // Final component — return raw name and its 8.3 form
            for (int i = 0; i <= len; i++) out_name[i] = component[i];
            to_83(component, out_name83);
            *out_dir_cluster = dir_cluster;
            return VFS_EOK;
        }

        // Intermediate component — LFN-aware lookup
        fat32_dirent_t ent;
        int ret = dir_find_lfn(dir_cluster, component, &ent, NULL, NULL, NULL);
        if (ret != VFS_EOK)
            return VFS_ENOENT;
        if (!(ent.attributes & FAT_ATTR_DIRECTORY))
            return VFS_ENOTDIR;

        dir_cluster = ((uint32_t)ent.cluster_hi << 16) | ent.cluster_lo;
    }

    // Path ended with '/' — treat root or last dir as the target
    out_name[0] = '.'; out_name[1] = '\0';
    to_83(".", out_name83);
    *out_dir_cluster = dir_cluster;
    return VFS_EOK;
}

// Resolve full path to the cluster and dirent of the target.
static int path_resolve(const char* path, fat32_dirent_t* out,
                        uint32_t* out_dir_cluster,
                        uint32_t* out_sector, uint32_t* out_entry_idx)
{
    uint32_t dir_cluster;
    char     name[VFS_MAX_NAME];
    uint8_t  name83[11];

    int ret = path_resolve_parent(path, &dir_cluster, name, name83);
    if (ret != VFS_EOK)
        return ret;

    // Use LFN-aware search for the leaf component too.
    return dir_find_lfn(dir_cluster, name, out,
                        out_dir_cluster, out_sector, out_entry_idx);
}

// ---------------------------------------------------------------------------
// Name helpers
// ---------------------------------------------------------------------------

// Extract the final component of a path into `out` (must be VFS_MAX_NAME bytes).
// e.g. "/foo/bar/baz.txt" -> "baz.txt"
static void path_basename(const char* path, char* out)
{
    // Find the last '/'
    const char* last_slash = path;
    for (const char* p = path; *p; p++)
        if (*p == '/') last_slash = p;

    // Copy everything after it (or the whole string if no slash)
    const char* start = (*last_slash == '/') ? last_slash + 1 : path;
    int i = 0;
    while (start[i] && i < (int)VFS_MAX_NAME - 1) {
        out[i] = start[i];
        i++;
    }
    out[i] = '\0';
}

// Check whether `requested` and `existing` are distinct strings that map to
// the same 8.3 name — i.e. a silent collision would occur.
// Returns 1 if a collision exists, 0 if the names are genuinely the same.
static int name_collides_83(const char* requested, const char* existing)
{
    // If the names are identical (case-insensitively) they refer to the same
    // file and O_CREAT on an existing file is fine — not a collision.
    if (str_eq_nocase(requested, existing))
        return 0;

    // Different names that share the same 8.3 representation — collision.
    uint8_t req83[11], ex83[11];
    to_83(requested, req83);
    to_83(existing,  ex83);
    for (int i = 0; i < 11; i++)
        if (req83[i] != ex83[i]) return 0;
    return 1;
}

// ---------------------------------------------------------------------------
// VFS vtable implementation
// ---------------------------------------------------------------------------

static int fat32_open(const char* path, uint32_t flags, vfs_file_t* out)
{
    // Special case: root directory
    if (path[0] == '/' && path[1] == '\0') {
        out->flags         = flags;
        out->size          = 0;
        out->first_cluster = vol.root_cluster;
        out->is_dir        = 1;
        out->pos           = 0;
        out->fs_data       = NULL;  // root has no dirent to flush
        return VFS_EOK;
    }

    fat32_dirent_t ent;
    int ret = path_resolve(path, &ent, NULL, NULL, NULL);

    if (ret == VFS_ENOENT && (flags & O_CREAT)) {
        uint32_t dir_cluster;
        char     name[VFS_MAX_NAME];
        uint8_t  name83[11];
        if (path_resolve_parent(path, &dir_cluster, name, name83) != VFS_EOK)
            return VFS_ENOENT;

        // Check for an 8.3 collision: a different file whose name maps to the
        // same 8.3 representation already exists (e.g. creating "HellO.txt"
        // when "hello.txt" is present).  Silently opening the wrong file would
        // corrupt it, so we return VFS_EEXIST instead.
        fat32_dirent_t collision_ent;
        if (dir_find(dir_cluster, name83, &collision_ent, NULL, NULL, NULL) == VFS_EOK) {
            char existing_name[13];
            char requested_name[VFS_MAX_NAME];
            from_83(collision_ent.name, collision_ent.ext, existing_name);
            path_basename(path, requested_name);
            if (name_collides_83(requested_name, existing_name))
                return VFS_EEXIST;
        }

        // FIX #11: don't pre-allocate a cluster for empty files; allocate on
        // first write instead. Store cluster=0 to indicate no data yet.
        fat32_dirent_t new_ent;
        mem_set(&new_ent, 0, sizeof(new_ent));
        mem_copy(new_ent.name, name83, 8);
        mem_copy(new_ent.ext,  name83 + 8, 3);
        new_ent.attributes = FAT_ATTR_ARCHIVE;
        new_ent.cluster_hi = 0;
        new_ent.cluster_lo = 0;
        new_ent.file_size  = 0;

        if (dir_add_entry(dir_cluster, &new_ent) != VFS_EOK)
            return VFS_EGENERIC;

        ent = new_ent;
    } else if (ret != VFS_EOK) {
        return ret;
    }

    // FIX #4: treat kmalloc failure as a hard error rather than silently
    // continuing with NULL state (which would cause writes to be lost on close).
    fat32_file_state_t* state = (fat32_file_state_t*)kmalloc(sizeof(fat32_file_state_t));
    if (!state)
        return VFS_EGENERIC;

    uint32_t plen = str_len(path) + 1;
    state->path = (char*)kmalloc(plen);
    if (!state->path) {
        kfree(state);
        return VFS_EGENERIC;
    }
    mem_copy(state->path, path, plen);

    uint32_t first_cluster = ((uint32_t)ent.cluster_hi << 16) | ent.cluster_lo;

    // FIX #1: for zero-length files (first_cluster == 0) initialise the cache
    // to a sentinel value so the write path knows there is no cluster yet,
    // rather than starting a walk from cluster 0 which is invalid.
    state->cur_cluster     = first_cluster ? first_cluster : FAT32_EOC;
    state->cur_cluster_idx = 0;

    out->flags         = flags;
    out->size          = ent.file_size;
    out->first_cluster = first_cluster;
    out->is_dir        = (ent.attributes & FAT_ATTR_DIRECTORY) ? 1 : 0;
    out->pos           = (flags & O_APPEND) ? ent.file_size : 0;
    out->fs_data       = state;

    return VFS_EOK;
}

static int fat32_read(vfs_file_t* file, void* buf, uint32_t len)
{
    if (file->pos >= file->size)
        return 0;  // EOF

    if (len > file->size - file->pos)
        len = file->size - file->pos;

    uint8_t* dst      = (uint8_t*)buf;
    uint32_t remaining = len;

    // Determine which cluster contains file->pos
    uint32_t target_idx = file->pos / vol.bytes_per_cluster;

    // Use cluster cache if available and useful (forward seek or same cluster)
    fat32_file_state_t* state = (fat32_file_state_t*)file->fs_data;
    uint32_t cluster;
    uint32_t cluster_idx;

    if (state && state->cur_cluster < FAT32_EOC &&
        state->cur_cluster_idx <= target_idx) {
        // Continue forward from cached position
        cluster     = state->cur_cluster;
        cluster_idx = state->cur_cluster_idx;
    } else {
        // Backward seek or no cache — walk from first cluster
        cluster     = file->first_cluster;
        cluster_idx = 0;
    }

    // Walk forward to target cluster
    while (cluster_idx < target_idx && cluster < FAT32_EOC) {
        cluster = fat_read(cluster);
        cluster_idx++;
    }

    uint32_t offset_in_cluster = file->pos % vol.bytes_per_cluster;

    static uint8_t cluster_buf[4096];
    if (vol.bytes_per_cluster > sizeof(cluster_buf))
        return VFS_EGENERIC;

    while (remaining > 0 && cluster < FAT32_EOC) {
        uint32_t lba = cluster_to_lba(cluster);
        for (uint32_t s = 0; s < vol.sectors_per_cluster; s++) {
            if (ata_read_sectors(vol.drive, lba + s, 1,
                                  cluster_buf + s * 512) != 0)
                return VFS_EGENERIC;
        }

        uint32_t avail = vol.bytes_per_cluster - offset_in_cluster;
        uint32_t copy  = remaining < avail ? remaining : avail;

        mem_copy(dst, cluster_buf + offset_in_cluster, copy);
        dst               += copy;
        remaining         -= copy;
        file->pos         += copy;
        offset_in_cluster  = 0;

        if (remaining > 0) {
            cluster = fat_read(cluster);
            cluster_idx++;
        }
    }

    // Update cluster cache to the last cluster we used
    if (state) {
        state->cur_cluster     = cluster;
        state->cur_cluster_idx = cluster_idx;
    }

    return (int)(len - remaining);
}

static int fat32_write(vfs_file_t* file, const void* buf, uint32_t len)
{
    if (len == 0) return 0;

    // FIX #2 (crash-before-close): if the process dies between a write that
    // allocates a new first cluster and the fat32_close that flushes it to the
    // dirent, the FAT chain becomes orphaned (allocated but unreachable).
    // Fixing this fully would require write-through dirent updates; for now it
    // is accepted as a known limitation.  We do ensure file->pos and file->size
    // are only advanced *after* disk confirms each cluster write (FIX #1), so
    // a partial I/O failure never inflates the recorded file size.

    const uint8_t* src = (const uint8_t*)buf;
    uint32_t remaining = len;

    // FIX #1: track how many bytes have been confirmed written to disk.
    // file->pos and file->size are updated only after each cluster is
    // successfully flushed, so an ata_write_sectors failure cannot leave
    // the in-memory size larger than what is actually on disk.
    uint32_t committed = 0;

    uint32_t target_idx = file->pos / vol.bytes_per_cluster;

    fat32_file_state_t* state = (fat32_file_state_t*)file->fs_data;
    uint32_t cluster;
    uint32_t cluster_idx;
    uint32_t prev_cluster = 0;

    // Use cluster cache for forward seeks, but only if it points to a valid
    // cluster (not EOC, which is used as the sentinel for empty files).
    if (state && state->cur_cluster < FAT32_EOC &&
        state->cur_cluster_idx <= target_idx) {
        cluster     = state->cur_cluster;
        cluster_idx = state->cur_cluster_idx;
        // Recover prev_cluster so chain extension works if cache lands on EOC.
        // Walk one step back from the cached position when it is not the head.
        if (cluster_idx > 0) {
            uint32_t c = file->first_cluster;
            for (uint32_t i = 0; i + 1 < cluster_idx && c < FAT32_EOC; i++)
                c = fat_read(c);
            prev_cluster = c;
        }
    } else {
        cluster     = file->first_cluster;
        cluster_idx = 0;
    }

    // FIX #2 (original): walk forward to target cluster, extending the chain
    // as needed.  prev_cluster is kept up-to-date so chain extension always
    // links from the correct tail.
    while (cluster_idx < target_idx) {
        if (cluster >= FAT32_EOC) {
            uint32_t new_c = fat_extend_chain(prev_cluster);
            if (!new_c) {
                // Flush what we committed before returning the error.
                if (committed > 0) {
                    file->pos  += committed;
                    if (file->pos > file->size) file->size = file->pos;
                }
                return VFS_ENOSPC;
            }
            cluster = new_c;
            // Don't advance cluster_idx yet — the newly allocated cluster IS
            // at cluster_idx; let the loop condition re-check.
            continue;
        }
        prev_cluster = cluster;
        cluster      = fat_read(cluster);
        cluster_idx++;
    }

    // If we're at EOC (or the file has no clusters yet) allocate a new one.
    // FIX #7: when file->first_cluster == 0 and prev_cluster == 0 we must use
    // fat_alloc_cluster (no predecessor to extend from).  In all other cases
    // prev_cluster correctly points to the tail of the existing chain.
    if (cluster >= FAT32_EOC || cluster == 0) {
        uint32_t new_c = prev_cluster
            ? fat_extend_chain(prev_cluster)
            : fat_alloc_cluster();
        if (!new_c) return VFS_ENOSPC;
        if (file->first_cluster == 0)
            file->first_cluster = new_c;
        cluster = new_c;
    }

    uint32_t offset_in_cluster = file->pos % vol.bytes_per_cluster;
    static uint8_t cluster_buf[4096];

    if (vol.bytes_per_cluster > sizeof(cluster_buf))
        return VFS_EGENERIC;

    while (remaining > 0) {
        uint32_t lba = cluster_to_lba(cluster);
        for (uint32_t s = 0; s < vol.sectors_per_cluster; s++) {
            if (ata_read_sectors(vol.drive, lba + s, 1,
                                  cluster_buf + s * 512) != 0) {
                // FIX #1: flush what we committed, return error.
                if (committed > 0) {
                    file->pos  += committed;
                    if (file->pos > file->size) file->size = file->pos;
                }
                return VFS_EGENERIC;
            }
        }

        uint32_t avail = vol.bytes_per_cluster - offset_in_cluster;
        uint32_t copy  = remaining < avail ? remaining : avail;

        mem_copy(cluster_buf + offset_in_cluster, src, copy);

        for (uint32_t s = 0; s < vol.sectors_per_cluster; s++) {
            if (ata_write_sectors(vol.drive, lba + s, 1,
                                   cluster_buf + s * 512) != 0) {
                // FIX #1: flush what we committed, return error.
                if (committed > 0) {
                    file->pos  += committed;
                    if (file->pos > file->size) file->size = file->pos;
                }
                return VFS_EGENERIC;
            }
        }

        // Sector write succeeded — advance committed cursor.
        src               += copy;
        committed         += copy;
        remaining         -= copy;
        offset_in_cluster  = 0;

        if (remaining > 0) {
            uint32_t next = fat_read(cluster);
            if (next >= FAT32_EOC) {
                next = fat_extend_chain(cluster);
                if (!next) {
                    // FIX #1: flush committed progress before returning.
                    file->pos += committed;
                    if (file->pos > file->size) file->size = file->pos;
                    return VFS_ENOSPC;
                }
            }
            prev_cluster = cluster;
            cluster      = next;
            cluster_idx++;
        }
    }

    // All data confirmed on disk — advance pos and size together.
    file->pos += committed;
    if (file->pos > file->size)
        file->size = file->pos;

    // Update cluster cache
    if (state) {
        state->cur_cluster     = cluster;
        state->cur_cluster_idx = cluster_idx;
    }

    return (int)len;
}

// Update the directory entry's file_size and first cluster after a write.
// This is called from fat32_close for modified files.
static int fat32_flush_dirent(const char* path, vfs_file_t* file)
{
    uint32_t dir_cluster, sector_off, entry_idx;
    fat32_dirent_t ent;

    int ret = path_resolve(path, &ent, &dir_cluster, &sector_off, &entry_idx);
    if (ret != VFS_EOK)
        return ret;

    ent.file_size  = file->size;
    ent.cluster_hi = (uint16_t)(file->first_cluster >> 16);
    ent.cluster_lo = (uint16_t)(file->first_cluster & 0xFFFF);

    return dir_write_entry(dir_cluster, sector_off, entry_idx, &ent);
}

static int fat32_close(vfs_file_t* file)
{
    fat32_file_state_t* state = (fat32_file_state_t*)file->fs_data;
    if (!state)
        return VFS_EOK;  // root directory — nothing to flush or free

    // Flush size and first_cluster back to the directory entry
    if (!file->is_dir && state->path)
        fat32_flush_dirent(state->path, file);

    // Free the state struct and path string
    if (state->path)
        kfree(state->path);
    kfree(state);
    file->fs_data = NULL;

    return VFS_EOK;
}

// ---------------------------------------------------------------------------
// LFN cleanup helper
// ---------------------------------------------------------------------------

// The FAT spec stores LFN entries in the slots immediately before their 8.3
// entry.  When we delete a file or directory we must mark those slots 0xE5
// too; otherwise external tools see orphaned LFN entries and wasted space
// accumulates.
//
// Strategy: re-walk the directory from the beginning, recording the physical
// position of every LFN slot we accumulate.  When we reach the 8.3 entry
// that lives at (target_cluster, target_sector, target_entry_idx), mark each
// recorded LFN slot deleted and flush its sector.
//
// We track up to 20 LFN slots (the FAT32 maximum for a 255-char name) in a
// small stack-allocated array.  Each slot is identified by (cluster, sector,
// entry-within-sector) so we can flush it independently.

#define MAX_LFN_SLOTS 20

typedef struct {
    uint32_t cluster;
    uint32_t sector;   // sector offset within cluster
    uint32_t entry;    // entry index within sector
} lfn_slot_t;

// Delete all LFN entries that precede the 8.3 entry at
// (target_cluster, target_sector_off, target_entry_idx) in dir_cluster.
// Silently succeeds if there are no LFN entries (pure 8.3 name).
static void dir_delete_lfn_entries(uint32_t dir_cluster,
                                    uint32_t target_cluster,
                                    uint32_t target_sector_off,
                                    uint32_t target_entry_idx)
{
    lfn_slot_t slots[MAX_LFN_SLOTS];
    int        nslots = 0;

    uint32_t cluster = dir_cluster;
    while (cluster < FAT32_EOC) {
        uint32_t lba = cluster_to_lba(cluster);

        for (uint32_t s = 0; s < vol.sectors_per_cluster; s++) {
            if (ata_read_sectors(vol.drive, lba + s, 1, sector_buf) != 0)
                return;  // I/O error — best effort, don't corrupt further

            fat32_dirent_t* entries = (fat32_dirent_t*)sector_buf;
            uint32_t eps = 512 / sizeof(fat32_dirent_t);

            for (uint32_t e = 0; e < eps; e++) {
                fat32_dirent_t* ent = &entries[e];

                if (ent->name[0] == 0x00)
                    return;  // end of directory — target not found

                if (ent->name[0] == 0xE5) {
                    // Deleted entry resets any pending LFN run.
                    nslots = 0;
                    continue;
                }

                if (ent->attributes == FAT_ATTR_LFN) {
                    fat32_lfn_t* lfn = (fat32_lfn_t*)ent;
                    if (lfn->seq & 0x40) {
                        // First LFN entry on disk (last in logical order):
                        // start a fresh accumulator for this entry set.
                        nslots = 0;
                    }
                    if (nslots < MAX_LFN_SLOTS) {
                        slots[nslots].cluster = cluster;
                        slots[nslots].sector  = s;
                        slots[nslots].entry   = e;
                        nslots++;
                    }
                    continue;
                }

                if (ent->attributes & FAT_ATTR_VOLUME_ID) {
                    nslots = 0;
                    continue;
                }

                // Regular 8.3 entry — check if this is our target.
                if (cluster == target_cluster &&
                    s       == target_sector_off &&
                    e       == target_entry_idx) {
                    // Found it.  Mark all accumulated LFN slots deleted.
                    for (int i = 0; i < nslots; i++) {
                        uint32_t slba = cluster_to_lba(slots[i].cluster)
                                        + slots[i].sector;
                        // Re-read the sector so we have the full 512 bytes.
                        if (ata_read_sectors(vol.drive, slba, 1,
                                             sector_buf) != 0)
                            continue;  // best effort
                        fat32_dirent_t* se =
                            (fat32_dirent_t*)sector_buf;
                        se[slots[i].entry].name[0] = 0xE5;
                        ata_write_sectors(vol.drive, slba, 1, sector_buf);
                    }
                    return;
                }

                // Any other 8.3 entry resets the accumulator.
                nslots = 0;
            }
        }

        cluster = fat_read(cluster);
    }
}

static int fat32_unlink(const char* path)
{
    uint32_t dir_cluster, sector_off, entry_idx;
    fat32_dirent_t ent;

    int ret = path_resolve(path, &ent, &dir_cluster, &sector_off, &entry_idx);
    if (ret != VFS_EOK)
        return ret;

    if (ent.attributes & FAT_ATTR_DIRECTORY)
        return VFS_EISDIR;

    // Guard against 8.3 collision: refuse to delete a file whose on-disk
    // name differs from the requested name (e.g. don't let
    // unlink("Hello.txt") silently delete "hello.txt").
    {
        char existing_name[13];
        char requested_name[VFS_MAX_NAME];
        from_83(ent.name, ent.ext, existing_name);
        path_basename(path, requested_name);
        if (name_collides_83(requested_name, existing_name))
            return VFS_ENOENT;  // treat colliding-but-different name as not found
    }

    // Free cluster chain
    uint32_t first = ((uint32_t)ent.cluster_hi << 16) | ent.cluster_lo;
    if (first >= 2)
        fat_free_chain(first);

    // FIX #3: delete any LFN entries that precede this 8.3 dirent so that
    // external tools don't see orphaned long-name entries.
    dir_delete_lfn_entries(dir_cluster, dir_cluster, sector_off, entry_idx);

    // Mark directory entry as deleted (0xE5)
    uint32_t lba = cluster_to_lba(dir_cluster) + sector_off;
    if (ata_read_sectors(vol.drive, lba, 1, sector_buf) != 0)
        return VFS_EGENERIC;

    fat32_dirent_t* entries = (fat32_dirent_t*)sector_buf;
    entries[entry_idx].name[0] = 0xE5;

    if (ata_write_sectors(vol.drive, lba, 1, sector_buf) != 0)
        return VFS_EGENERIC;

    return VFS_EOK;
}

static int fat32_readdir(vfs_file_t* dir_file, uint32_t index,
                          vfs_dirent_t* out)
{
    fat32_dirent_t ent;
    char lfn[LFN_MAX_CHARS + 1];  // stack local — not static (FIX: non-reentrant hazard)

    int ret = dir_read_entry(dir_file->first_cluster, index, &ent,
                             NULL, NULL, lfn);
    if (ret != VFS_EOK)
        return ret;

    // Use LFN if present, otherwise fall back to 8.3
    if (lfn[0])
        mem_copy(out->name, lfn, LFN_MAX_CHARS + 1);
    else
        from_83(ent.name, ent.ext, out->name);

    out->is_dir  = (ent.attributes & FAT_ATTR_DIRECTORY) ? 1 : 0;
    out->size    = ent.file_size;
    out->cluster = ((uint32_t)ent.cluster_hi << 16) | ent.cluster_lo;

    return VFS_EOK;
}

static int fat32_mkdir(const char* path)
{
    uint32_t dir_cluster;
    char     name[VFS_MAX_NAME];
    uint8_t  name83[11];

    if (path_resolve_parent(path, &dir_cluster, name, name83) != VFS_EOK)
        return VFS_ENOENT;

    // Check it doesn't already exist (8.3 comparison is correct here —
    // mkdir always creates 8.3-named directories).
    fat32_dirent_t existing;
    if (dir_find(dir_cluster, name83, &existing, NULL, NULL, NULL) == VFS_EOK)
        return VFS_EEXIST;

    uint32_t new_cluster = fat_alloc_cluster();
    if (!new_cluster)
        return VFS_ENOSPC;

    // Write "." and ".." entries into the new directory cluster
    uint32_t lba = cluster_to_lba(new_cluster);
    if (ata_read_sectors(vol.drive, lba, 1, sector_buf) != 0)
        return VFS_EGENERIC;

    fat32_dirent_t* entries = (fat32_dirent_t*)sector_buf;
    mem_set(sector_buf, 0, 512);

    // "." entry
    mem_set(entries[0].name, ' ', 8);
    mem_set(entries[0].ext,  ' ', 3);
    entries[0].name[0]    = '.';
    entries[0].attributes = FAT_ATTR_DIRECTORY;
    entries[0].cluster_hi = (uint16_t)(new_cluster >> 16);
    entries[0].cluster_lo = (uint16_t)(new_cluster & 0xFFFF);

    // ".." entry
    mem_set(entries[1].name, ' ', 8);
    mem_set(entries[1].ext,  ' ', 3);
    entries[1].name[0]    = '.';
    entries[1].name[1]    = '.';
    entries[1].attributes = FAT_ATTR_DIRECTORY;
    entries[1].cluster_hi = (uint16_t)(dir_cluster >> 16);
    entries[1].cluster_lo = (uint16_t)(dir_cluster & 0xFFFF);

    // End of directory marker
    entries[2].name[0] = 0x00;

    if (ata_write_sectors(vol.drive, lba, 1, sector_buf) != 0)
        return VFS_EGENERIC;

    // Add directory entry in parent
    fat32_dirent_t new_ent;
    mem_set(&new_ent, 0, sizeof(new_ent));
    mem_copy(new_ent.name, name83, 8);
    mem_copy(new_ent.ext,  name83 + 8, 3);
    new_ent.attributes = FAT_ATTR_DIRECTORY;
    new_ent.cluster_hi = (uint16_t)(new_cluster >> 16);
    new_ent.cluster_lo = (uint16_t)(new_cluster & 0xFFFF);
    new_ent.file_size  = 0;

    return dir_add_entry(dir_cluster, &new_ent);
}

static int fat32_rmdir(const char* path)
{
    uint32_t dir_cluster, sector_off, entry_idx;
    fat32_dirent_t ent;

    int ret = path_resolve(path, &ent, &dir_cluster, &sector_off, &entry_idx);
    if (ret != VFS_EOK)
        return ret;

    if (!(ent.attributes & FAT_ATTR_DIRECTORY))
        return VFS_ENOTDIR;

    // Check empty (only "." and ".." allowed)
    uint32_t target_cluster = ((uint32_t)ent.cluster_hi << 16) | ent.cluster_lo;
    fat32_dirent_t child;
    uint32_t idx = 0;
    // FIX #6: pass NULL for the missing lfn_out argument
    while (dir_read_entry(target_cluster, idx, &child, NULL, NULL, NULL) == VFS_EOK) {
        char name[13];
        from_83(child.name, child.ext, name);
        if (!str_eq(name, ".") && !str_eq(name, ".."))
            return VFS_EGENERIC;  // directory not empty
        idx++;
    }

    // Free cluster chain
    fat_free_chain(target_cluster);

    // FIX #4: delete any LFN entries that precede the directory's 8.3 dirent
    // so external tools don't see orphaned long-name entries.
    dir_delete_lfn_entries(dir_cluster, dir_cluster, sector_off, entry_idx);

    // Mark as deleted
    uint32_t lba = cluster_to_lba(dir_cluster) + sector_off;
    if (ata_read_sectors(vol.drive, lba, 1, sector_buf) != 0)
        return VFS_EGENERIC;

    fat32_dirent_t* entries = (fat32_dirent_t*)sector_buf;
    entries[entry_idx].name[0] = 0xE5;

    return ata_write_sectors(vol.drive, lba, 1, sector_buf);
}

static int fat32_create(const char* path)
{
    uint32_t dir_cluster;
    char     name[VFS_MAX_NAME];
    uint8_t  name83[11];

    if (path_resolve_parent(path, &dir_cluster, name, name83) != VFS_EOK)
        return VFS_ENOENT;

    // dir_find returns a match for any file whose 8.3 name collides, whether
    // it is an exact match or a different name that folds to the same 8.3
    // representation (e.g. "HellO.txt" vs "hello.txt").  Both are errors.
    fat32_dirent_t existing;
    if (dir_find(dir_cluster, name83, &existing, NULL, NULL, NULL) == VFS_EOK) {
        char existing_name[13];
        char requested_name[VFS_MAX_NAME];
        from_83(existing.name, existing.ext, existing_name);
        path_basename(path, requested_name);
        if (name_collides_83(requested_name, existing_name))
            return VFS_EEXIST;  // collision with a differently-named file
        return VFS_EEXIST;      // exact match — file already exists
    }

    // FIX #11: don't pre-allocate a cluster; store cluster=0 for empty files.
    fat32_dirent_t new_ent;
    mem_set(&new_ent, 0, sizeof(new_ent));
    mem_copy(new_ent.name, name83, 8);
    mem_copy(new_ent.ext,  name83 + 8, 3);
    new_ent.attributes = FAT_ATTR_ARCHIVE;
    new_ent.cluster_hi = 0;
    new_ent.cluster_lo = 0;
    new_ent.file_size  = 0;

    return dir_add_entry(dir_cluster, &new_ent);
}

// ---------------------------------------------------------------------------
// Rename / move
// ---------------------------------------------------------------------------
//
// fat32_rename(src, dst) is an atomic in-place dirent manipulation:
//
//   Same-directory rename:
//     Update the 8.3 name fields in the existing dirent.  Wipe any preceding
//     LFN entries (we only write 8.3 names, so keeping stale LFN data would
//     cause dir_find_lfn to match the old long name after the rename).
//
//   Cross-directory move:
//     Remove the dirent from the source parent and add a new dirent with the
//     destination name to the destination parent.  The cluster chain is NOT
//     copied — only the dirent pointer changes.  If the moved entry is a
//     directory its ".." entry is updated to point at the new parent.
//
//   dst already exists as a file:
//     Its cluster chain is freed and its dirent deleted, then the rename
//     proceeds.  (Equivalent to POSIX rename semantics.)
//
//   dst already exists as a directory:
//     Allowed only if src is also a directory AND dst is empty.  dst is
//     removed (rmdir) and then the rename proceeds.
//
// Limitations:
//   - 8.3 names only for the destination (same as create/mkdir).
//   - Does not write LFN entries for the new name.
//   - src and dst must be on the same volume (the only volume we have).

static int fat32_rename(const char* src, const char* dst)
{
    // ---- Resolve source -------------------------------------------------------
    fat32_dirent_t src_ent;
    uint32_t src_dir_cluster, src_sector_off, src_entry_idx;

    int ret = path_resolve(src, &src_ent,
                           &src_dir_cluster, &src_sector_off, &src_entry_idx);
    if (ret != VFS_EOK)
        return ret;

    uint32_t src_data_cluster =
        ((uint32_t)src_ent.cluster_hi << 16) | src_ent.cluster_lo;
    int src_is_dir = (src_ent.attributes & FAT_ATTR_DIRECTORY) ? 1 : 0;

    // ---- Resolve destination parent + leaf name ------------------------------
    uint32_t dst_dir_cluster;
    char     dst_name[VFS_MAX_NAME];
    uint8_t  dst_name83[11];

    if (path_resolve_parent(dst, &dst_dir_cluster, dst_name, dst_name83)
            != VFS_EOK)
        return VFS_ENOENT;

    // ---- Handle existing dst -------------------------------------------------
    fat32_dirent_t dst_ent;
    uint32_t dst_ent_cluster, dst_ent_sector, dst_ent_idx;

    int dst_exists = (dir_find_lfn(dst_dir_cluster, dst_name, &dst_ent,
                                   &dst_ent_cluster, &dst_ent_sector,
                                   &dst_ent_idx) == VFS_EOK);
    if (dst_exists) {
        int dst_is_dir = (dst_ent.attributes & FAT_ATTR_DIRECTORY) ? 1 : 0;

        if (dst_is_dir && !src_is_dir)
            return VFS_EISDIR;   // can't overwrite a directory with a file
        if (!dst_is_dir && src_is_dir)
            return VFS_ENOTDIR;  // can't overwrite a file with a directory

        if (dst_is_dir) {
            // dst must be empty (only "." and ".." allowed)
            uint32_t dst_data =
                ((uint32_t)dst_ent.cluster_hi << 16) | dst_ent.cluster_lo;
            fat32_dirent_t child;
            uint32_t cidx = 0;
            while (dir_read_entry(dst_data, cidx, &child,
                                  NULL, NULL, NULL) == VFS_EOK) {
                char cname[13];
                from_83(child.name, child.ext, cname);
                if (!str_eq(cname, ".") && !str_eq(cname, ".."))
                    return VFS_EGENERIC;  // dst directory not empty
                cidx++;
            }
            fat_free_chain(dst_data);
        } else {
            // dst is a regular file — free its data
            uint32_t dst_data =
                ((uint32_t)dst_ent.cluster_hi << 16) | dst_ent.cluster_lo;
            if (dst_data >= 2)
                fat_free_chain(dst_data);
        }

        // Delete dst's LFN entries then its 8.3 dirent
        dir_delete_lfn_entries(dst_dir_cluster,
                               dst_ent_cluster, dst_ent_sector, dst_ent_idx);

        uint32_t dst_lba = cluster_to_lba(dst_ent_cluster) + dst_ent_sector;
        if (ata_read_sectors(vol.drive, dst_lba, 1, sector_buf) != 0)
            return VFS_EGENERIC;
        ((fat32_dirent_t*)sector_buf)[dst_ent_idx].name[0] = 0xE5;
        if (ata_write_sectors(vol.drive, dst_lba, 1, sector_buf) != 0)
            return VFS_EGENERIC;
    }

    // ---- Build the new dirent ------------------------------------------------
    fat32_dirent_t new_ent = src_ent;       // preserve all timestamps + attrs
    mem_copy(new_ent.name, dst_name83,     8);
    mem_copy(new_ent.ext,  dst_name83 + 8, 3);

    // ---- Same directory: update name in place --------------------------------
    if (src_dir_cluster == dst_dir_cluster) {
        // Wipe any LFN entries that precede the source 8.3 dirent, since the
        // old long name no longer matches the new 8.3 name.
        dir_delete_lfn_entries(src_dir_cluster,
                               src_dir_cluster, src_sector_off, src_entry_idx);

        // Overwrite the 8.3 name fields in the existing dirent.
        uint32_t lba = cluster_to_lba(src_dir_cluster) + src_sector_off;
        if (ata_read_sectors(vol.drive, lba, 1, sector_buf) != 0)
            return VFS_EGENERIC;
        fat32_dirent_t* ents = (fat32_dirent_t*)sector_buf;
        mem_copy(ents[src_entry_idx].name, dst_name83,     8);
        mem_copy(ents[src_entry_idx].ext,  dst_name83 + 8, 3);
        return ata_write_sectors(vol.drive, lba, 1, sector_buf);
    }

    // ---- Cross-directory: add new dirent in dst, delete old in src -----------

    // Add the entry to the destination directory first so that even if the
    // delete step fails we don't lose the file entirely.
    ret = dir_add_entry(dst_dir_cluster, &new_ent);
    if (ret != VFS_EOK)
        return ret;

    // If src is a directory, update its ".." entry to point at the new parent.
    if (src_is_dir && src_data_cluster >= 2) {
        uint32_t dot_lba = cluster_to_lba(src_data_cluster);
        if (ata_read_sectors(vol.drive, dot_lba, 1, sector_buf) != 0)
            return VFS_EGENERIC;
        fat32_dirent_t* ents = (fat32_dirent_t*)sector_buf;
        // Entry 1 is ".."
        ents[1].cluster_hi = (uint16_t)(dst_dir_cluster >> 16);
        ents[1].cluster_lo = (uint16_t)(dst_dir_cluster & 0xFFFF);
        if (ata_write_sectors(vol.drive, dot_lba, 1, sector_buf) != 0)
            return VFS_EGENERIC;
    }

    // Delete LFN entries then the source 8.3 dirent.
    dir_delete_lfn_entries(src_dir_cluster,
                           src_dir_cluster, src_sector_off, src_entry_idx);

    uint32_t src_lba = cluster_to_lba(src_dir_cluster) + src_sector_off;
    if (ata_read_sectors(vol.drive, src_lba, 1, sector_buf) != 0)
        return VFS_EGENERIC;
    ((fat32_dirent_t*)sector_buf)[src_entry_idx].name[0] = 0xE5;
    return ata_write_sectors(vol.drive, src_lba, 1, sector_buf);
}

// ---------------------------------------------------------------------------
// Initialisation
// ---------------------------------------------------------------------------

int fat32_init(uint8_t drive_index, vfs_ops_t* ops)
{
    const ata_drive_t* drive = ata_get_drive(drive_index);
    if (!drive) {
        klog_warn("FAT32: drive %u not present", drive_index);
        return -1;
    }

    // Read MBR (LBA 0)
    uint8_t mbr[512];
    if (ata_read_sectors(drive_index, 0, 1, mbr) != 0) {
        klog_warn("FAT32: failed to read MBR from drive %u", drive_index);
        return -1;
    }

    // Verify MBR signature
    if (mbr[510] != 0x55 || mbr[511] != 0xAA) {
        klog_warn("FAT32: invalid MBR signature on drive %u", drive_index);
        return -1;
    }

    // Read partition 0 entry (offset 446)
    mbr_partition_t* part = (mbr_partition_t*)(mbr + 446);
    if (part->lba_start == 0) {
        klog_warn("FAT32: no partition found on drive %u", drive_index);
        return -1;
    }

    uint32_t partition_lba = part->lba_start;

    // Read VBR (Volume Boot Record = first sector of partition)
    uint8_t vbr[512];
    if (ata_read_sectors(drive_index, partition_lba, 1, vbr) != 0) {
        klog_warn("FAT32: failed to read VBR", 0);
        return -1;
    }

    fat32_bpb_t* bpb = (fat32_bpb_t*)vbr;

    // Validate FAT32
    if (bpb->bytes_per_sector != 512) {
        klog_warn("FAT32: unsupported sector size %u", bpb->bytes_per_sector);
        return -1;
    }

    // "FAT32   " check
    int is_fat32 = 1;
    const char* expected = "FAT32   ";
    for (int i = 0; i < 8; i++)
        if (bpb->fs_type[i] != (uint8_t)expected[i]) { is_fat32 = 0; break; }

    if (!is_fat32) {
        klog_warn("FAT32: partition is not FAT32 (fs_type mismatch)");
        return -1;
    }

    // Fill volume state
    vol.drive              = drive_index;
    vol.partition_lba      = partition_lba;
    vol.fat_lba            = partition_lba + bpb->reserved_sectors;
    vol.fat_size_sectors   = bpb->fat_size_32;
    vol.fat_count          = bpb->fat_count;
    vol.sectors_per_cluster= bpb->sectors_per_cluster;
    vol.bytes_per_cluster  = bpb->sectors_per_cluster * 512;
    vol.root_cluster       = bpb->root_cluster;
    vol.data_lba           = vol.fat_lba + (uint32_t)bpb->fat_count * bpb->fat_size_32;

    // FIX #9: derive max_cluster from BPB instead of using a hardcoded 65536.
    // This allows the driver to address the full volume regardless of size.
    uint32_t total_sectors = bpb->total_sectors_32
                             ? bpb->total_sectors_32
                             : bpb->total_sectors_16;
    uint32_t data_sectors  = total_sectors - (vol.data_lba - partition_lba);
    vol.max_cluster        = (data_sectors / bpb->sectors_per_cluster) + 2;

    klog_info("FAT32: mounted drive %u - root_cluster=%u spc=%u bpc=%u",
        drive_index, vol.root_cluster,
        vol.sectors_per_cluster, vol.bytes_per_cluster);
    klog_info("FAT32: partition_lba=%u fat_lba=%u data_lba=%u max_cluster=%u",
        vol.partition_lba, vol.fat_lba, vol.data_lba, vol.max_cluster);

    // Fill vtable
    ops->open    = fat32_open;
    ops->read    = fat32_read;
    ops->write   = fat32_write;
    ops->close   = fat32_close;
    ops->unlink  = fat32_unlink;
    ops->readdir = fat32_readdir;
    ops->mkdir   = fat32_mkdir;
    ops->rmdir   = fat32_rmdir;
    ops->create  = fat32_create;
    ops->rename  = fat32_rename;

    return 0;
}