# WindClone

> An operating system built from scratch — Linux-style architecture,
> Windows-inspired look & feel, bootable ISO for any x86_64 machine.

![build status](https://github.com/cameleonnbss/windclone/actions/workflows/build.yml/badge.svg)

**WindClone** is a hobby operating system project. Nothing is copied from
Linux or Windows: the kernel is written by us in C (with assembly for the
earliest stages), and Rust / C++ are planned for drivers and userland as the
project matures. The long-term goal is a Linux-compatible system call layer
so that existing Linux binaries can eventually run on top of it, wrapped in a
desktop shell that feels familiar.

⚠️ This is a learning project. v0.0.1 boots, prints, and halts. That is the
point — every OS starts exactly there.

## What boots today

`make` produces `windclone.iso`, a **hybrid BIOS + UEFI image** that boots on:

| Target            | Status |
|-------------------|--------|
| QEMU (BIOS)       | ✅ tested in CI |
| QEMU (UEFI/OVMF)  | ✅ tested in CI |
| Real PC, legacy BIOS CD/USB | ✅ (Limine El Torito + `bios-install` MBR) |
| Real PC, UEFI CD/USB        | ✅ |

On boot the kernel:
1. is loaded into 64-bit long mode by the [Limine](https://github.com/limine-bootloader/limine) bootloader,
2. verifies the boot memory map and higher-half direct mapping,
3. brings up the COM1 serial console and the classic 80×25 VGA text mode,
4. prints firmware type + usable RAM, then parks the CPU.

## Quick start

You need a Linux build host (native, WSL2, VM, or Docker — see
[`REQUIREMENTS.md`](REQUIREMENTS.md)).

```sh
# 1. one-time: fetch the bootloader pieces
./tools/fetch-limine.sh

# 2. build kernel + ISO
make

# 3. boot it in QEMU (serial console included)
make run

# bonus: watch the raw 512-byte MBR bootloader do almost nothing
make mbr && make run-mbr
```

Write the ISO to a USB stick to boot it on real hardware:

```sh
cp windclone.iso /dev/sdX     # careful: this overwrites the whole device
```

## Repository layout

```
src/kernel/        C kernel: entry (kmain), VGA + serial drivers
src/boot/          hand-written NASM boot code (educational MBR)
linkers/           ELF64 linker script (higher-half kernel)
vendor/limine.h    vendored bootloader API header (BSD-0, upstream Limine)
tools/             fetch-limine.{sh,ps1}, build-iso.sh
limine.cfg         boot entries shipped inside the ISO
docs/              ROADMAP.md, CONTRIBUTING.md
.github/workflows/ CI: build ISO + boot smoke tests in QEMU
Makefile           kernel, ISO, MBR, run targets
```

## Roadmap (short version)

The full plan with checkboxes lives in [`docs/ROADMAP.md`](docs/ROADMAP.md).

1. ✅ Boot to long mode, serial + VGA console
2. ⏳ GDT / IDT, interrupts, timer
3. ⏳ Physical + virtual memory managers (`kmalloc`)
4. ⏳ PS/2 keyboard, PCI enumeration
5. ⏳ Initrd filesystem, ELF user processes, syscalls
6. ⏳ Shell in C, drivers starting to move to Rust
7. ⏳ POSIX/Linux compatibility layer (ELF loader + Linux syscall subset)
8. ⏳ Graphical "windows" shell — earning the *Wind* in WindClone

## Why these choices?

* **Limine instead of writing our own boot chain (for now):** getting from
  16-bit real mode to paging is 90 % of most hobby-OS projects' boot bugs.
  Limine is a proven, spec-clean loader; we keep a hand-written MBR
  (`src/boot/mbr.asm`) so the low-level story is still learned in the open.
* **C kernel first:** the most documented territory in OS development
  (OSDev wiki, Philipp Oppermann's blog). Rust enters the picture for
  drivers once we have processes to isolate them into.
* **Hybrid ISO:** "bootable on anything" was the founding requirement.

## License

MIT (this project). `vendor/limine.h` is BSD-0 licensed, © Mintsuki and
contributors. No code from Linux or Windows is used or intended to be
reverse-engineered.
