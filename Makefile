# Toolchain
CC = gcc
AS = nasm
LD = ld

# Directories
BOOT_DIR = boot
KERNEL_DIR = kernel
DRIVERS_DIR = drivers
BUILD_DIR = build
ISO_DIR = iso

# Output
KERNEL = kernel.elf

# Find all source files automatically
C_SRC  := $(shell find $(KERNEL_DIR) $(DRIVERS_DIR) -name "*.c" 2>/dev/null)
ASM_SRC := $(shell find $(BOOT_DIR) -name "*.asm" 2>/dev/null)

# Convert to object files in build/
C_OBJ  := $(patsubst %.c,$(BUILD_DIR)/%.o,$(C_SRC))
ASM_OBJ := $(patsubst %.asm,$(BUILD_DIR)/%.o,$(ASM_SRC))

# Flags
CFLAGS = -m32 -ffreestanding -nostdlib -Wall -Wextra -Iinclude
ASFLAGS = -f elf32
LDFLAGS = -m elf_i386 -T linker.ld

# Default target
all: $(KERNEL)

# Ensure build dirs exist
$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

# Compile C files (auto directory creation)
$(BUILD_DIR)/%.o: %.c | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# Assemble ASM files
$(BUILD_DIR)/%.o: %.asm | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) $< -o $@

# Link kernel
$(KERNEL): $(C_OBJ) $(ASM_OBJ)
	$(LD) $(LDFLAGS) $^ -o $@

# ISO creation
iso: $(KERNEL)
	mkdir -p $(ISO_DIR)/boot/grub
	cp $(KERNEL) $(ISO_DIR)/boot/kernel.elf
	cp $(BOOT_DIR)/grub/grub.cfg $(ISO_DIR)/boot/grub/grub.cfg
	grub-mkrescue -o myos.iso $(ISO_DIR)

# Run in QEMU
run: iso
	qemu-system-i386 -cdrom myos.iso \
	    -drive file=disk.img,format=raw \
	    -boot order=d \
	    -serial stdio

# Clean
clean:
	rm -rf $(BUILD_DIR)
	rm -rf $(ISO_DIR)
	rm -f $(KERNEL)
	rm -f myos.iso

.PHONY: all iso run clean