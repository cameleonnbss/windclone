# WindClone — build system
#
# The ISO build targets Linux/WSL/Docker (gcc + nasm + xorriso + qemu).
# On Windows, run `wsl` or use tools/fetch-limine.ps1 + WSL make.
#
#   make            -> windclone.iso (bootable on BIOS + UEFI machines)
#   make initrd     -> build/initrd.tar (the tar module packed from rootfs/)
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
KERNEL_ASM := $(wildcard src/kernel/*.asm)
KERNEL_ASM_OBJ := $(patsubst src/kernel/%.asm,build/%.asm.o,$(KERNEL_ASM))

# initrd: the bootloader loads this tar as a module; rebuild when any
# file under rootfs/ changes.
ROOTFS_FILES := $(shell find rootfs -type f 2>/dev/null)
INITRD       := build/initrd.tar

# Userland programs: flat binaries assembled at the address usermode.c
# maps them to (USER_BASE in src/kernel/usermode.h).
USER_BINS := build/hello

# -mno-sse... : keep gcc from emitting SSE/AVX. At boot time CR4/XMM state
# is not guaranteed, and -O2 happily vectorises loops (movdqa etc.) which
# faults before we even have an IDT -> triple fault. Same flags Linux uses.
CFLAGS  := -std=gnu11 -O2 -Wall -Wextra -Werror \
           -ffreestanding -fno-pic -fno-pie -no-pie \
           -mno-red-zone -mcmodel=kernel \
           -mno-sse -mno-sse2 -mno-avx -mno-mmx -mno-80387 -msoft-float \
           -fno-stack-protector -fno-asynchronous-unwind-tables \
           -fno-omit-frame-pointer \
           -fno-builtin -Ivendor -Isrc/kernel
LDFLAGS := -T linkers/kernel.ld -nostdlib -static -no-pie \
           -z max-page-size=0x1000 -Wl,--build-id=none

.PHONY: all kernel iso initrd run mbr run-mbr clean fetch-limine

all: iso

# ---- kernel ---------------------------------------------------------

build/%.o: src/kernel/%.c
	@mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@

# 64-bit assembly (interrupt stubs) linked into the kernel ELF
build/%.asm.o: src/kernel/%.asm
	@mkdir -p build
	$(NASM) -f elf64 -o $@ $<

build/kernel.elf: $(KERNEL_OBJ) $(KERNEL_ASM_OBJ) linkers/kernel.ld
	$(CC) $(LDFLAGS) -o $@ $(KERNEL_OBJ) $(KERNEL_ASM_OBJ)

kernel: build/kernel.elf

# ---- initrd (tar module, parsed in place by src/kernel/initrd.c) -----

build/hello: src/user/hello.asm
	@mkdir -p build
	$(NASM) -f bin -o $@ $<

build/initrd.tar: tools/mkinitrd.sh $(ROOTFS_FILES) $(USER_BINS)
	@mkdir -p build
	./tools/mkinitrd.sh $(INITRD)

initrd: build/initrd.tar

# ---- bootable ISO (Limine BIOS + UEFI) -------------------------------

iso: windclone.iso

windclone.iso: build/kernel.elf $(INITRD) limine.conf \
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
# stage-0 (MBR, 512 bytes at LBA0) loads stage-2 from LBA1 via BIOS
# int 13h and jumps to it: a real two-stage bootloader in ~1 KB.

build/mbr.bin: src/boot/mbr.asm
	@mkdir -p build
	$(NASM) -f bin -o $@ $<

build/stage2.bin: src/boot/stage2.asm
	@mkdir -p build
	$(NASM) -f bin -o $@ $<

mbr: build/mbr.bin build/stage2.bin

# 1MB sparse disk: mbr (LBA0) + stage2 (LBA1..) + zero padding
build/mbr.img: build/mbr.bin build/stage2.bin
	cat build/mbr.bin build/stage2.bin > $@
	truncate -s 1M $@

run-mbr: build/mbr.img
	$(QEMU) -drive format=raw,file=build/mbr.img

# ---- run -------------------------------------------------------------

run: windclone.iso
	$(QEMU) -cdrom windclone.iso -boot d -m 512 \
	       -serial stdio -device isa-debug-exit,iobase=0xf4,iosize=0x04

clean:
	rm -rf build isodir windclone.iso
