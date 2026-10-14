# WindClone — build system
#
# The ISO build targets Linux/WSL/Docker (gcc + nasm + xorriso + qemu).
# On Windows, run `wsl` or use tools/fetch-limine.ps1 + WSL make.
#
#   make            -> windclone.iso (bootable on BIOS + UEFI machines)
#   make run        -> boot the ISO in QEMU (with serial console)
#   make mbr        -> educational 512-byte MBR image (build/mbr.bin)
#   make run-mbr    -> boot the MBR image in QEMU
#   make clean      -> remove build artifacts
#
# SPDX-License-Identifier: MIT

CC      ?= gcc
NASM    ?= nasm
XORRISO ?= xorriso
QEMU    ?= qemu-system-x86_64

KERNEL_SRC := $(wildcard src/kernel/*.c)
KERNEL_OBJ := $(patsubst src/kernel/%.c,build/%.o,$(KERNEL_SRC))

CFLAGS  := -std=gnu11 -O2 -Wall -Wextra -Werror \
           -ffreestanding -fno-pic -fno-pie -no-pie \
           -mno-red-zone -mcmodel=kernel \
           -fno-stack-protector -fno-asynchronous-unwind-tables \
           -fno-builtin -Ivendor -Isrc/kernel
LDFLAGS := -T linkers/kernel.ld -nostdlib -static -no-pie \
           -z max-page-size=0x1000 -Wl,--build-id=none

.PHONY: all kernel iso run mbr run-mbr clean fetch-limine

all: iso

# ---- kernel ---------------------------------------------------------

build/%.o: src/kernel/%.c
	@mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@

build/kernel.elf: $(KERNEL_OBJ) linkers/kernel.ld
	$(CC) $(LDFLAGS) -o $@ $(KERNEL_OBJ)

kernel: build/kernel.elf

# ---- bootable ISO (Limine BIOS + UEFI) -------------------------------

iso: windclone.iso

windclone.iso: build/kernel.elf limine.cfg \
               limine/limine-bios-cd.bin limine/limine-uefi-cd.bin \
               limine/limine-bios.sys \
               limine/BOOTX64.EFI limine/BOOTIA32.EFI
	./tools/build-iso.sh

limine/limine-bios-cd.bin:
	./tools/fetch-limine.sh

limine/%: limine/limine-bios-cd.bin
	./tools/fetch-limine.sh

fetch-limine:
	./tools/fetch-limine.sh

# ---- educational MBR (pure assembly, not used by the ISO) ------------

build/mbr.bin: src/boot/mbr.asm
	@mkdir -p build
	$(NASM) -f bin -o $@ $<

mbr: build/mbr.bin

# 1MB sparse disk so the firmware sees a valid bootable media
build/mbr.img: build/mbr.bin
	dd if=build/mbr.bin of=$@ bs=512 count=1 2>/dev/null
	dd if=/dev/zero of=$@ bs=512 count=2047 seek=1 conv=notrunc 2>/dev/null

run-mbr: build/mbr.img
	$(QEMU) -drive format=raw,file=build/mbr.img

# ---- run -------------------------------------------------------------

run: windclone.iso
	$(QEMU) -cdrom windclone.iso -boot d -m 512 \
	       -serial stdio -device isa-debug-exit,iobase=0xf4,iosize=0x04

clean:
	rm -rf build isodir windclone.iso
