# Running real Windows software on WindClone — the honest plan

**Requested:** run `.exe`, Python, `.bat` and PowerShell programs the way
Windows does, with `pip install rich` working, on a desktop that behaves like a
daily driver.

**Where WindClone is today (v0.1.44):** a from-scratch x86-64 kernel on Limine,
its own ELF64 loader, its own int `0x80` ABI of fourteen numbered syscalls, a
libc-less userland of 27 freestanding C programs, a tmpfs store, an e1000 driver
with an ICMP/UDP/DHCP/TCP/HTTP stack on top of it, a compositor with per-cell
colour and a pixel channel, an inflate decoder, and headless QEMU tests that
compare bytes rather than impressions.

That stack is good. It is not Windows. The honest consequence, stated up front
because it is the biggest single fact in this file:

> **A PE / `.exe` loader is not something this kernel can be given.** A Windows
> binary does not ask the kernel for anything WindClone could answer: it imports
> `kernel32.dll!CreateFileW`, expects the NT native API, section objects, APCs
> and the Windows thread model, a registry, GDI and DirectX, and `ntdll`
> semantics that thousands of callers depend on. Reimplementing that to the
> point where arbitrary third-party programs run is the Wine problem — roughly
> two decades of full-time work, and Wine still does not run everything. A
> weekend patch that "runs .exe files" would produce a broken promise, a
> misleading README and a release that lies about itself. So nothing below
> claims more than a headless test can check.

## What already works, exactly

| Requested | Reality in v0.1.42 |
|---|---|
| `.bat` files run | Yes, and since v0.1.37 with the cmd semantics a copied script actually leans on: `bat <NAME>` executes `/tmp/<NAME>.BAT` with `@` suppressing a line's echo, `ECHO OFF`/`ECHO ON`/bare `ECHO` as the script's echo state, `SET NAME=value` + `%NAME%` expansion (session environment, case-insensitive names), `rem` alongside `;` as a comment, and a nested `bat` refused. Every script line also logs its resolved form to the serial transcript, so an `ECHO OFF` run is still fully provable. **No** `if`/`for`/`goto`/`call`/`errorlevel`, no redirection or pipes, no arguments passed to a script. |
| Windows-like shell verbs | `DIR`, `DEL`, `REN` are built-ins with `cmd`'s error classes (`EPERM` read-only, `ENOENT`, `EEXIST`, `EBUSY`) — v0.1.32. `SET` and `%VAR%` live inside scripts (v0.1.37), and since v0.1.38 `SET NAME=value` / bare `SET` and `TYPE <path>` are prompt built-ins too (`set` folds the name to upper case like cmd, lists the environment back, and `type` prints a store/archive file's bytes to the transcript). Since v0.1.39 `%VAR%` expands on a *typed* line too — a defined name substitutes before the command runs, an undefined one stays literal so a `save script` writing a `%` survives. Since v0.1.40 cmd's caret escape works on both typed and scripted lines: `^` consumes itself and emits the next character literally, and `^%` cannot start a reference, so a real percent survives a line that also expands variables. `MOVE` is a built-in too since v0.1.42, and it is deliberately not `REN`: where `ren` refuses an occupied destination (`EEXIST`), `move` drops the file in the way, lands the source on the freed name and says `(replaced existing)`; `/Y` is parsed (any case) so a script that passes it still resolves to the right source and target. Still missing: cmd's doubled-caret-inside-a-batch-file and its quoting interaction, `CD`, `COPY` as a verb (it is a `/bin` program), `MKDIR`, and `MOVE` over wildcards or directory trees (this store has flat names, so there is no tree to recurse). |
| ZIP extraction "like Windows" | `/bin/unzip` extracts **STORED** (v0.1.33) *and* **DEFLATE** (v0.1.35, `src/user/inflate.h`) members, and since v0.1.36 under **relative directory names** (`demo/__init__.py`) into the store, with a per-entry digest the test recomputes from what the format says the file is — so a real `.whl` unpacks in place. Still refused, by rule and not by accident: a name carrying a `..` component or a leading `/` (the zip-slip that would write outside the destination), and bzip2/LZMA entries, which are not this decoder's format. A folder record (a name ending in `/`) is skipped because this store has no directories. What a `.whl` still needs is an interpreter, not an extractor. |
| Internet access to download things | Yes over the e1000: DHCP lease, ARP, ICMP, UDP, a TCP handshake and an HTTP `GET` whose body lands in the store (`fetch <path>`, `/bin/pkg`, `/bin/browser`) — v0.1.25→31. |
| Images on screen | `/bin/imgview` decodes a 24-bit BI_RGB BMP and the kernel blits it — v0.1.34. |
| Python programs | **No.** There is no interpreter of any kind in this OS. |
| PowerShell (`.ps1`) | **No.** |
| `pip install rich` | **No**, and it is not one release away — see below. |
| `.exe` | **No**, and see the opening statement. |
| Bluetooth / WiFi / GPU acceleration | **No.** One NIC driver exists (e1000, QEMU-proven); there is no USB stack, no WiFi, no 2D/3D acceleration beyond the CPU blit the compositor does. |
| VirtualBox VM | Script provided, see below. Not booted here: this host's VirtualBox install has its `VBoxSDS` service disabled and re-enabling it needs an administrator, which no tool can supply on its own. |

