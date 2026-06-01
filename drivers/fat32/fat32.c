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
} fat32_vol_t;

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
    // Linear scan from cluster 2 — slow but simple for M7.
    // Cap the scan conservatively; a proper implementation would use
    // the FSInfo sector's free cluster hint.
    uint32_t max_cluster = 65536;

    for (uint32_t c = 2; c < max_cluster; c++) {
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
// entry index `entry_index`.  Skips LFN and deleted entries when counting.
// Returns 0 on success, VFS_ENOENT if index is past end.
static int dir_read_entry(uint32_t dir_cluster, uint32_t entry_index,
                           fat32_dirent_t* out,
                           uint32_t* out_cluster, uint32_t* out_sector_offset)
{
    uint32_t cluster = dir_cluster;
    uint32_t real_idx = 0;  // index counting only valid (non-LFN, non-deleted) entries

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
                if (ent->name[0] == 0xE5)  // deleted entry
                    continue;
                if (ent->attributes == FAT_ATTR_LFN)  // skip LFN entries
                    continue;
                if (ent->attributes & FAT_ATTR_VOLUME_ID)  // skip volume label
                    continue;

                if (real_idx == entry_index) {
                    *out = *ent;
                    if (out_cluster)       *out_cluster      = cluster;
                    if (out_sector_offset) *out_sector_offset = s;
                    return VFS_EOK;
                }
                real_idx++;
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
                    // Free slot — write here
                    entries[e] = *new_ent;
                    // If this was an end marker, add a new end marker after
                    if (first == 0x00 && (e + 1) < eps)
                        entries[e + 1].name[0] = 0x00;
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
// Returns the cluster of the parent directory and the final component name
// as an 8.3 byte array.  Returns VFS_ENOENT if any intermediate component
// is missing.
// ---------------------------------------------------------------------------

static int path_resolve_parent(const char* path,
                                uint32_t* out_dir_cluster,
                                uint8_t   out_name83[11])
{
    // Accept relative paths by treating them as rooted
    char abspath[VFS_MAX_PATH];
    if (path[0] != '/') {
        abspath[0] = '/';
        int i = 1;
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
            // This is the final component — return its 8.3 and the parent dir
            to_83(component, out_name83);
            *out_dir_cluster = dir_cluster;
            return VFS_EOK;
        }

        // Intermediate component — must be a directory
        uint8_t name83[11];
        to_83(component, name83);
        fat32_dirent_t ent;
        int ret = dir_find(dir_cluster, name83, &ent, NULL, NULL, NULL);
        if (ret != VFS_EOK)
            return VFS_ENOENT;
        if (!(ent.attributes & FAT_ATTR_DIRECTORY))
            return VFS_ENOTDIR;

        dir_cluster = ((uint32_t)ent.cluster_hi << 16) | ent.cluster_lo;
    }

    // Path ended with '/' — treat root or last dir as the target
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
    uint8_t  name83[11];

    int ret = path_resolve_parent(path, &dir_cluster, name83);
    if (ret != VFS_EOK)
        return ret;

    return dir_find(dir_cluster, name83, out,
                    out_dir_cluster, out_sector, out_entry_idx);
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
        uint8_t  name83[11];
        if (path_resolve_parent(path, &dir_cluster, name83) != VFS_EOK)
            return VFS_ENOENT;

        uint32_t new_cluster = fat_alloc_cluster();
        if (!new_cluster)
            return VFS_ENOSPC;

        fat32_dirent_t new_ent;
        mem_set(&new_ent, 0, sizeof(new_ent));
        mem_copy(new_ent.name, name83, 8);
        mem_copy(new_ent.ext,  name83 + 8, 3);
        new_ent.attributes = FAT_ATTR_ARCHIVE;
        new_ent.cluster_hi = (uint16_t)(new_cluster >> 16);
        new_ent.cluster_lo = (uint16_t)(new_cluster & 0xFFFF);
        new_ent.file_size  = 0;

        if (dir_add_entry(dir_cluster, &new_ent) != VFS_EOK)
            return VFS_EGENERIC;

        ent = new_ent;
    } else if (ret != VFS_EOK) {
        return ret;
    }

    // Store a copy of the path so fat32_close can flush the dirent.
    uint32_t plen = str_len(path) + 1;
    char* stored_path = (char*)kmalloc(plen);
    if (stored_path)
        mem_copy(stored_path, path, plen);

    out->flags         = flags;
    out->size          = ent.file_size;
    out->first_cluster = ((uint32_t)ent.cluster_hi << 16) | ent.cluster_lo;
    out->is_dir        = (ent.attributes & FAT_ATTR_DIRECTORY) ? 1 : 0;
    out->pos           = (flags & O_APPEND) ? ent.file_size : 0;
    out->fs_data       = stored_path;

    return VFS_EOK;
}

