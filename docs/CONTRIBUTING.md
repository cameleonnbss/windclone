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
* No allocations before `kmalloc_init()` has run — the Phase-2 self-test
  (`kmalloc selftest ok`) must always come *before* any other user of the heap.
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
* **Walk page tables down to level 1, then stop.** The last level is a *leaf
  table*, not a page: descending "one more level like the others" makes the
  walk dereference the PTE value as if it were an address — and when that
  address is device MMIO outside the HHDM the fault triple-kills the kernel
  before the IDT even exists. Read the leaf entry *after* the loop.
* **Limine v9 has no `terminal:` boot option** (it is silently ignored). The
  real knobs are global `graphics: no` and per-entry `textmode: yes`, and
  even those are best-effort hints — Limine's GUI can hand off while the
  CRTC is still scanning a graphical mode. The kernel re-forces VGA text mode
  itself (`vga_text_mode_init()`), so don't "fix" this in the boot config.
* **Bochs VBE dispi: `ENABLE` is index 4, not 7.** The register map is
  0=ID, 1=XRES, 2=YRES, 3=BPP, 4=ENABLE, 5=BANK, 6=VIRT_WIDTH, 7=VIRT_HEIGHT.
  Writing "ENABLE=0" to index 7 zeroes the virtual height and corrupts the
  mode in a way that is painful to debug.
* **After a graphical bootloader, trust the *guest*, not the QEMU monitor,
  about MMIO.** A kernel-side write/read-back of a text cell at 0xB8000
  round-trips even when QEMU's `xp` of the same physical address and
  `screendump` disagree (the device model's display path lags the guest's
  MMIO view). That is why the CI evidence for the VGA console is the serial
  marker `vga: text console live (MMIO round-trip ok)`, not a screenshot.
* **8042 command-byte bits are a compatibility swamp — read before
  writing.** QEMU models the *original Intel* layout (kbd IRQ enable =
  0x01, disable interface = 0x10, translate = 0x40) while 90s PS/2
  hardware uses the later one (translate = 0x01, kbd IRQ = 0x02...). A
  mask that looked right silently killed IRQ1 in QEMU. We now READ the
  live mode byte (command 0x20) and only OR safety bits onto it.
* **Don't stack set-1 on top of AT translation.** While the controller's
  translate filter is on it converts whatever the device emits at READ
  time; switching the *device* to raw set 1 underneath it makes QEMU run
  the set-1 codes through the translate table a second time ('a' arrives
  as 0x03 — '2'). Pick one layer: we keep translation ON and never send
  the F0 set-select command.
* **A tar is 512-byte blocks, not a stream of records.** The size field is
  *octal ASCII*, the header checksum is the block's byte sum with the
  checksum field counted as spaces, file data is padded to a 512-byte
  multiple, and directories are entries too (typeflag `'5'`) that you must
  step over even though you never want to read them. Forget the padding and
  every entry after the first one is garbage. `initrd.c` also checks the
  checksum: half-parsing a corrupt archive is how you get a wild pointer
  instead of an error message.
* **A TSS descriptor must sit exactly where the GDTR limit says it does.**
  Our GDT entries and the 16-byte TSS descriptor used to be two separate
  statics; the linker is free to place those anywhere, so `ltr` read
  whatever bytes happened to follow the table and #GP'd. Before the IDT
  exists, a #GP is a triple fault with no message at all. They now live in
  one packed struct: offset 40 is index 5, or it is a crash.
* **`int 0x80` is an interrupt gate, so the CPU clears IF on entry.** Our
  `exit` syscall does not `iretq` — it unwinds the stack back into the
  kernel — so nothing restores the flags the kernel was running with. The
  kernel resumed with interrupts disabled, `hlt` never woke up, and the
  uptime heartbeat stopped dead. `user_exit_to_kernel` now does `sti`
  before returning; if a manual context switch ever replaces iretq, that
  flag is the first thing to check.
* **The USER bit has to be on every level of the walk, not just the leaf.**
  A ring-3 access to a page whose P4/P3/P2 entries lack it faults even
  though the leaf looks correct.
* **Fill the image first, protect it second.** Mapping `.text` read-only and
  then copying the file into it is a kernel write to a read-only page: the
  fault lands inside the loader, where there is no error to return. Same
  family: with one address space and one page list, loading a *second*
  program must release the first one's frames (unmap + `pmm_free_frame`)
  before filling anything, or the second image is copied onto pages the first
  left write-protected.
* **A `static` nobody ever writes can vanish from the binary.** gcc folds a
  read-only initialising variable into its use site, so the `.data` the test
  was supposed to check does not exist and the test passes while proving
  nothing. `/bin/motd`'s marker is `volatile` for exactly that reason — the
  keyword is the test, not an optimisation.
* **`-z noexec-stack` is not a flag.** ld prints *"warning: -z noexec-stack
  ignored"* and links anyway; the spelling is `-z noexecstack`. A warning you
  read as a style nit is a feature you do not have.
* **A bracket in a grep pattern is a character class.** `grep -aq "[motd]
  segments"` never matches the kernel line `[motd] segments`, so the smoke
  test fails on a line that is plainly in the log. Escape it (`\\[motd\\]`)
  when the literal is a shell string that feeds `grep` a BRE.
* **Prove input headlessly with the QEMU monitor:** `-monitor
  unix:/tmp/wc-mon.sock,server,nowait` plus `sendkey a` injected after
  boot, then grep the serial log for the decoded character. That is how
  CI tests a keyboard nobody is pressing.

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
