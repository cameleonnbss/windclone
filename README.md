# WindClone

> An operating system built from scratch — Linux-style architecture,
> Windows-inspired look & feel, bootable ISO for any x86_64 machine.

![build status](https://github.com/cameleonnbss/windclone/actions/workflows/build.yml/badge.svg)

![WindClone desktop: two overlapping windows, a taskbar with window
buttons and desktop icons](docs/screenshots/desktop.png)

*The desktop as it boots: an info window and a terminal overlapping, the
terminal focused and running `/bin/hello` from the initrd in ring 3.
Screenshot taken by the test suite itself (`tools/qemu-desktop.py`).*

![WindClone start menu: thirteen program rows read out of /bin in the initrd,
above the DEMARRER button](docs/screenshots/start-menu.png)

*The start menu, opened by one click on DEMARRER. Every row is a `/bin`
entry that the kernel enumerated from the initrd at that moment — clicking
one starts it, as a process with its own pid and the shell as its parent. The
test suite clicks a row it found in its own serial log and checks the program
answered (`tools/qemu-desktop.py menu`), and clicks `/bin/whoami` to check the
pid the kernel handed out at spawn is the one the program reads back
(`menuproc`).*

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
   system that is not part of the kernel image,
10. installs its own GDT (ring-3 selectors) and a TSS, puts a small descriptor
   table over the initrd (`open`/`read`/`write`/`close`, fd 0-2 being the
   console and fd 3+ the files), then loads `/bin/hello` and `/bin/motd` as
   real **ELF64** images — `PT_LOAD` segments honoured, `.bss` zeroed, code
   pages write-protected — and runs them **in ring 3**. They reach the
   console and the archive only through the `int 0x80` gate
   (`write`, `exit`, `open`, `read`, `close`, `getdirent`, and later
   `getpid`, `sched_yield`, `spawn`, `wait`, `kill`), every user pointer
   being checked against the page tables before the kernel touches it, and
   `exit` unwinds back to the kernel — the first privilege boundary in the
   system,
11. maps the linear framebuffer the bootloader set up (device memory, so
   uncached and outside the HHDM) and composites the **desktop**: gradient
   wallpaper, desktop icons, a taskbar with a start button, and three
   overlapping windows — all drawn by the kernel with a 5x7 font it carries
   itself (no font files, no libc),
12. drives a **PS/2 mouse** on the 8042's aux channel (IRQ12, three-byte
   packets, sign bits decoded) and draws a cursor. The **window manager**
   answers it the way people expect: click a window to focus it, drag it by
   the title bar, minimise / maximise / close from the title-bar buttons,
   restore or raise it from a taskbar button, reopen a closed window from
   its desktop icon. Hit testing runs front to back and stops at the first
   window that owns the press. CI drives real PS/2 clicks through the QEMU
   monitor and greps the kernel's `wm:` markers — a headless run therefore
   proves the window manager, not just the mouse,
13. runs a **command prompt in a window**: a terminal with a line editor and
   a scrollback grid, focused by clicking it. Only `help`, `about` and `clear`
   are built in; anything else is looked for in `/bin` and started as a real
   program with the rest of the line as its `argv` — `ls` lists the archive
   through `getdirent`, `cat /etc/issue` prints a file, `echo zzz` answers
   `zzz`, and whatever a program writes lands in the terminal. CI types the
   commands through the emulated keyboard and greps what they printed.
14. runs **two programs at once**, and now **interrupts them mid-compute**.
   Each process gets its own 4-level page tables
   (`paging_clone_address_space()` shares the kernel's view, keeps the user
   window private), so two images can sit at the same `0x400000` and never
   see each other. The **round-robin scheduler** (`proc.c`) switches between
   them by saving a process's `syscall_frame`, loading the next one's `CR3`,
   and letting the return frame's `iretq` resume it — no new assembly, twice:
   a voluntary switch rides the `int 0x80` frame, and a timer tick landing on
   ring 3 rides the IRQ frame, which the CPU builds with the *same* layout.
   The boot spawns two copies of `/bin/tick` (print-and-yield) and two of
   `/bin/spin` (a 30-million-iteration hot loop that never yields); CI
   asserts the two processes' output line ranges *cross*, which a
   run-to-completion loop cannot fake — for the spins it is the only way they
   can interleave at all, so the tick really does preempt.