static int fat32_read(vfs_file_t* file, void* buf, uint32_t len)
{
    if (file->pos >= file->size)
        return 0;  // EOF

    if (len > file->size - file->pos)
        len = file->size - file->pos;

    uint8_t* dst      = (uint8_t*)buf;
    uint32_t cluster  = file->first_cluster;
    uint32_t remaining = len;

    // Seek to the cluster containing file->pos
    uint32_t cluster_idx = file->pos / vol.bytes_per_cluster;
    for (uint32_t i = 0; i < cluster_idx && cluster < FAT32_EOC; i++)
        cluster = fat_read(cluster);

    uint32_t offset_in_cluster = file->pos % vol.bytes_per_cluster;

    uint8_t cluster_buf[4096];  // max cluster size we support (8 sectors * 512)
    if (vol.bytes_per_cluster > sizeof(cluster_buf))
        return VFS_EGENERIC;  // cluster too large for our buffer

    while (remaining > 0 && cluster < FAT32_EOC) {
        // Read the whole cluster
        uint32_t lba = cluster_to_lba(cluster);
        for (uint32_t s = 0; s < vol.sectors_per_cluster; s++) {
            if (ata_read_sectors(vol.drive, lba + s, 1,
                                  cluster_buf + s * 512) != 0)
                return VFS_EGENERIC;
        }

        uint32_t avail = vol.bytes_per_cluster - offset_in_cluster;
        uint32_t copy  = remaining < avail ? remaining : avail;

        mem_copy(dst, cluster_buf + offset_in_cluster, copy);
        dst                  += copy;
        remaining            -= copy;
        file->pos            += copy;
        offset_in_cluster     = 0;

        if (remaining > 0)
            cluster = fat_read(cluster);
    }

    return (int)(len - remaining);
}

