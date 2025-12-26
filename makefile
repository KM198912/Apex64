CROSS_PREFIX=/opt/cross64/bin/
CC=$(CROSS_PREFIX)x86_64-elf-gcc
CFLAGS=-ffreestanding -O2 -Wall -Wextra -nostdlib -Iinc -mno-red-zone -mcmodel=large -msse -msse2
DEPFLAGS=-MMD -MP
NASM=nasm
NASMFLAGS=-f elf64
LD=$(CROSS_PREFIX)x86_64-elf-ld
LDFLAGS=-T configs/linker.ld -nostdlib

SRC_DIR=src
BOOT_DIR=boot
OUT_DIR=out

C_FILES=$(shell find $(SRC_DIR) -name '*.c')
ASM_FILES=$(shell find $(SRC_DIR) -name '*.asm') \
          $(shell find $(BOOT_DIR) -name '*.asm')

# Map source files to object files in OUT_DIR, preserving directory structure
C_OBJ_FILES=$(patsubst %.c,$(OUT_DIR)/%.o,$(C_FILES))
ASM_OBJ_FILES=$(patsubst %.asm,$(OUT_DIR)/%.o,$(ASM_FILES))
OBJ_FILES=$(C_OBJ_FILES) $(ASM_OBJ_FILES)

# Dependency files
DEP_FILES=$(C_OBJ_FILES:.o=.d)

BUILD_DIR=build
ISO_DIR=iso
ISO_BOOT_DIR=$(ISO_DIR)/boot
ISO_BOOT_GRUB_DIR=$(ISO_BOOT_DIR)/grub
KERNEL_IMAGE=$(BUILD_DIR)/kernel.bin
ISO_IMAGE=TitanOS.iso

all: $(ISO_IMAGE)

$(ISO_IMAGE): $(KERNEL_IMAGE)
	mkdir -p $(ISO_BOOT_GRUB_DIR)
	cp $(KERNEL_IMAGE) $(ISO_BOOT_DIR)/kernel.bin
	cp initrd.img $(ISO_BOOT_DIR)/initrd.img
	echo 'set timeout=0' > $(ISO_BOOT_GRUB_DIR)/grub.cfg
	echo 'set default=0' >> $(ISO_BOOT_GRUB_DIR)/grub.cfg
	echo '' >> $(ISO_BOOT_GRUB_DIR)/grub.cfg
	echo 'menuentry "MyOS" {' >> $(ISO_BOOT_GRUB_DIR)/grub.cfg
	echo '    multiboot2 /boot/kernel.bin root=/dev/sda1 loglevel=3' >> $(ISO_BOOT_GRUB_DIR)/grub.cfg
	echo '    module2 /boot/initrd.img' >> $(ISO_BOOT_GRUB_DIR)/grub.cfg
	echo '    boot' >> $(ISO_BOOT_GRUB_DIR)/grub.cfg
	echo '}' >> $(ISO_BOOT_GRUB_DIR)/grub.cfg
	grub-mkrescue -o $(ISO_IMAGE) $(ISO_DIR) -V "TitanOS"

$(KERNEL_IMAGE): $(OBJ_FILES)
	mkdir -p $(BUILD_DIR)
	$(LD) $(LDFLAGS) -o $@ $^

# Compile C files to out directory with dependency generation
$(OUT_DIR)/%.o: %.c
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

# Compile ASM files to out directory
$(OUT_DIR)/%.o: %.asm
	mkdir -p $(dir $@)
	$(NASM) $(NASMFLAGS) $< -o $@

clean:
	rm -rf $(BUILD_DIR) $(ISO_DIR) $(OUT_DIR) $(ISO_IMAGE) $(shell find . -name '*.o')

run: $(ISO_IMAGE)
	qemu-system-x86_64 -M q35 -debugcon stdio -m 4G --enable-kvm -cpu host -rtc base=localtime,clock=host \
		-drive id=disk,file=disk.img,if=none,format=raw \
		-device ahci,id=ahci \
		-device ide-hd,drive=disk,bus=ahci.0 \
		-device ide-cd,drive=cdrom,bus=ahci.1 \
		-drive id=cdrom,file=$(ISO_IMAGE),if=none,media=cdrom -boot d

debug: $(ISO_IMAGE)
	qemu-system-x86_64 -M q35 -cdrom $(ISO_IMAGE) -debugcon stdio -m 1G -s -S --enable-kvm

run-legacy: $(ISO_IMAGE)
	qemu-system-x86_64 -M pc -cdrom $(ISO_IMAGE) -debugcon stdio -m 4G --enable-kvm -cpu host -rtc base=localtime,clock=host \
		-drive file=disk.img,format=raw,if=ide,index=0,media=disk -boot d

ustar:
	@if command -v pax >/dev/null 2>&1; then \
		echo "Creating initrd.img using pax..."; \
		(cd ramdisk && pax -w -x ustar -f ../initrd.img .); \
	else \
		echo "Creating initrd.img using tar..."; \
		tar --format=ustar -cf initrd.img -C ramdisk .; \
	fi

# Include dependency files (ignore errors if they don't exist yet)
-include $(DEP_FILES)

.PHONY: all clean run debug run-legacy ustar