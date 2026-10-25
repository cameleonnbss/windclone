# WindClone Roadmap

A staged plan where every phase ends with something that **boots**. CI runs
the ISO in QEMU on every push, so a broken phase can never hide.

Legend: ✅ done · 🔄 in progress · ⏳ not started

---

## Phase 0 — Bootable skeleton ✅
*Goal: from `make` to a screen full of text on BIOS *and* UEFI.*

- [x] Limine request-based boot (base revision 3)
- [x] Higher-half kernel at `0xffffffff80100000`, `-mcmodel=kernel`
- [x] COM1 serial console (works with no display → CI)
- [x] VGA text-mode driver written (activates in Phase 2: Limine's HHDM maps
      only RAM, so MMIO at 0xB8000 needs our own page tables first)
- [x] Memory map + firmware type report at boot
- [x] Hybrid ISO (El Torito BIOS + UEFI, `bios-install` for dd-to-USB)
- [x] Educational 512-byte NASM MBR (`make mbr`, `make run-mbr`)
- [x] GitHub Actions: build + BIOS/UEFI boot smoke tests

## Phase 1 — CPU plumbing ✅
*Goal: react to the machine, not just read its boot data.*

- [x] GDT audit via `sgdt` (Limine's GDT stays live — we report its layout;
      replacing it with our own comes when we add ring 3 in Phase 4)
- [x] IDT + 32 exception stubs in NASM; panic prints the faulting vector,
      a full register dump and CR2 for page faults
- [x] PIC remap (8259 → vectors 32..47), masking, EOI, spurious-IRQ filter
- [x] PIT timer at 100 Hz, kernel tick + `uptime: Ns` on the serial console
- [x] Kernel built `-mno-sse -mno-mmx -mno-80387`: gcc's `-O2` vectoriser
      emits SSE, which triple-faults before the IDT even exists
- [x] Educational MBR upgraded to a real two-stage loader: stage-0 reads
      stage-2 from LBA1 via int 13h (with retry + BIOS error-code report)

## Phase 2 — Memory ✅
*Goal: real allocators.*

- [x] Physical frame allocator: bitmap over the Limine memmap (1 bit / 4 KiB
      frame, allocates high→low to keep low memory free for future DMA;
      reserves the low 1 MiB + our own kernel image via `__kernel_start`/`_end`)
- [x] Virtual page tables: our own 4-level P4 that copies Limine's higher
      half (kernel + HHDM live) and owns the lower half; `map`/`unmap`/`translate`
      API; virtual page 0 left unmapped as a free NULL trap; huge pages and
      missing-levels handled in `paging_translate`
- [x] First self-mapped MMIO: the kernel pages in the VGA window at 0xB8000
      (PCD/uncached) — Limine's HHDM covers RAM only, never device memory —
      and a written text cell is read back to prove the path works
- [x] `kmalloc`/`kfree`/`kcalloc`: first-fit + split + forward coalesce over
      a lazily-grown heap arena at 0x4000000000 (pages backed on demand by
      pmm + paging); `kassert` wired to `panic`
- [x] `panic()` walks the stack for a backtrace (`-fno-omit-frame-pointer`),
      guarded so the unwinder can never fault itself on unmapped frames

## Phase 3 — Input & devices ✅
*Goal: talk to the machine's peripherals, and prove it headlessly.*

- [x] PS/2 keyboard: 8042 driver that *keeps* the controller's AT
      translation ON and decodes the resulting set-1 make/break stream
      (US layout table + shift + caps); verified in CI by injecting a
      real `sendkey` through the QEMU monitor and grepping the decoded
      result — no human needed
- [x] PCI: configuration-space enumeration via mechanism #1 (ports
      0xCF8/0xCFC) over all 256 buses, with vendor/device/class report
      and a BAR decoder (io vs mem32/64, 64-bit pairs combined)
- [x] Boot messages ring buffer (`klog`): last 48 formatted messages
      kept in RAM (IRQ-safe, tiny printf), replayed every 5 s on serial
      and reserved for the Phase 5 debug shell

## Phase 4 — Userland v0 ✅
*Goal: the first program that isn't the kernel.*

- [x] tar-based initrd from Limine modules: `rootfs/` is packed into a
      ustar archive by `tools/mkinitrd.sh`, Limine loads it with
      `module_path:`, and the kernel walks it **in place** (header →
      checksum → octal size → padded data → next block). Files are
      slices of the module, so reading `/etc/motd` costs zero copies;
      an absent path returns `NULL`, never a wild pointer
- [x] VFS layer (`open`/`read`/`write`/`close` in-kernel): a fixed table of
      descriptors over the initrd's slices — fd 0/1/2 are the console, fd 3+
      are files, `read` advances the offset, EOF is `0`, a closed or unknown
      descriptor is `-9` (`EBADF`) and a missing path is `-2` (`ENOENT`);
      files are read-only, so a `write` to a file descriptor is refused
      instead of silently succeeding. The kernel's own ELF loader reads
      programs through this same door, and so does ring 3
- [x] Ring 3: our own GDT with DPL-3 code/data selectors, a TSS so an
      interrupt taken in userland lands on a kernel stack, `iretq` entry,
      and a DPL-3 `int 0x80` gate — the first privilege boundary in the
      system
- [x] Five syscalls so far: `write`, `exit`, then `open`/`read`/`close`.
      `exit` unwinds the syscall stack back into the kernel, so a program
      that ends hands control back instead of wedging the CPU; an unknown
      call number answers `-38` (`ENOSYS`) rather than faulting. Every
      user pointer is validated first — range, then `PRESENT` + `USER`
      (+ `WRITE` for a read target) page by page — because there is no
      fixup table yet: a fault inside a syscall is a panic, not an `-EFAULT`