static int fat32_write(vfs_file_t* file, const void* buf, uint32_t len)
{
    if (len == 0) return 0;

    const uint8_t* src       = (const uint8_t*)buf;
    uint32_t       remaining = len;

    // Find or extend to the cluster containing file->pos
    uint32_t cluster      = file->first_cluster;
    uint32_t prev_cluster = 0;
    uint32_t cluster_idx  = file->pos / vol.bytes_per_cluster;

    for (uint32_t i = 0; i < cluster_idx; i++) {
        if (cluster >= FAT32_EOC) {
            // Need to extend the chain
            uint32_t new_c = fat_extend_chain(prev_cluster);
            if (!new_c)
                return VFS_ENOSPC;
            cluster = new_c;
        }
        prev_cluster = cluster;
        cluster      = fat_read(cluster);
    }

    // If cluster is EOC we need a new cluster for this write
    if (cluster >= FAT32_EOC || cluster == 0) {
        uint32_t new_c = prev_cluster
            ? fat_extend_chain(prev_cluster)
            : fat_alloc_cluster();
        if (!new_c)
            return VFS_ENOSPC;
        if (file->first_cluster == 0)
            file->first_cluster = new_c;
        cluster = new_c;
    }

    uint32_t offset_in_cluster = file->pos % vol.bytes_per_cluster;
    uint8_t  cluster_buf[4096];

    if (vol.bytes_per_cluster > sizeof(cluster_buf))
        return VFS_EGENERIC;

    while (remaining > 0) {
        // Read-modify-write the current cluster
        uint32_t lba = cluster_to_lba(cluster);
        for (uint32_t s = 0; s < vol.sectors_per_cluster; s++) {
            if (ata_read_sectors(vol.drive, lba + s, 1,
                                  cluster_buf + s * 512) != 0)
                return VFS_EGENERIC;
        }

        uint32_t avail = vol.bytes_per_cluster - offset_in_cluster;
        uint32_t copy  = remaining < avail ? remaining : avail;

        mem_copy(cluster_buf + offset_in_cluster, src, copy);

        for (uint32_t s = 0; s < vol.sectors_per_cluster; s++) {
            if (ata_write_sectors(vol.drive, lba + s, 1,
                                   cluster_buf + s * 512) != 0)
                return VFS_EGENERIC;
        }

        src                  += copy;
        remaining            -= copy;
        file->pos            += copy;
        offset_in_cluster     = 0;

        if (remaining > 0) {
            uint32_t next = fat_read(cluster);
            if (next >= FAT32_EOC) {
                next = fat_extend_chain(cluster);
                if (!next)
                    return VFS_ENOSPC;
            }
            cluster = next;
        }
    }

    if (file->pos > file->size)
        file->size = file->pos;

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
    if (!file->fs_data)
        return VFS_EOK;  // root dir or no path stored — nothing to flush

    const char* path = (const char*)file->fs_data;

    // Flush size and first_cluster back to the directory entry.
    // Only needed for writable files — directories manage their own size.
    if (!file->is_dir) {
        fat32_flush_dirent(path, file);
    }

    kfree(file->fs_data);
    file->fs_data = NULL;

    return VFS_EOK;
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

    // Free cluster chain
    uint32_t first = ((uint32_t)ent.cluster_hi << 16) | ent.cluster_lo;
    if (first >= 2)
        fat_free_chain(first);

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
    int ret = dir_read_entry(dir_file->first_cluster, index, &ent, NULL, NULL);
    if (ret != VFS_EOK)
        return ret;

    from_83(ent.name, ent.ext, out->name);
    out->is_dir  = (ent.attributes & FAT_ATTR_DIRECTORY) ? 1 : 0;
    out->size    = ent.file_size;
    out->cluster = ((uint32_t)ent.cluster_hi << 16) | ent.cluster_lo;

    return VFS_EOK;
}

static int fat32_mkdir(const char* path)
{
    uint32_t dir_cluster;
    uint8_t  name83[11];

    if (path_resolve_parent(path, &dir_cluster, name83) != VFS_EOK)
        return VFS_ENOENT;

    // Check it doesn't already exist
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
    while (dir_read_entry(target_cluster, idx, &child, NULL, NULL) == VFS_EOK) {
        char name[13];
        from_83(child.name, child.ext, name);
        if (!str_eq(name, ".") && !str_eq(name, ".."))
            return VFS_EGENERIC;  // directory not empty
        idx++;
    }

    // Free cluster chain
    fat_free_chain(target_cluster);

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
    uint8_t  name83[11];

    if (path_resolve_parent(path, &dir_cluster, name83) != VFS_EOK)
        return VFS_ENOENT;

    fat32_dirent_t existing;
    if (dir_find(dir_cluster, name83, &existing, NULL, NULL, NULL) == VFS_EOK)
        return VFS_EEXIST;

    uint32_t new_cluster = fat_alloc_cluster();
    if (!new_cluster)
        return VFS_ENOSPC;

    fat32_dirent_t new_ent;
    mem_set(&new_ent, 0, sizeof(new_ent));
    mem_copy(new_ent.name, name83, 8);
    mem_copy(new_ent.ext,  name83 + 8, 3);
    new_ent.attributes = FAT_ATTR_ARCHIVE;
    new_ent.cluster_hi = (uint16_t)(new_cluster >> 16);
    new_ent.cluster_lo = (uint16_t)(new_cluster & 0xFFFF);
    new_ent.file_size  = 0;

    return dir_add_entry(dir_cluster, &new_ent);
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

    klog_info("FAT32: mounted drive %u - root_cluster=%u spc=%u bpc=%u",
        drive_index, vol.root_cluster,
        vol.sectors_per_cluster, vol.bytes_per_cluster);
    klog_info("FAT32: partition_lba=%u fat_lba=%u data_lba=%u",
        vol.partition_lba, vol.fat_lba, vol.data_lba);

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

    return 0;
}