#!/bin/bash
# mkdisk.sh — create (or update) disk.img with user ELF programs.
#
# Requires: mtools (mformat, mcopy) or a loop-mount capable Linux system.
# Run from the repo root: bash mkdisk.sh
#
# The image is a 32 MiB raw FAT32 disk with a single partition.
# QEMU is invoked with: -drive file=disk.img,format=raw

set -e

IMG=disk.img
IMG_SIZE_MB=32
MOUNT=/tmp/myos_disk

# ---- Build user programs first ---------------------------------------------
echo "==> Building user programs..."
(cd user && make)

# ---- Create disk image if it doesn't exist ---------------------------------
if [ ! -f "$IMG" ]; then
    echo "==> Creating $IMG (${IMG_SIZE_MB} MiB, FAT32)..."

    # Allocate raw image
    dd if=/dev/zero of="$IMG" bs=1M count=$IMG_SIZE_MB status=none

    # Write a partition table: one primary FAT32 partition covering the whole disk.
    # We use sector offsets that leave 1 MiB for an MBR + alignment.
    # parted is most portable for this.
    parted -s "$IMG" \
        mklabel msdos \
        mkpart primary fat32 1MiB 100%

    # Format the partition inside the image.
    # mkfs.fat needs the partition start offset.
    PART_START=$(parted -s "$IMG" unit B print | awk '/^ 1/{print $2}' | tr -d B)
    PART_SIZE=$(parted -s "$IMG" unit B print  | awk '/^ 1/{print $4}' | tr -d B)

    # Use a loop device to format just the partition.
    LOOP=$(sudo losetup --offset "$PART_START" --sizelimit "$PART_SIZE" \
               --find --show "$IMG")
    sudo mkfs.fat -F 32 "$LOOP"
    sudo losetup -d "$LOOP"

    echo "==> Disk image created."
fi

# ---- Copy ELF files onto the FAT32 partition -------------------------------
echo "==> Copying user programs to $IMG..."

PART_START=$(parted -s "$IMG" unit B print | awk '/^ 1/{print $2}' | tr -d B)
PART_SIZE=$(parted -s "$IMG" unit B print  | awk '/^ 1/{print $4}' | tr -d B)

mkdir -p "$MOUNT"
LOOP=$(sudo losetup --offset "$PART_START" --sizelimit "$PART_SIZE" \
           --find --show "$IMG")
sudo mount "$LOOP" "$MOUNT"

for elf in user/*.elf; do
    sudo cp "$elf" "$MOUNT/$(basename $elf)"
    echo "    copied $elf -> /$( basename $elf)"
done

sudo umount "$MOUNT"
sudo losetup -d "$LOOP"

echo "==> Done. Run: make run"