- [x] `/bin/hello` — the first program. Started as a flat binary assembled
      at `USER_BASE`; it is now a real ELF64 object (`nasm -f elf64`,
      `_start`, `.rodata`) that the loader places and runs, and CI greps
      the line it printed from ring 3
- [x] `/bin/motd` — the first program that reads a file: `open`/`read` in
      128-byte chunks until EOF, `close`, then one more `read` to show the
      descriptor is really dead. It also reports its own `.data` (a marker
      read back with `volatile`, because a compiler that folds a variable
      nothing ever writes would delete the very section under test) and its
      `.bss` counter, which is how the load/zero split is proven from inside
      the program rather than from a `readelf` invocation
- [x] ELF64 process loader: the flat-binary path in `usermode.c` is gone.
      `elf.c` honours every `PT_LOAD` (entry must be mapped, offsets and
      virtual addresses must agree `mod p_align`, sizes must not overflow or
      escape the user window), uses each page as a window into the file, and
      zeroes `p_memsz - p_filesz` for `.bss`. Protection is a **second**
      pass: filling happens while every page is writable, then code pages are
      re-mapped read-only — otherwise writing `.text` read-only faults on the
      very copy that fills it. Loading a program releases the previous one's
      frames first, so running two in a row cannot re-fill read-only pages.
      Known gaps: no NX (read-only code is still executable), one address
      space shared with the kernel, no `argv`/`envp`/auxv on the stack.
      *(The roadmap called this the v0.1.0 milestone, but v0.1.0 was cut at
      Phase 5 — the loader therefore ships as v0.1.1)*

## Phase 5 — Shell & processes 🔄
- [x] Text shell with line editing (a 64-byte IRQ queue, Enter/Backspace),
      built-ins and a scrollback grid that survives repaints: `help`, `ls`,
      `cat <file>`, `about`, `clear`
- [x] `hello` starts `/bin/hello` from the initrd in ring 3 and returns to
      the prompt — a command that is a real program, not a built-in
- [ ] More programs in the initrd (`cat`, `ls`, `echo` as ELF files rather
      than built-ins), which needs the ELF loader
- [ ] `fork`-like process creation, scheduler (priority round-robin)

## Phase 6 — Language expansion ⏳
*Goal: use the right tool per layer (the "asm/C/C++/Rust" promise).*

- [ ] Rust driver scaffolding (no_std) behind a stable C ABI
- [ ] First driver rewritten in Rust: keyboard
- [ ] C++ usable in userland (headers + libstdc++ subset or cxx shim)
- [ ] `rustfmt`/`clippy` + `cargo` added to CI

## Phase 7 — Linux compatibility ⏳
*The "Linux clone" part of the pitch.*

- [ ] ELF loader matching Linux semantics (mmap-based)
- [ ] Linux syscall numbers → internal translation layer
- [ ] `read/write/openat/close/br/mmap/munmap` subset
- [ ] Run `busybox` static binary as the conformance test — release v0.2.0

## Phase 8 — The *Wind* in WindClone 🔄
- [x] Framebuffer graphics: Limine's linear buffer mapped uncached into a
      low window of our own page tables, 32-bit pixels, pitch-correct
      writes, a vertical-gradient wallpaper and a 5x7 font compiled in
- [x] First composited frame: desktop icons, taskbar with a start button,
      one window with a title bar and a close button — drawn back to front,
      no double buffer yet (verified by a QEMU `screendump`)
- [x] Window manager: three overlapping windows, focus by clicking anywhere
      on one, title-bar buttons (minimise / maximise / close), taskbar
      buttons that restore or raise, and desktop icons that reopen a window
      that was closed. Hit testing runs front to back and stops at the first
      window that owns the press — one rule, and it is what makes overlapping
      windows behave the way people expect
- [x] Headless proof: `tools/qemu-desktop.py` injects real PS/2 moves and
      clicks through the QEMU monitor and greps the kernel's `wm:` markers
- [ ] Damage tracking + double buffer (a mouse move still repaints everything)
- [ ] Start menu, and a launcher that starts programs from the initrd
- [x] Mouse driver: 8042 aux channel, IRQ12 (cascade unmasked), 3-byte
      packets with the sign bits in the flags byte — verified headlessly by
      injecting `mouse_move` through the QEMU monitor and grepping the
      decoded packet
- [x] Cursor + one draggable window: grab the title bar, the window follows,
      clamped to the desktop (full-screen repaint — damage tracking is next)
- [ ] USB (xHCI) pointing devices, once there is a PCIe driver stack
- [ ] A settings app that looks embarrassingly familiar

## Phase 9 — Beyond x86 ⏳
- [ ] AArch64 + RISC-V boot (Limine supports both)
- [ ] Port plan per platform in `docs/ports/`

---

## Definition of "1.0"

A Windows-shaped desktop you download as an ISO from the GitHub Releases page,
write to a USB stick, and boot on real BIOS *and* UEFI machines. On screen: a
graphical shell with overlapping windows, a taskbar and a start menu. Behind
it: a command line where the familiar commands run as real executables, and
enough compatibility to run programs built for Linux. Every phase above is
checked, and every `v*` tag publishes that ISO through
`.github/workflows/release.yml`.

Ambitious? Yes. That's why each phase ships a bootable ISO.
