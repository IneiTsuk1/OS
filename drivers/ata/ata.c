#include "ata.h"
#include "../../kernel/klog.h"
#include "../../kernel/panic.h"
#include <stdint.h>
#include <stddef.h>

// ---------------------------------------------------------------------------
// ATA PIO — register map
//
// Primary channel base:   0x1F0 (command), 0x3F6 (control)
// Secondary channel base: 0x170 (command), 0x376 (control)
// ---------------------------------------------------------------------------

// Command block registers (base-relative)
#define ATA_REG_DATA        0x00    // R/W  16-bit data port
#define ATA_REG_ERROR       0x01    // R    error information
#define ATA_REG_FEATURES    0x01    // W    features
#define ATA_REG_SECCOUNT    0x02    // R/W  sector count
#define ATA_REG_LBA_LO      0x03    // R/W  LBA bits 0-7
#define ATA_REG_LBA_MID     0x04    // R/W  LBA bits 8-15
#define ATA_REG_LBA_HI      0x05    // R/W  LBA bits 16-23
#define ATA_REG_DRIVE       0x06    // R/W  drive select + LBA bits 24-27
#define ATA_REG_STATUS      0x07    // R    status
#define ATA_REG_CMD         0x07    // W    command

// Control block register
#define ATA_REG_ALTSTATUS   0x00    // R    alternate status (no interrupt clear)
#define ATA_REG_DEVCTRL     0x00    // W    device control

// Status register bits
#define ATA_SR_ERR   (1u << 0)      // error
#define ATA_SR_DRQ   (1u << 3)      // data request — drive ready to transfer
#define ATA_SR_SRV   (1u << 4)      // overlapped service request
#define ATA_SR_DF    (1u << 5)      // drive fault
#define ATA_SR_RDY   (1u << 6)      // drive ready
#define ATA_SR_BSY   (1u << 7)      // busy

// Commands
#define ATA_CMD_READ_PIO   0x20
#define ATA_CMD_WRITE_PIO  0x30
#define ATA_CMD_IDENTIFY   0xEC
#define ATA_CMD_FLUSH      0xE7

// Drive/head register bits
#define ATA_DRIVE_MASTER  0xA0      // 1010 0000 — LBA mode, master
#define ATA_DRIVE_SLAVE   0xB0      // 1011 0000 — LBA mode, slave

// Channel I/O base addresses
static const uint16_t channel_base[2]    = { 0x1F0, 0x170 };
static const uint16_t channel_control[2] = { 0x3F6, 0x376 };

static ata_drive_t drives[ATA_MAX_DRIVES];

// ---- port I/O helpers ------------------------------------------------------

static inline void outb(uint16_t port, uint8_t val)
{
    __asm__ volatile ("outb %0, %1" :: "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=r"(ret) : "Nd"(port));
    return ret;
}

static inline uint16_t inw(uint16_t port)
{
    uint16_t ret;
    __asm__ volatile ("inw %1, %0" : "=r"(ret) : "Nd"(port));
    return ret;
}

static inline void outw(uint16_t port, uint16_t val)
{
    __asm__ volatile ("outw %0, %1" :: "a"(val), "Nd"(port));
}

// 400ns delay — read alternate status 4 times (each read ~100ns on real HW;
// on QEMU it is essentially instant but we keep it for correctness)
static void ata_delay(uint8_t channel)
{
    for (int i = 0; i < 4; i++)
        inb(channel_control[channel] + ATA_REG_ALTSTATUS);
}

// ---- polling ---------------------------------------------------------------

// Wait until BSY clears.  Returns 0 on success, -1 on timeout or drive fault.
static int ata_wait_not_busy(uint8_t channel)
{
    uint32_t timeout = 100000;
    while (timeout--) {
        uint8_t status = inb(channel_base[channel] + ATA_REG_STATUS);
        if (!(status & ATA_SR_BSY))
            return 0;
    }
    return -1;  // timeout
}