## Why `pip install rich` is a sequence and not a feature

The chain it needs, and what is missing at each link:

1. **HTTPS.** PyPI is HTTPS-only; WindClone speaks plain HTTP to a local
   server in its tests. TLS is a crypto stack (X.509, AEAD, handshake) on its
   own — its own releases.
2. **A wheel is a ZIP.** We can fetch bytes, and since v0.1.35 we can unpack
   deflated entries. What a `.whl` specifically adds is that every member lives
   under a directory (`demo/__init__.py`, `demo-1.0.dist-info/METADATA`) — and
   since v0.1.36 `/bin/unzip` writes those relative names into the store rather
   than refusing them, so a real wheel unpacks in place. The archive rung is
   done; what a `.whl` still cannot run on is links 1 and 3 — no TLS to reach
   PyPI, and no interpreter to execute the modules once they are unpacked.
3. **An interpreter.** `rich` is ~40 modules of modern Python — classes,
   decorators, f-strings, generators, dataclasses, `typing`. It sits on a deep
   stdlib (`io`, `os`, `re`, `unicodedata`, `threading`). A partial interpreter
   runs a hand-written program; it does not run `rich`, and saying otherwise
   would be the same broken promise as a fake `.exe` loader.

So the honest ladder, in the order each rung becomes testable:

- [x] **inflate** (DEFLATE) in `/bin/unzip` → real archives open. Landed in
      v0.1.35: `src/user/inflate.h`, proven by unpacking deflated members —
      including a canned-table block no machine-generated archive would exercise —
      and matching digests the test recomputes from the archive's own bytes.
- [x] **subdirectory entries in `/bin/unzip`**, so a wheel's
      `demo/__init__.py` is not refused. Landed in v0.1.36: the store already
      keys files by their whole path, so this was a name-validation rule and a
      test, not a filesystem — folder records are skipped and a `..` component
      or leading `/` is still refused by name. Getting there fixed a real input
      bug (`keyboard.c` swallowed the LeftShift release, so a typed underscore
      stuck Shift on and uppercased everything after it).
- [x] **the `@`/echo/environment half of batch semantics**: `@echo off`,
      `%VAR%` and `SET`, `rem`. Landed in v0.1.37 inside the batch reader (the
      prompt keeps its v0.1.19 behaviour), proven by typing three scripts at
      the prompt and reading the resolved lines back off the serial transcript —
      including `/bin/echo` printing the *expanded* value of `%X%`.
- [ ] **the control-flow half of batch semantics**: `IF`/`FOR`, `ERRORLEVEL`,
      `>`/`|`, and arguments passed to a script. Each is its own rung, each
      proven the same typed-script-against-transcript way.
- [x] **prompt-time `SET` and `TYPE`**, the two cmd verbs a person types at the
      keyboard. Landed in v0.1.38 on the `execute()` path: `do_set(work, who)`
      is shared with the batch reader (`"batch"`/`"prompt"` tag the transcript),
      so `set name=value` folds the name to upper case and a bare `set` lists the
      environment; `do_type` opens through `vfs_open` and dumps the file's exact
      bytes to the serial log. Proven by the `promptset` scenario (definition,
      listing, malformed refusal, the byte-for-byte dump, missing-file refusal).
      Still not done: `%VAR%` expansion on a *typed* line (a caret escape must
      land first), and `CD`/`MKDIR` (`MOVE` landed in v0.1.42).
