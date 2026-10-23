# WindClone

> An operating system built from scratch — Linux-style architecture,
> Windows-inspired look & feel, bootable ISO for any x86_64 machine.

![build status](https://github.com/cameleonnbss/windclone/actions/workflows/build.yml/badge.svg)

**WindClone** is a hobby operating system project. Nothing is copied from
Linux or Windows: the kernel is written by us in C (with assembly for the
earliest stages), and Rust / C++ are planned for drivers and userland as the
project matures.

**The goal:** one ISO, downloadable from Releases, that boots into a system
that behaves like Windows from the user's side — a graphical shell with
windows, a taskbar and a start menu, a command line where the familiar
commands and utilities work, and enough compatibility to run real programs —
while the kernel underneath stays a Linux-style design we wrote ourselves.

⚠️ This is a learning project. Today it boots, manages its own memory
(frame allocator, page tables, `kmalloc`), handles hardware interrupts,
and ticks a timer — which is exactly where every real OS started.

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
3. builds its own physical frame allocator (bitmap over the Limine memmap)
   and its own 4-level page tables — then maps the VGA text window (0xB8000,
   MMIO: Limine's HHDM only covers RAM) and brings up the screen console
   next to the COM1 serial one,
4. starts the `kmalloc` heap (lazily-paged arena at `0x4000000000`) and
   runs a heap self-test before anything is allowed to allocate,
5. prints firmware type + usable RAM,
6. audits the GDT, loads a 256-entry IDT with NASM exception/IRQ stubs,
   remaps the 8259 PIC and unmasks IRQ0,
7. takes the PIT timer at 100 Hz and prints a 1-second `uptime:` heartbeat
   from the interrupt-driven idle loop,
8. enumerates the PCI bus (vendor/device/class + BAR decode), arms the PS/2
   keyboard on IRQ1, and keeps a ring buffer of the last 48 boot messages —
   replayed on the serial console every 5 seconds,
9. parses the **initrd** the bootloader loaded as a module: `rootfs/` is
   packed into a plain ustar tar by `tools/mkinitrd.sh`, and the kernel walks
   it in place, lists its files and dumps `/etc/motd` — the first data in the
   system that is not part of the kernel image.

An unhandled exception prints a full register dump (rip/cs/rflags + all GPRs)
plus a frame-pointer backtrace on the serial console instead of silently
triple-faulting.

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

# bonus: watch the two-stage NASM bootloader: MBR loads stage-2 from disk
make mbr && make run-mbr
```

Write the ISO to a USB stick to boot it on real hardware:

```sh
cp windclone.iso /dev/sdX     # careful: this overwrites the whole device
```

## Releases

Every `v*` tag runs the same smoke suite as CI and publishes the bootable ISO
to [Releases](../../releases) — the image attached is the one that just booted
on BIOS and UEFI, with a `SHA256SUMS` beside it:

```sh
git tag v0.1.0 && git push origin v0.1.0
```

## Repository layout

```
src/kernel/        C kernel: entry (kmain), console + memory managers,
                   IDT/PIC/PIT, PCI + PS/2 drivers, interrupt stubs (NASM),
                   string + port-I/O helpers
src/boot/          hand-written NASM boot code (two-stage educational MBR)
rootfs/            what goes into the initrd: packed to a ustar tar, loaded
                   by the bootloader as a module, read in place by the kernel
linkers/           ELF64 linker script (higher-half kernel)
vendor/limine.h    vendored bootloader API header (BSD-0, upstream Limine)
tools/             fetch-limine.{sh,ps1}, build-iso.sh, mkinitrd.sh
limine.conf          boot entries shipped inside the ISO
docs/              ROADMAP.md, CONTRIBUTING.md
.github/workflows/ CI: build ISO + boot smoke tests in QEMU
Makefile           kernel, ISO, MBR, run targets
```

## Roadmap (short version)

The full plan with checkboxes lives in [`docs/ROADMAP.md`](docs/ROADMAP.md).

1. ✅ Boot to long mode, serial + VGA console
2. ✅ GDT audit, IDT + exceptions, PIC remap, PIT timer uptime
3. ✅ Physical + virtual memory managers (`pmm`, own page tables, `kmalloc`)
4. ✅ PS/2 keyboard, PCI enumeration
5. 🔄 Initrd ✅, VFS, ELF user processes, syscalls
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
