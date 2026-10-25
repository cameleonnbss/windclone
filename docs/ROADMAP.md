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
      A `PT_LOAD` with neither memory nor bytes — what `ld` emits for a program
      with nothing writable — is skipped rather than treated as corruption.
      Known gaps: no NX (read-only code is still executable), one address
      space shared with the kernel, and `envp`/auxv are the empty placeholders
      `argv` was extended with, not a feature yet.
      *(The roadmap called this the v0.1.0 milestone, but v0.1.0 was cut at
      Phase 5 — the loader therefore ships as v0.1.1)*

## Phase 5 — Shell & processes ✅
- [x] Text shell with line editing (a 64-byte IRQ queue, Enter/Backspace),
      built-ins and a scrollback grid that survives repaints: `help`,
      `about`, `clear`. `ls` and `cat` are no longer here, and that is the
      point — they moved into `/bin` below
- [x] `hello` starts `/bin/hello` from the initrd in ring 3 and returns to
      the prompt — a command that is a real program, not a built-in
- [x] More programs in the initrd (`cat`, `ls`, `echo` as ELF files rather
      than built-ins), which needs the ELF loader. What it took, beyond
      three `.c` files:
      - **`argv` across the boundary.** `usermode_run()` now takes a command
        line and `build_stack()` lays it out on the program's own stack in the
        exact AMD64 psABI shape (`argc`, `argv[]`, `NULL`, empty `envp`,
        `AT_NULL`), with the strings above it and the whole thing bounded by
        `USER_ARG_MAX`/`USER_ARGV_BYTES` so "does not fit" is an answer
        instead of a run off the bottom of a 16 KiB stack. `_start` is also
        entered with `rdi=argc, rsi=argv`: registers are the convenient
        contract, the stack is the honest one, and a Linux binary — Phase 7 —
        reads the second and not the first.
      - **A sixth syscall, `getdirent(6)`**, which copies one fixed-layout
        `struct wc_dirent` (size, namelen, 64-byte name) by index and answers
        `-ENOENT` past the last file. That is how `ls` finds the end without
        the kernel ever learning what a directory is. The buffer is validated
        (`PRESENT` + `USER` + `WRITE`) before the struct is filled, and the
        kernel copies from its own stack, so the VFS never sees a ring-3
        pointer.
      - **`vfs_exists()`**, because the shell has to answer "is this word a
        program?" without reaching into the initrd itself.
      - **A console sink.** `vfs_set_console_sink()` routes a program's
        `write(1)`/`write(2)` into the terminal grid, wrapping and flushing on
        exit so nothing is lost. The serial port still receives every byte —
        it is what CI greps — but a human only has the window.
      - `/bin/ls` lists the archive (8 entries, sizes columned), `/bin/cat`
        copies files or reports `-ENOENT` and exits `1`, `/bin/echo` prints
        its arguments — and `echo zzz` coming back as `zzz` over a real
        `sendkey`-typed command line is the proof the whole handover works.
      - They share `src/user/wcstr.h`: eight `static inline` helpers, because
        `-nostdlib` means there is no `strlen`, no `memcpy`, and no archive to
        link one from.