- [x] **prompt-time `%VAR%` expansion** on a *typed* line. Landed in v0.1.39:
      `prompt_expand()` runs at the top of `execute()` before the built-ins and
      `run_line`, so `echo %pz%` after `set pz=bonjour` dispatches `echo bonjour`
      the way cmd expands before running. Unlike the batch reader, an *undefined*
      name stays literal, which is exactly what keeps a typed
      `save script a … echo %x%` writing `%x%` into the file (batch2 stays green).
      The kernel logs `shell: prompt exec '<resolved>'` only when a defined name
      substituted; the `promptvar` scenario proves the substitution and, by a
      count check, that `echo %nope%` writes no exec row. Still not done: the
      caret (`^`) escape that lets a literal percent survive a *defined*
      expansion — and `CD`/`MKDIR` (`MOVE` landed in v0.1.42).
- [x] **cmd's caret escape** on typed and scripted lines. Landed in v0.1.40:
      `^` consumes itself and emits the next character literally, and because it
      is handled before the `%` scanner a `^%` can never start a reference — so
      `echo ^%pz%` prints `%pz%` even with `pz` set, and `echo a^b` becomes
      `echo ab`. Added to both `prompt_expand()` and the batch reader's
      `bat_expand()`. The kernel already mapped scancode `0x07` + shift to `^`;
      the harness `KEYMAP` gained `"^": "shift-6"`, the same QMP chord path that
      delivers `%`. Proven by the `caret` scenario. Still not done: cmd's
      doubled caret inside a batch file and its quoting interaction — and
      `CD`/`MKDIR` (`MOVE` landed in v0.1.42).
- [x] **the boot test keys on the guest, not a clock** (v0.1.41). The two CI
      flakes — `UEFI log missing: [exit] code 9 pid` at v0.1.23, the same line on
      the v0.1.40 push — were never WindClone's: the boot checks SIGTERM-killed
      QEMU at a fixed wall and grepped whatever had flushed by then, so a slow
      runner truncated the tail of the demo. The kernel now prints
      `boot: startup demo complete` when its synchronous demo is fully written,
      and `tools/local-test.sh` waits for that (plus the last interactive marker)
      before quitting QEMU through the monitor, which flushes the serial file.
      This is what makes a green tag build mean the same thing on every machine —
      the reliability the whole release train rests on.
- [x] **`MOVE` as a built-in** (v0.1.42). The verb cmd users reach for next, and
      the one that is *not* a rename: `vfs_rename` answers `WC_EEXIST` when the
      destination name is taken and `REN` reports that refusal, while Windows
      `move` overwrites. `cmd_move` tries the rename whole first — the only way a
      refused move can promise the source never moved — and only after that
      attempt proves the name occupied does it `vfs_remove` the destination and
      retry, logging `(replaced existing)`. Built from the two VFS primitives
      v0.1.32 already shipped: no new syscall, no ABI change. `/Y` is skipped
      case-insensitively so a script's switch is never read as the source name.
      The `move` scenario proves the arithmetic rather than the sentence: after
      two real copies, a move onto a free name holds the entry count and a move
      onto the taken name drops it by exactly one — the overwritten file is gone,
      not shadowed — plus both refusals (`no such file`, `the archive is
      read-only`).
- [x] **`/bin/fc` — File Compare with the exit code as the verdict** (v0.1.43).
      `fc` reads two files through the VFS, compares them byte-for-byte, reports
      sizes and FNV-1a digests, and exits 0 (identical), 1 (different, with the
      first-difference byte and line), or 2 (refused: unreadable, past FC_MAX,
      or wrong argument count). The `fc` scenario tests archive-vs-store copy
      equality, a single-letter equal-length difference (byte 25 line 1), a
      two-line script difference (byte 16 line 2), a proper-prefix size mismatch
      (byte 4 line 2), and all three refusal paths. Two harness limits shaped the
      test: the shell's 61-char input buffer and `USER_ARG_MAX = 16` both truncate
      a long command silently, making "differing" files identical when the
      discriminating word falls past the cut. The `count_lines` helper in fc.c
      included a trailing partial line, which double-counted the +1 used for
      1-based line numbering — replaced by a direct newline count.
