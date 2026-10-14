# WindClone — Requirements

Everything needed to build, run, and develop WindClone. If a section says
"not supported", it means the *build* isn't supported — the produced ISO
still boots on anything.

## 1. Build host

| Platform | Status | Notes |
|----------|--------|-------|
| Linux (Debian/Ubuntu, Fedora, Arch) | ✅ recommended | native toolchain |
| WSL2 (any distro) from Windows | ✅ | the normal dev loop on this project |
| Docker (`ubuntu:24.04`) | ✅ | `docker run -it --rm -v $PWD:/w -w /w ubuntu:24.04 bash` + install list below |
| macOS | ⚠️ | needs `x86_64-elf-gcc` cross toolchain (brew: `x86_64-elf-gcc`, `nasm`, `xorriso`, `qemu`) |
| Windows native | ❌ | use WSL2; `tools/fetch-limine.ps1` works natively for the boot files |

## 2. Toolchain (Debian/Ubuntu names)

```sh
sudo apt-get install -y \
  build-essential \   # gcc, ld, make, coreutils
  nasm \              # assembler for src/boot/*.asm
  xorriso \           # hybrid ISO-9660 masterizer (El Torito BIOS+UEFI)
  qemu-system-x86 \   # local + CI boot testing
  ovmf \              # UEFI firmware for QEMU testing
  curl                # tools/fetch-limine.sh
```

Other distros:
* **Arch:** `base base-devel nasm libisofs qemu-desktop edk2-ovmf curl`
* **Fedora:** `@development-tools nasm xorriso qemu-system-x86 ovmf curl`

Minimum versions:
* GCC ≥ 11 (needs `gnu11`, `-mcmodel=kernel` with `-no-pie`)
* GNU Make ≥ 4.3
* xorriso ≥ 1.5.4 (GNU ISO 9660 + El Torito + hybrid MBR support)

## 3. Third-party boot files (fetched, not built)

`./tools/fetch-limine.sh` downloads prebuilt files from the
[Limine bootloader](https://github.com/limine-bootloader/limine)
`v9.x-binary` branch into `limine/` (git-ignored):

| File | Purpose |
|------|---------|
| `limine-bios-cd.bin` / `limine-bios.sys` | legacy BIOS boot stages |
| `limine-uefi-cd.bin` | UEFI El Torito image |
| `BOOTX64.EFI` / `BOOTIA32.EFI` | UEFI firmware fallback loaders |
| `limine.h` | the boot API header (also vendored in `vendor/`) |
| `limine.exe` | host tool used for `bios-install` (dd-to-USB bootability) |

Override the branch with `LIMINE_BRANCH=...` if upstream moves.

## 4. Test / run environment

* **QEMU** ≥ 7 (for `-display none -serial file:` CI mode)
* **Real hardware**: any x86_64 PC with BIOS or UEFI firmware; CD or USB
  (the ISO is hybrid — `cp windclone.iso /dev/sdX` works)
* RAM: kernel currently needs < 16 MiB; use `-m 512` or more

## 5. Disk / network

* ~200 MB free for toolchains, < 5 MB per build artifact
* HTTPS to github.com during `fetch-limine.sh` (once per machine)

## 6. Knowledge prerequisites (for contributors)

You do **not** need them to start, but these are the concepts each phase of
the roadmap leans on:

1. C pointers, `stdint` types, bit manipulation → memory manager
2. x86_64 assembly basics (real vs long mode, `in`/`out`) → `src/boot/`
3. Paging, GDT/IDT → roadmap phases 2–3 (OSDev wiki is the textbook)
4. ELF file format → userland process loading
5. Rust ownership (later, drivers) → roadmap phase 6

## 7. CI

GitHub Actions (`.github/workflows/build.yml`) does the full loop on every
push: install toolchain → fetch Limine → `make` → boot the ISO headless in
QEMU under **both BIOS and UEFI** and grep the serial log for the kernel's
banner → upload `windclone.iso` as an artifact.