// Wait until DRQ is set (drive has data or is ready to accept data).
// Also checks for error/fault.  Returns 0 on success, negative on error.
static int ata_wait_drq(uint8_t channel)
{
    uint32_t timeout = 100000;
    while (timeout--) {
        uint8_t status = inb(channel_base[channel] + ATA_REG_STATUS);
        if (status & ATA_SR_ERR) return -1;
        if (status & ATA_SR_DF)  return -2;
        if (status & ATA_SR_DRQ) return 0;
    }
    return -3;  // timeout
}

// ---- drive selection -------------------------------------------------------

static void ata_select_drive(uint8_t channel, uint8_t slave)
{
    uint8_t sel = slave ? ATA_DRIVE_SLAVE : ATA_DRIVE_MASTER;
    outb(channel_base[channel] + ATA_REG_DRIVE, sel);
    ata_delay(channel);
}

// ---- IDENTIFY --------------------------------------------------------------

// Issue IDENTIFY to one drive slot.  Fills `drive` on success.
// Returns 1 if a drive is present, 0 if not.
static int ata_identify(uint8_t channel, uint8_t slave, ata_drive_t* drive)
{
    ata_select_drive(channel, slave);

    // Zero sector count and LBA registers — required before IDENTIFY
    outb(channel_base[channel] + ATA_REG_SECCOUNT, 0);
    outb(channel_base[channel] + ATA_REG_LBA_LO,   0);
    outb(channel_base[channel] + ATA_REG_LBA_MID,  0);
    outb(channel_base[channel] + ATA_REG_LBA_HI,   0);

    outb(channel_base[channel] + ATA_REG_CMD, ATA_CMD_IDENTIFY);

    // If status is 0 immediately, no drive present
    uint8_t status = inb(channel_base[channel] + ATA_REG_STATUS);
    if (status == 0)
        return 0;

    // Wait for BSY to clear
    if (ata_wait_not_busy(channel) != 0)
        return 0;

    // If LBA_MID or LBA_HI are non-zero, this is not an ATA drive
    // (could be ATAPI — we skip it for now)
    uint8_t mid = inb(channel_base[channel] + ATA_REG_LBA_MID);
    uint8_t hi  = inb(channel_base[channel] + ATA_REG_LBA_HI);
    if (mid || hi)
        return 0;  // ATAPI or other non-ATA device

    // Wait for DRQ
    if (ata_wait_drq(channel) != 0)
        return 0;

    // Read 256 words of IDENTIFY data
    uint16_t identify[256];
    for (int i = 0; i < 256; i++)
        identify[i] = inw(channel_base[channel] + ATA_REG_DATA);

    // Word 60-61: 28-bit LBA sector count (word 60 = low, 61 = high)
    drive->sector_count = ((uint32_t)identify[61] << 16) | identify[60];

    // Words 27-46: model string (each word is two ASCII chars, big-endian)
    for (int i = 0; i < 20; i++) {
        drive->model[i * 2]     = (char)(identify[27 + i] >> 8);
        drive->model[i * 2 + 1] = (char)(identify[27 + i] & 0xFF);
    }
    drive->model[40] = '\0';

    // Trim trailing spaces from model string
    for (int i = 39; i >= 0 && drive->model[i] == ' '; i--)
        drive->model[i] = '\0';

    drive->present = 1;
    drive->channel = channel;
    drive->slave   = slave;

    return 1;
}

// ---- LBA28 setup -----------------------------------------------------------

static void ata_setup_lba28(uint8_t channel, uint8_t slave,
                             uint32_t lba, uint8_t count)
{
    uint8_t drive_sel = slave
        ? (0xF0 | ((lba >> 24) & 0x0F))   // slave  | LBA bits 24-27
        : (0xE0 | ((lba >> 24) & 0x0F));   // master | LBA bits 24-27

    outb(channel_base[channel] + ATA_REG_DRIVE,    drive_sel);
    outb(channel_base[channel] + ATA_REG_SECCOUNT, count);
    outb(channel_base[channel] + ATA_REG_LBA_LO,   (uint8_t) lba);
    outb(channel_base[channel] + ATA_REG_LBA_MID,  (uint8_t)(lba >>  8));
    outb(channel_base[channel] + ATA_REG_LBA_HI,   (uint8_t)(lba >> 16));
}

// ---- public API ------------------------------------------------------------

