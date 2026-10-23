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

## Phase 4 — Userland v0 🔄
*Goal: the first program that isn't the kernel.*

- [x] tar-based initrd from Limine modules: `rootfs/` is packed into a
      ustar archive by `tools/mkinitrd.sh`, Limine loads it with
      `module_path:`, and the kernel walks it **in place** (header →
      checksum → octal size → padded data → next block). Files are
      slices of the module, so reading `/etc/motd` costs zero copies;
      an absent path returns `NULL`, never a wild pointer
- [ ] VFS layer (`open/read/write/close` in-kernel)
- [x] Ring 3: our own GDT with DPL-3 code/data selectors, a TSS so an
      interrupt taken in userland lands on a kernel stack, `iretq` entry,
      and a DPL-3 `int 0x80` gate — the first privilege boundary in the
      system
- [x] Two syscalls first: `write(fd, buf, len)` and `exit(code)`; exit
      unwinds the syscall stack back into the kernel, so a program that
      ends hands control back instead of wedging the CPU
- [x] `/bin/hello` — a flat binary from the initrd (assembled at
      `USER_BASE`, first 94 bytes of user code) that writes a line from
      ring 3 and exits; CI greps the line it printed
- [ ] ELF64 process loader: replace the flat-binary path in `usermode.c`
      with real `PT_LOAD` segments — release v0.1.0 milestone

## Phase 5 — Shell & processes ⏳
- [ ] `fork`-like process creation, scheduler (priority round-robin)
- [ ] Text shell with line editing, reading commands from initrd
- [ ] `cat`, `ls`, `echo` — the classics, written for WindClone

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

## Phase 8 — The *Wind* in WindClone ⏳
- [ ] Framebuffer graphics (Limine FB + simple compositor)
- [ ] Window server: overlapping windows, title bar, close button
- [ ] Taskbar + start menu, and a launcher that starts programs from the initrd
- [ ] Mouse driver (PS/2 → later USB via xHCI)
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