- [x] `fork`-like process creation, scheduler (priority round-robin). This was
      not a function to bolt on — it first broke a hard limit every earlier
      phase had accepted: one address space and one fixed load base
      (`0x400000`) meant a second program always overwrote the first, so
      `usermode_run()` could only ever finish one before starting the next.
      - **Per-process address spaces.** `paging_clone_address_space()` copies
        the kernel's `P4` wholesale — text, data, the HHDM, the heap at 256
        GiB, the VGA window — so the kernel keeps running unchanged under
        whoever's `CR3` is loaded, then hands back a *private, empty* user
        window. Two processes now hold the same `0x400000` and never see each
        other's bytes. New page-table frames a clone allocates are recorded
        through a table sink so `proc` can reclaim them on exit
        (`paging_set_table_sink` / `paging_free_table_frames`).
      - **The loader writes into the caller's table.** `elf_load_pages()` fills
        a process's own page list, and because the image is built while the
        kernel still runs on the boot `CR3`, `fill_segment` writes each byte
        through the HHDM (`pmm_phys_to_virt`), not the user VA. `elf_load()`
        stays as a thin wrapper so the shell/boot path is byte-for-byte
        unchanged — every pre-existing CI marker still passes.
      - **A context switch with no new assembly.** `proc.c` keeps a table of
        `struct proc`, each holding a whole `struct syscall_frame`. To switch,
        the running frame is saved into its slot, the next process's `CR3` and
        working `P4` are loaded, and its saved frame is copied over the one on
        the stack — the `syscall_stub`'s `iretq` then resumes *that* process.
        The single kernel IRQ stack is re-loaded from the TSS on every ring 3 →
        ring 0 entry, so nothing of a suspended process is stranded on it.
        That is precisely why the *voluntary* switch is **cooperative** (taken
        inside a syscall, `IF` already cleared by the `0xEE` gate). Timer-driven
        **preemption** shipped afterwards — see the two bullets below; it was
        deliberately not claimed until it had a proof only it could produce.
      - **Three new syscalls:** `getpid(7)`, `sched_yield(8)`, `spawn(9)`.
        Policy is round-robin over the READY set with a numeric priority as the
        tie-break (lower runs first). `wait(10)` was reserved in `wcabi.h` and
        at first deliberately **not wired** — it fell through to "unknown call"
        rather than pretending to block. It is now real, and `kill(11)` joined
        it; see the last bullets.
      - **`/bin/tick` proves the cooperative switch, and so does CI.** The demo
        spawns two copies of one binary with different `argv` labels; each
        prints a line and yields in turn, each resuming its own counter after
        the other had the CPU. `local-test.sh` asserts the two processes' line
        ranges *cross* (each prints while the other still has lines left),
        order-free — a run-to-completion loop with extra steps could not
        produce that crossing, and which process prints first is timing luck
        once the timer can also switch.
      - **Timer-driven preemption (v0.1.4).** A tick that lands on a ring-3
        process now switches it out *without being asked*, again with no new
        assembly: for a ring-3-origin interrupt the CPU pushes `ss`/`rsp` on
        the privilege change exactly like `int 0x80` does, and `irq_common`
        saves the GPRs in the same order as `syscall_stub` — so the IRQ frame
        *is* a `struct syscall_frame`. `proc_timer_preempt()`, called from
        `irq_handler` after the PIC is acked, retires the running process
        READY and copies the next process's frame over the one on the stack;
        `irq_common`'s `iretq` resumes the other one. IRQ gates are `0x8E`
        interrupt gates, so `IF` is clear during the frame surgery and no
        second tick can re-enter it. A tick that lands on ring 0 (the kernel,
        or a process inside a syscall) changes nothing — ring 0 is never
        preempted here.
      - **`/bin/spin` proves *that*, and only preemption can produce it.** Two
        copies burn a 30-million-iteration counted loop per round and NEVER
        call `sched_yield` — cooperative round-robin would run one all the way
        to `done` before the other printed a line. Their output crossing in
        the serial log is therefore the witness, asserted with the same
        order-free line-range check.
      - **Two real ABI bugs this surfaced, both fixed.** (1)
        `usermode_enter()` / `user_exit_to_kernel()` stashed only `rsp`/`rbp`,
        so a kernel pointer a C caller lived in `r14` across
        `proc_run_all()` came back holding a dead process's context — the
        ring-3 excursion obeys the SysV AMD64 ABI only if the asm handover
        itself saves `rbx`/`r12`–`r15`, and now it does. (2) A tick landing
        between two `write` syscalls tore a marker line across the other
        process's output, so the demo programs compose each line in a buffer
        and issue ONE write via new `wc_app_*` helpers — a syscall completes
        unpreempted, which makes a single write atomic with respect to
        process switches and keeps CI's greps line-based.
      - **Blocking `wait()` + `spawn` with argv (v0.1.5)** — the process
        trilogy closed. `spawn(9)` now takes `rsi=argc, rdx=argv`; because the
        child gets a *private* address space, each argument string is copied
        twice — out of ring 3 into a kernel bounce buffer (`static char
        spawn_argbuf[USER_ARG_MAX][USER_PATH_MAX]`, validated per byte against
        the *caller's* live tables) and from there onto the child's stack by
        `proc_create`. `argc <= 0` keeps the old path-only contract. `wait(10)`
        is the first syscall that does not return into its caller:
        `proc_wait()` either reaps an existing zombie (pid + exit code, slot to
        FREE, no switch) or, when there are only live children, saves the frame
        and marks the parent `PROC_BLOCKED`; a new `SWITCH_BLOCK` arm of
        `sched_switch` gives the CPU away. `proc_create` stamps each child's
        `parent` from the running process, so only the real parent can reap it.
        A child's exit wakes a blocked parent: its pid is planted in the
        parent's saved `ctx.rax` (the yield arm's trick) and the exit code is
        staged for `wakeup_deliver()`, which writes the parent's `int *status`
        *after* the parent's `CR3` is loaded — the only moment that user page
        is both mapped and writable. When nothing is READY but something is
        BLOCKED the switch prints an honest `sched: deadlock` marker instead of
        pretending the wait was satisfied. `WC_ECHILD (-10)` joined the frozen
        error table.
      - **A real double-reap bug the timer surfaced, and fixed.** Two children
        can die before the parent's page tables next run, and the first design
        let the second exit overwrite the wake the first had already planted —
        so the parent got the *same* pid twice and the second child's zombie
        stayed `EXITED` forever. The fix adds an `armed` flag: a wake marks the
        exact zombie it speaks for, `proc_wait` skips armed slots (they are
        already promised to a pending return), and a second exit finds the
        parent no longer `PROC_BLOCKED` so it leaves its zombie unarmed — a
        normal reap candidate for the next `wait()`. One promise per zombie.
        CI reads the children's pids back from the log (they are not hardcoded,
        so moving the demo does not rot the check) and asserts the spawn pid,
        the child's own `[exit] code N pid P`, and the parent's `reaped pid P
        code N` all agree for both children, order-free.
      - **`kill(pid, sig)` (v0.1.6)** — the fourth verb, and the first syscall
        that edits *somebody else's* entry in the process table. Only `SIGKILL`
        exists: there is no delivery, no masking and no handler, so this is
        termination rather than Unix in full, and `WC_ESRCH (-3)` was added to
        the frozen error table for "no such process". The implementation is
        **synchronous**, and the reason is worth keeping: the killer is the
        running process, so every other target is by definition off-CPU — READY
        or BLOCKED, holding a saved frame and an address space no `CR3` points
        at — which lets `proc_kill()` retire it where it stands exactly as an
        exiting process retires itself (`proc_free_space`, slot `EXITED` with
        code `-SIGKILL`, `wake_parent`) without touching a trap frame. Two
        alternatives were rejected: deferring death to the victim's next syscall
        needs an "owed" flag and leaves a compute loop unkillable, and killing a
        *running* process asynchronously means surgery on the tick's frame.
        Self-kill is the one case that cannot be in-place — the victim holds the
        CPU and the frame the `iretq` is about to pop — so it routes through the
        `SWITCH_EXIT` arm and does not return. `wake_parent()` became a function
        for this: an exit and a kill owe the parent exactly the same promise.
      - **`/bin/bell`, `/bin/reaper`, `/bin/vanish`.** The victim is a child that
        works for seconds and never quits by itself, so a kill that lands is the
        only way it stops; the parent yields first to prove it was alive. The
        evidence is deliberately half absence: `[bell] done` and `[vanish] STILL
        HERE` must **not** appear, while the spawn pid, the bell's own `getpid()`,
        the kernel's `proc: killed pid P (SIGKILL) by pid Q`, and the parent's
        `reaped pid P code -9` must all name the same process. The three error
        answers are checked too — `-WC_ESRCH` for a pid already reaped,
        `-WC_EINVAL` for a signal that does not exist, `-WC_ECHILD` for a
        `wait()` with nothing left to wait for.
      - **The table now gets swept, or the boot demos would have run it dry.**
        An `EXITED` slot whose parent is gone as well (a boot-spawned process,
        or an orphan — there is no `init` to re-parent to yet) can never be
        reaped, and `PROC_MAX` is 8: four demo blocks each leaking two or three
        slots exhausts the table mid-boot. `proc_run_all()` now calls
        `proc_table_sweep()` on the way home, freeing every leftover (an
        orphaned `BLOCKED` sleeper still owns its pages) while deliberately
        *not* resetting `next_pid`, so pids are never reused and a stale pid can
        never point at somebody new. The honest limitation stays written down:
        orphans exist until a shell is everyone's parent.

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