void ata_init(void)
{
    for (int i = 0; i < ATA_MAX_DRIVES; i++)
        drives[i].present = 0;

    // Disable IRQs on both channels — we use polling only
    outb(channel_control[0] + ATA_REG_DEVCTRL, 0x02);  // nIEN bit
    outb(channel_control[1] + ATA_REG_DEVCTRL, 0x02);

    uint8_t idx = 0;
    for (uint8_t ch = 0; ch < 2; ch++) {
        for (uint8_t sl = 0; sl < 2; sl++, idx++) {
            int found = ata_identify(ch, sl, &drives[idx]);
            if (found) {
                klog_info("ATA: drive %u - %s (%s %s, %u sectors = %u MiB)",
                    idx,
                    drives[idx].model,
                    ch ? "secondary" : "primary",
                    sl ? "slave" : "master",
                    drives[idx].sector_count,
                    drives[idx].sector_count / 2048);
            } else {
                klog_info("ATA: drive %u (%s %s) - not present",
                    idx,
                    ch ? "secondary" : "primary",
                    sl ? "slave" : "master");
            }
        }
    }
}

const ata_drive_t* ata_get_drive(uint8_t drive_index)
{
    if (drive_index >= ATA_MAX_DRIVES)
        return NULL;
    if (!drives[drive_index].present)
        return NULL;
    return &drives[drive_index];
}

int ata_read_sectors(uint8_t drive_index, uint32_t lba,
                     uint8_t count, void* buf)
{
    if (drive_index >= ATA_MAX_DRIVES || !drives[drive_index].present)
        return -1;
    if (count == 0)
        return 0;

    ata_drive_t* d = &drives[drive_index];
    uint16_t*    dst = (uint16_t*)buf;

    if (ata_wait_not_busy(d->channel) != 0) {
        klog_warn("ATA: read timeout (drive %u, lba %u)", drive_index, lba);
        return -1;
    }

    ata_setup_lba28(d->channel, d->slave, lba, count);
    outb(channel_base[d->channel] + ATA_REG_CMD, ATA_CMD_READ_PIO);

    for (uint8_t s = 0; s < count; s++) {
        ata_delay(d->channel);

        if (ata_wait_not_busy(d->channel) != 0) {
            klog_warn("ATA: read BSY timeout sector %u", s);
            return -1;
        }

        if (ata_wait_drq(d->channel) != 0) {
            klog_warn("ATA: read DRQ timeout sector %u", s);
            return -1;
        }

        // Read 256 words = 512 bytes = one sector
        for (int i = 0; i < 256; i++)
            *dst++ = inw(channel_base[d->channel] + ATA_REG_DATA);
    }

    return 0;
}

int ata_write_sectors(uint8_t drive_index, uint32_t lba,
                      uint8_t count, const void* buf)
{
    if (drive_index >= ATA_MAX_DRIVES || !drives[drive_index].present)
        return -1;
    if (count == 0)
        return 0;

    ata_drive_t*     d   = &drives[drive_index];
    const uint16_t*  src = (const uint16_t*)buf;

    if (ata_wait_not_busy(d->channel) != 0) {
        klog_warn("ATA: write timeout (drive %u, lba %u)", drive_index, lba);
        return -1;
    }

    ata_setup_lba28(d->channel, d->slave, lba, count);
    outb(channel_base[d->channel] + ATA_REG_CMD, ATA_CMD_WRITE_PIO);

    for (uint8_t s = 0; s < count; s++) {
        ata_delay(d->channel);

        if (ata_wait_not_busy(d->channel) != 0) {
            klog_warn("ATA: write BSY timeout sector %u", s);
            return -1;
        }

        if (ata_wait_drq(d->channel) != 0) {
            klog_warn("ATA: write DRQ timeout sector %u", s);
            return -1;
        }

        // Write 256 words = 512 bytes = one sector
        for (int i = 0; i < 256; i++)
            outw(channel_base[d->channel] + ATA_REG_DATA, *src++);
    }

    // Flush write cache
    outb(channel_base[d->channel] + ATA_REG_CMD, ATA_CMD_FLUSH);
    ata_wait_not_busy(d->channel);

    return 0;
}