- [x] **`/bin/find` — cmd's string search, count cross-checked** (v0.1.44).
      `find` prints every line of a file that carries a piece of text and lets
      the exit code say whether any line did (0 found, 1 none, 2 refused). It
      reuses the same slurp path fc proved, so a search reaches both archive and
      store. cmd's switches are honoured where they are cheap: /i folds ASCII
      case (the specimen has 1 lowercase `answer` line but 2 once `#define ANSWER`
      joins), /n prefixes the 1-based line number, /v inverts to the non-matching
      lines. The harness computes the expected counts in Python from the raw bytes
      rather than trusting the program's own arithmetic. A store file of hand-built
      content ("one\ntwo\nthree\ntwo\n") proves /n two (hits lines 2 and 4) and
      /v two (lines 1 and 3 lack it). The three refusals — no file, unopenable
      name, too-big ELF — exit 2 and stop the run; they never silently skip a
      file. The flag grammar is `/` + one letter exactly, same rule as fc.
- [ ] **`CD`, `MKDIR`** as built-ins, so the prompt behaves like
      `cmd` for the remaining verbs a person actually types.
- [ ] **a small Python-language interpreter** (`/bin/python`) for pure-Python
      programs — ints, strings, lists, dicts, f-strings, functions, `import` of
      modules from the store. Proven by running a program the test also writes.
      This is the rung that answers "Python scripts run here", and it is worth
      reaching.
- [ ] **`pip`-shaped install of a *pure-Python* package**: fetch over the HTTP
      client that exists, unpack with the inflate (v0.1.35) and directory-aware
      extraction (v0.1.36) that both landed, put the
      modules on the import path, run against them. The set of packages this
      can serve is exactly the set that needs no C extension and no deep
      stdlib — and the release note will list which ones, with test output.
- [ ] **`file <path>` reports a PE**: machine, subsystem, DLL characteristics
      and imported names. Honest, useful, testable, and *not* execution.

PowerShell gets attempted only after the batch interpreter proves the
parse/execute loop, and only as a named subset of the syntax.

## Visual fidelity, themes and icons

Microsoft's Windows 11 wallpaper, icon set and fonts are copyrighted and not
redistributable; downloading them into this repository would make every ISO a
pirated one, and shipping them in a release under an MIT label would be worse
than inaccurate. The rule here: **original Windows-*style* art this project
draws itself**, matching layout, metrics and behaviour — the Fluent-shaped
rounded corners, the acrylic title bars, the centred taskbar and start layout
that are already in `desktop.c`. A user who owns the assets can put them in the
store at runtime; `fetch` and `/bin/imgview` (v0.1.34) are the two halves a
wallpaper setter needs, and that is the next visual increment.

## Hardware

Bluetooth, WiFi and GPU acceleration are driver stacks against real silicon,
and QEMU can only prove so much of each. They stay on
[`ROADMAP.md`](ROADMAP.md) and move at the speed of testable evidence — the
same order the NIC went: TX (v0.1.25), RX (v0.1.26), then ICMP/UDP/DHCP/TCP/
HTTP, then a package over it. A game like Minecraft is not a scope item here;
a 3D-accelerated guest needs a GPU driver and a command buffer before any
version of that sentence is true.

## VirtualBox

[`../tools/virtualbox/create-windclone-vm.cmd`](../tools/virtualbox/create-windclone-vm.cmd)
builds the VM with the settings WindClone actually needs, and every choice in it
is a fact about this OS rather than a default:

* **no virtual hard disk** — there is no block-device driver, so the only
  writable store is tmpfs and a disk would be an object the guest cannot touch;
* **DVD = the hybrid ISO**, booted first, which is how Limine's MBR entry works;
* **NIC model 82540EM** — that *is* the e1000 the kernel drives, so the guest's
  ARP/DHCP/TCP path is speaking to a chip it knows instead of one it will never
  find;
* **UART0 = COM1 at 0x3F8, logged to a host file**, so a VirtualBox boot
  produces the same serial transcript the QEMU suite greps;
* PS/2 mouse and keyboard, `--memory 512`, no audio, no USB.

Boot it with `VBoxManage startvm WindClone`, or `--type headless` and read the
serial file. UEFI is a documented one-line switch (`--firmware efi`), because CI
already boots this same image through OVMF.

What will **not** work in VirtualBox, said before anyone discovers it: nothing
can be installed or saved across a reboot (no disk driver), the guest's network
reaches VirtualBox's NAT but has no TLS and no DNS, and the emulated display is
a framebuffer the kernel has not met before — the QEMU suite, not this script,
is the authoritative test of what boots.

## What "done" means here

A release ships only when the headless suite passes: the ISO builds, the guest
boots, the scenario types the commands, and the serial log contains the bytes we
said it would. That is what separates an OS that is daily-drivable from a demo
that looks like one, and it is the standard this project will not trade its
honesty against.
