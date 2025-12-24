CROSS_PREFIX=/opt/cross64/bin/
CC=$(CROSS_PREFIX)x86_64-elf-gcc
CFLAGS=-ffreestanding -O2 -Wall -Wextra -nostdlib -Iinc -mno-red-zone -mcmodel=large -msse -msse2
NASM=nasm
NASMFLAGS=-f elf64
LD=$(CROSS_PREFIX)x86_64-elf-ld
LDFLAGS=-T configs/linker.ld -nostdlib

SRC_DIR=src
BOOT_DIR=boot

C_FILES=$(shell find $(SRC_DIR) -name '*.c')
ASM_FILES=$(shell find $(SRC_DIR) -name '*.asm') \
          $(shell find $(BOOT_DIR) -name '*.asm')

OBJ_FILES=$(C_FILES:.c=.o) $(ASM_FILES:.asm=.o)

BUILD_DIR=build
ISO_DIR=iso
ISO_BOOT_DIR=$(ISO_DIR)/boot
ISO_BOOT_GRUB_DIR=$(ISO_BOOT_DIR)/grub

KERNEL_IMAGE=$(BUILD_DIR)/kernel.bin
ISO_IMAGE=MyOS.iso

all: $(ISO_IMAGE)

$(ISO_IMAGE): $(KERNEL_IMAGE)
	mkdir -p $(ISO_BOOT_GRUB_DIR)
	cp $(KERNEL_IMAGE) $(ISO_BOOT_DIR)/kernel.bin
	echo 'set timeout=0' > $(ISO_BOOT_GRUB_DIR)/grub.cfg
	echo 'set default=0' >> $(ISO_BOOT_GRUB_DIR)/grub.cfg
	echo '' >> $(ISO_BOOT_GRUB_DIR)/grub.cfg
	echo 'menuentry "MyOS" {' >> $(ISO_BOOT_GRUB_DIR)/grub.cfg
	echo '    multiboot2 /boot/kernel.bin' >> $(ISO_BOOT_GRUB_DIR)/grub.cfg
	echo '    boot' >> $(ISO_BOOT_GRUB_DIR)/grub.cfg
	echo '}' >> $(ISO_BOOT_GRUB_DIR)/grub.cfg
	grub-mkrescue -o $(ISO_IMAGE) $(ISO_DIR)

$(KERNEL_IMAGE): $(OBJ_FILES)
	mkdir -p $(BUILD_DIR)
	$(LD) $(LDFLAGS) -o $@ $^

%.o: %.c
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.asm
	mkdir -p $(dir $@)
	$(NASM) $(NASMFLAGS) $< -o $@

clean:
	rm -rf $(BUILD_DIR) $(ISO_DIR) $(ISO_IMAGE)
	find $(SRC_DIR) $(BOOT_DIR) -name '*.o' -delete

run: $(ISO_IMAGE)
	qemu-system-x86_64 -M q35 -cdrom $(ISO_IMAGE) -debugcon stdio -m 4G --enable-kvm

debug: $(ISO_IMAGE)
	qemu-system-x86_64 -M q35 -cdrom $(ISO_IMAGE) -debugcon stdio -m 1G -s -S

.PHONY: all clean run debug