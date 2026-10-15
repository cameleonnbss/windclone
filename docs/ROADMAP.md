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

## Phase 2 — Memory 🔄
*Goal: real allocators.*

- [ ] Physical frame allocator (bitmap over the Limine memmap)
- [ ] Virtual page tables (4-level, map/unmap API)
- [ ] `kmalloc` / `kfree` (buddy or slab-lite) + `kassert`
- [ ] `panic()` prints a stack dump via frame pointers

## Phase 3 — Input & devices ⏳
- [ ] PS/2 keyboard driver (scancode set 2 → US layout)
- [ ] PCI configuration-space enumeration + BAR decoder
- [ ] Boot messages ring buffer viewable later via a debug shell

## Phase 4 — Userland v0 ⏳
*Goal: the first program that isn't the kernel.*

- [ ] tar-based initrd from Limine modules
- [ ] VFS layer (`open/read/write/close` in-kernel)
- [ ] ELF64 process loader (ring 3), `syscall`/`sysret` ABI
- [ ] Two syscalls first: `write(fd, buf, len)`, `exit(code)`
- [ ] `hello.elf` running on the console — release v0.1.0 milestone

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
- [ ] Mouse driver (PS/2 → later USB via xHCI)
- [ ] A settings app that looks embarrassingly familiar

## Phase 9 — Beyond x86 ⏳
- [ ] AArch64 + RISC-V boot (Limine supports both)
- [ ] Port plan per platform in `docs/ports/`

---

## Definition of "1.0"
Boots from ISO on BIOS+UEFI hardware, runs a Linux `busybox` binary, has a
mouse-driven window shell, and every phase above is checked. Ambitious? Yes.
That's why each phase ships a bootable ISO.