15. lets a program **not come back from a syscall**. `wait()` is the first
   call in this kernel that does not return into its caller: `/bin/waiter`
   spawns `/bin/exit 7` and `/bin/exit 9` — spawn now carries a real `argv`
   through kernel bounce buffers into the child's private address space —
   then sleeps as `PROC_BLOCKED` while the children burn their turns, and
   each child's exit wakes it with that child's pid, its exit code written
   through the pointer under the *parent's* page tables. The zombie's table
   slot survives until the parent reaps it, so `spawn → exit → wait → reap`
   closes the process trilogy the way Unix does it; CI cross-checks the
   pid/code pairs the parent saw against the ones the children printed.
16. lets a program **end another one**. `kill(pid, SIGKILL)` is the fourth
   verb: `/bin/reaper` spawns `/bin/bell` — a child that works for seconds and
   never quits by itself — yields until the bell demonstrably prints, kills it,
   and reaps it. The victim is off-CPU by definition (READY or BLOCKED), so the
   kernel retires it where it stands: address space back, slot marked `EXITED`
   with code `-SIGKILL`, the waiting parent woken. `/bin/vanish` covers the one
   case that cannot be a table edit — killing yourself holds the CPU and the
   trap frame, so it rides the exit arm and does not return. CI proves both by
   absence as well as presence: `[bell] done` and `[vanish] STILL HERE` must
   **not** be in the log, while the reaped pid and its `-9` must.
17. opens a **start menu** that is a launcher, not a picture of one: the rows
   are the `/bin` entries of the initrd, enumerated through the VFS each time
   the panel opens, so a program added to the archive is clickable without the
   desktop knowing its name. Clicking a row runs that program through the same
   path the command prompt uses — same lookup, same terminal for its output,
   same exit code — and the test proves it by reading the row index out of the
   kernel's own log line and clicking *that*, then checking `/bin/hello`
   answered. Nothing in `desktop.c` contains a program name.
18. gives the **command prompt a pid**. The shell used to drive the process
   table from outside it, and three refusals were the consequence: a program
   typed at the prompt could not `spawn`, `wait` or `kill`, because it had no
   entry in the table to be anybody's parent or child. `shell_init()` now claims
   one — pid 1, state `PROC_KERNEL`, no address space and no trap frame because
   its code runs on the kernel stack — and every program the prompt starts or
   the menu clicks is created as its child and run by the real scheduler.
   `/bin/whoami` is the proof from the other side of the boundary: the pid it
   reads back with `getpid()` must be the number the kernel printed when it
   spawned it, and its `kill(1)` must come back `-EPERM` rather than `-ESRCH`,
   because the shell genuinely is there. Typing `vanish` proves the other half:
   a program the shell starts can now be *killed*, so the `-9` arrives at the
   prompt and has to be reported as `-9` instead of a tidy, invented `0`.

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
                   IDT/PIC/PIT, PCI + PS/2 drivers, initrd/VFS, ELF64 loader,
                   per-process page tables + preemptive round-robin scheduler
                   (proc), interrupt stubs (NASM), string + port-I/O helpers
src/user/          the programs that run in ring 3 — hello, motd, ls, cat,
                   echo, tick, spin, exit, waiter, bell, reaper, vanish,
                   whoami — with no libc of their own: a header of inline
                   `int 0x80` wrappers and eight static inline string helpers
src/boot/          hand-written NASM boot code (two-stage educational MBR)
rootfs/            what goes into the initrd: packed to a ustar tar, loaded
                   by the bootloader as a module, read in place by the kernel
linkers/           ELF64 linker scripts — kernel.ld (higher-half kernel),
                   user.ld (programs linked at 0x400000, small model)
vendor/limine.h    vendored bootloader API header (BSD-0, upstream Limine)
tools/             fetch-limine.{sh,ps1}, build-iso.sh, mkinitrd.sh,
                   local-test.sh (smoke suite, same one CI runs),
                   qemu-desktop.py (drives the desktop with real PS/2 clicks)
limine.conf          boot entries shipped inside the ISO
docs/              ROADMAP.md, CONTRIBUTING.md, screenshots/
.github/workflows/ CI: build ISO + boot smoke tests in QEMU
Makefile           kernel, ISO, MBR, run targets
```

## Roadmap (short version)

The full plan with checkboxes lives in [`docs/ROADMAP.md`](docs/ROADMAP.md).

1. ✅ Boot to long mode, serial + VGA console
2. ✅ GDT audit, IDT + exceptions, PIC remap, PIT timer uptime
3. ✅ Physical + virtual memory managers (`pmm`, own page tables, `kmalloc`)
4. ✅ PS/2 keyboard, PCI enumeration
5. ✅ Initrd, VFS, ELF64 processes, nine syscalls, `cat`/`ls`/`echo` as real
   programs, and a preemptive round-robin scheduler running two processes at
   once with the timer switching them
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
