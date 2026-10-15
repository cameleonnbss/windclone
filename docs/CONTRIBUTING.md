# Contributing to WindClone

Contributions are welcome at every level — including "I have never written
an OS before". The project exists to learn in public.

## Dev loop

```sh
./tools/fetch-limine.sh   # once per machine
make                      # kernel + ISO
make run                  # QEMU with serial console (Ctrl+A, X to quit)

./tools/local-test.sh     # run the same BIOS+UEFI+MBR smoke tests CI runs
```

Every PR must keep CI green: the ISO must still boot in QEMU (BIOS and
UEFI) and print the serial banner. That is the whole contract.

## Code style

* Kernel is C11, compiled with `-Wall -Wextra -Werror` — warnings are bugs.
* No allocations in boot path until `kmalloc` exists (Phase 2).
* Assembly (NASM) uses `BITS 16/64` explicitly and is always commented like
  `src/boot/mbr.asm`.
* Keep each driver in its own `*.c`/`*.h` pair with a one-paragraph header
  comment explaining *why* it exists.

## Pitfalls we already fell into (so you don't have to)

* **Never let SSE into early boot code.** gcc `-O2` happily vectorises loops
  (`movdqa`, `shufps`…) — executing one before the IDT exists is a triple
  fault. The kernel is compiled `-mno-sse -mno-sse2 -mno-avx -mno-mmx
  -mno-80387 -msoft-float` for exactly this reason. If a mystery crash
  disappears after adding a `serial_write()` inside a loop, that was it.
* **Limine's kernel code selector is `0x28`, not the textbook `0x08`.** Read
  CS at runtime (`mov %%cs, %0`) when building IDT gates or TSS entries.
* **Interrupt-frame structs mirror push order, in reverse.** Our stubs push
  the vector *after* the (dummy) error code, so `vector` sits at a lower
  address than `error_code` in `struct regs`. Get it wrong and every IRQ
  reports vector 224 like ours did.
* **BIOS `int 13h`: set `ES`/`BX` before `AH`/`AL`.** `mov ax, 0x8000` after
  `mov ah, 0x02` silently destroys the function number — the disk "read
  failure" was us calling undefined function 0x80.

## Good first issues

Look for the [`good first issue`](https://github.com/cameleonnbss/windclone/labels/good%20first%20issue)
label. Typical size: one file, under 50 lines (e.g. add a `ksnprintf`, a
blink attribute to VGA, a serial hexdump helper).

## Learning resources we follow

* OSDev wiki — https://wiki.osdev.org (the textbook for Phases 1–4)
* Philipp Oppermann's writing an OS in Rust — https://os.phil-opp.com
* Limine spec & examples — https://github.com/limine-bootloader/limine
* AMD64 ABI / ELF spec for the userland phases

## Ground rules

1. From scratch: no copying code from Linux, Windows, or proprietary sources.
   Reference reading of *specs* is fine; copy-paste from other OSes is not.
2. Be kind. This is a teaching project.
3. If you change the build, update `REQUIREMENTS.md` in the same PR.
