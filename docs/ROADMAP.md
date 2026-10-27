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
      *(read-only for four releases: `tmpfs` and `SYS_OPEN2` give the layer a
      writable half in v0.1.18 — see Phase 5, "A file layer that can write")*
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
      - **The table gets swept, or the boot demos would have run it dry.**
        An `EXITED` slot whose parent is gone as well can never be reaped by a
        `wait()` — and before v0.1.9 an orphan's zombie had exactly that problem,
        because nothing re-parented it. Adoption (see the v0.1.9 bullet) now moves
        every orphan onto the shell slot, so a leftover here is a genuinely
        parent-less boot-spawned process, and `PROC_MAX` is 8: four demo blocks
        each leaking two or three slots
        exhausts the table mid-boot. `proc_run_all()` calls
        `proc_table_sweep()` on the way home, freeing every leftover (an
        orphaned `BLOCKED` sleeper still owns its pages) while deliberately
        *not* resetting `next_pid`, so pids are never reused and a stale pid can
        never point at somebody new. Since v0.1.8 this is no longer the desktop's
        *only* reaper: a zombie whose parent is the shell is named while it is
        freed (`shell: reaped pid P code C`) and the code travels back to the
        prompt. What the sweep still frees in silence is what the shell could not
        have been the parent of — the boot demos — which is a considerably
        smaller claim than the one this bullet used to make.
      - **The shell is a process (v0.1.8): pid 1, and every program it starts is
        its child.** This replaces the paragraph that said the desktop shell was
        not wired to the table at all, and with it the three refusals that
        followed: `spawn` and `kill` answering `-WC_EPERM` to a program typed at
        the prompt, `wait` answering `-WC_ECHILD`, and no shell-started program
        ever having children. `proc_shell_start()` claims a slot in
        `shell_init()` — before one key can be typed — and takes pid 1, because
        nothing has asked for a number yet. Its state is new, `PROC_KERNEL`,
        because the shell is *not* a schedulable context: it has no address
        space, no page tables and no trap frame, since its code runs on the
        kernel stack from the idle loop. Three places must therefore refuse to
        touch it, each with the reason next to it: `pick_ready()` never selects
        it, `proc_kill()` answers `-WC_EPERM` rather than `-WC_ESRCH` (the shell
        genuinely is in the table; claiming it was never born would be the
        lazier answer), and the sweep skips it — that slot is what starts the
        next run. `proc_run_program()` replaces `usermode_run()` on the launch
        path: the program is created as the shell's child, the scheduler runs
        its whole group to the end, the sweep reaps it, and the exit code comes
        back. So what you type at the prompt or click in the menu is a process
        now, with a pid, a parent and working process verbs.
        - The evidence is cross-checked, not eyeballed. `reaper` typed at the
          keyboard really spawns a real child instead of being handed `-1`,
          really kills it, and is itself reaped by the shell with code 0.
          `/bin/whoami` prints what `getpid()` answered and CI requires that
          number to be the one `proc: spawned /bin/whoami as pid` printed a line
          earlier — a pid handed out on one side of the boundary and read back on
          the other — then tries to kill pid 1 and must be refused. The
          `menuproc` scenario makes the same claim from the mouse, by clicking
          the row.
        - A number had to become honest on the way. A shell-started program can
          now be killed, so `-SIGKILL` reaches the prompt, and the unsigned
          printer would have rendered it as a twenty-digit nothing: an ending
          reported as a clean exit. `serial_write_signed()` is now the one signed
          printer, used by the kernel's reap line and by the shell's, and typing
          `vanish` is required to come back as `shell: vanish exited with -9`.
        - The cost, written down rather than hidden: pid 1 permanently occupies
          one of eight slots, so seven are left for a program and its
          descendants. And the run is still synchronous —
          `proc_run_program()` returns only when the scheduler has nothing left
          to run, so a long program holds the screen, exactly as a long `spin`
          does. A window that owns a *live* process, so the desktop keeps
          painting while a program works, is the rest of this phase.
      - **Re-parenting, and the shell that adopts (v0.1.9).** The bullet this
        replaces said no program had been shown to outlive its parent; `/bin/orphan`
        is that program — it spawns `/bin/nest`, prints its own pid, its parent and
        the child, then exits *without waiting*, which is the whole point: a parent
        that reaps its own children never touches the adoption path. When a slot
        reaches `EXITED` (by normal exit or by kill) the scheduler now re-parents
        every live or zombie child of the dying pid onto an adopter before it wakes
        anyone (`proc_adopt_children()` → `proc_adopter_pid()`, currently the shell
        slot, with the seam left for a future `/bin/init` to take the role), so
        `proc: pid C adopted by pid A (parent P is gone)` names a parent that will
        actually come back for the code. `/bin/nest` is the witness from inside the
        adopted process: it re-reads `getppid()` each round, so the new
        syscall, `SYS_GETPPID` (12), is the ring-3 proof that a program's parent
        changed underneath it — the only answer in the ABI that can move during a
        process's life. An adoption landed in which round is scheduling luck, so
        nest only observes and the test only requires that *some* round reports the
        adopter and that the line precedes the child's own exit — no fragile
        round-number assert.
        - A latent leak surfaced on the way and was fixed, not papered over:
          `wake_parent()` used to leave the zombie it had just answered sitting in
          the table behind an `armed` flag that `proc_wait()`'s scan then skipped,
          so a woken parent could never reap the very child that woke it and the
          slot burned until the sweep. The wake *is* the reap now — the promise and
          the retirement happen in one call — which made `armed` dead code, so the
          field is gone from `struct proc` entirely.
        - The cost, honestly: pid 1 stays the sole adopter, so an orphan's exit
          code is collected by the shell rather than by a dedicated `init`, and the
          run is still synchronous. Deeper orphans (a child whose grandparent also
          dies) and a real `/bin/init` are Phase 6+ work, not claimed here.
- [x] **A file layer that can write (v0.1.18).** Up to here `write()` on a file
      descriptor was *refused*, and that refusal was the honest answer: the
      initrd is a boot archive whose files are slices of RAM the bootloader
      handed over, so a changed byte had nowhere to go. Any "text editor" built
      on that would have been a screenshot of a save dialog. This is the store
      that makes `write` mean something, and it is deliberately small:
      - **`tmpfs`** (`src/kernel/tmpfs.c`) — a fixed pool of 8 × 4 KiB named
        blobs in the kernel's own `.bss`. Static on purpose: `vfs.c`'s rule is
        that nothing allocates on the syscall path, because a syscall runs in
        whatever interrupt state the program left. **RAM only — a reboot
        forgets every byte here**, and no test claims otherwise; persistence
        across boots needs a block device driver first (ATA or NVMe — there is
        PCI enumeration but no disk yet), and that is not this release's work.
      - **A thirteenth syscall, `SYS_OPEN2` (13), not a second argument on
        `SYS_OPEN`.** A binary built against `open(path)` sets `rdi` and leaves
        `rsi` alone, so growing the old call's parameter list would have made
        whatever the compiler had lying around decide whether the program was
        about to overwrite a file. Flags are Linux's (`O_CREAT` 0100,
        `O_TRUNC` 01000, `O_EXCL` 04000); unknown bits are masked off at the
        border rather than passed down, so a program cannot invent behaviour the
        VFS did not choose. No `lseek`, no `O_APPEND`: an untested, unused API
        surface is refused, not shipped.
      - **Two stores, one path, and the archive always wins.** `open()` looks in
        the initrd first, then in `tmpfs`, so a program cannot shadow `/bin/hello`
        by saving a file under that name — the name never reaches the writable
        side. `O_WRONLY` on an archive path answers `-EPERM` (a boot archive is
        not a disk) and writing to a read-only descriptor answers `-EBADF`, which
        is what Linux says too. `vfs_exists()` asks both halves and
        `vfs_dirent()` enumerates the store *then* the archive, so `ls` and the
        start menu see one filesystem instead of one half of it. Menu rows are
        filtered on the `bin/` prefix, so a saved note can never become a
        launchable program.
      - **A generation counter per pool slot.** A slot index is a position in an
        array and a position can be recycled, so a descriptor left open on a
        removed file would otherwise start reporting the *next* file's bytes —
        not a corner-case bug but a data leak through a number a program still
        believes is valid. `live_file()` is the single gate every read, write and
        `size` passes through; `vfs_close()` deliberately bypasses it, because a
        descriptor has to be releasable even when the file behind it is gone or
        the slot leaks.
      - **The evidence runs at boot, before a program exists**: write two chunks,
        read them back through a fresh read-only `open()` of the same path,
        require `-EBADF` on a write to it, `-EEXIST` on `O_CREAT | O_EXCL`,
        `-EPERM` on `/bin/hello`, `-ENOENT` on a name nobody made, one merged
        directory listing of `initrd_count() + 1` entries whose *first* row is a
        path that exists nowhere in the tar, then remove the file and require the
        still-open descriptor to answer `-EBADF` after a *different* file has been
        created in the same slot. The check leaves the store empty, so no listing,
        menu row or program's behaviour shifts because it ran.
      - **And the userland half, from a real keyboard** (`/bin/save`, scenario
        `qemu-desktop.py filewrite`): one process writes a note and dies, a second
        process with a different pid opens the same path and prints those bytes
        back, a third sees the name and size through the merged `getdirent`. The
        scenario compares both byte counts, requires the two pids to differ, and
        requires the note's own text to sit *between* the reader's two lines —
        the shell never echoes a typed command to the serial port, so nothing but
        the store can have put it there.
      - The cost, written down: eight descriptors and eight files, 4 KiB each, no
        directories (names are flat, `tmp/x.txt` is one string), no `unlink` in
        userland, no permissions, no disk. That is what makes the roadmap's text
        editor and code editor *possible*; it is not them, and this release does
        not claim to have shipped an editor.
- [x] **Batch files: the shell obeys a file a program wrote (v0.1.19).** The
      previous release made `write()` mean something; this one makes it mean
      something *to the shell*. `save script demo about nl echo hi` puts two
      command lines in `/tmp/demo.bat`, and `bat demo` reads them back and runs
      each one through the dispatcher a typed line already went through — so a
      script gets the built-ins and the `/bin` programs exactly the way a person
      at the prompt does, and nothing about the launcher had to be forked to
      allow it.
      - **`execute()` became `run_line(line, len, origin)`.** The tokeniser and
        the built-in table are no longer tied to the shell's own input buffer: a
        typed line lives in `input`, a script line lives in `bat_line`, both are
        dispatched by the same function. `origin` is the proof — `launch()` logs
        `shell: running 'echo' from the prompt` for a keystroke and
        `from a batch file` for a script line, so a log can never claim a program
        ran from a file when somebody typed it, and the test asserts the second
        string specifically.
      - **`nl` is a newline only in script mode.** The shell tokenises on spaces
        and there is no way to type a line terminator into a command line, so the
        separator has to be a word. `save write demo the nl plan` still writes the
        two letters `nl`, because a note is text; `save script` turns the same word
        into a line break, because a script is lines. One flag on one helper, and
        the difference is what the two verbs are *for*.
      - **Three refusals, each a line in the log and not a crash.** 16 commands
        maximum (`shell: batch stopped at 16 lines, more in <path>`) because a file
        that runs forever is not a script; no `bat` inside a `bat`
        (`shell: batch refused nested script`) so the depth is two and a
        self-referring file cannot eat the kernel stack it executes on — and the
        lines *after* a refused one still run, which the scenario counts;
        `BAT_LINE_MAX` truncates a long line rather than dropping it, because a
        half command that fails is an answer where silence is a lie about what
        ran. A name that is not letters, digits or `_` is refused before anything
        is opened, so `bat ../bin/ls` answers `shell: bad batch name ../bin/ls`.
      - **The whole file is read before the first line runs**, into one kernel
        static: a script cannot change underneath itself half way through, and the
        byte count the shell read is logged against the count the writer reported
        (`shell: batch file /tmp/demo.bat (14 bytes)`), so a short read is visible.
      - **The evidence is typed, not called** (`qemu-desktop.py batchfile`): real
        IRQ1 keystrokes write the script, run it, then exercise a missing file, a
        traversal-shaped name and a nested script. The punctuation that the name
        test needs (`/`, `.`) now goes through a key *map* in the harness, and the
        assertion checks the refused name back out of the serial log — if a
        `sendkey` had been silently dropped, the logged name would not match and
        the run fails instead of testing a different command line.
      - The cost, written down: no variables, no redirection, no `@echo off`, no
        `call`, no loops, no arguments passed to a script, 512 bytes and 16 lines
        per file, 8 files per boot — and still RAM, so a script written this release
        is gone at the next reboot. `command.com` this is not; it is the smallest
        thing that makes "run that file" true.

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
      one window with a title bar and a close button — drawn back to front
      (verified by a QEMU `screendump`); it composes into a back buffer now, see
      the damage-tracking line below
- [x] Window manager: three overlapping windows, focus by clicking anywhere
      on one, title-bar buttons (minimise / maximise / close), taskbar
      buttons that restore or raise, and desktop icons that reopen a window
      that was closed. Hit testing runs front to back and stops at the first
      window that owns the press — one rule, and it is what makes overlapping
      windows behave the way people expect
- [x] Headless proof: `tools/qemu-desktop.py` injects real PS/2 moves and
      clicks through the QEMU monitor and greps the kernel's `wm:` markers
- [x] Damage tracking + double buffer: the desktop composes into a RAM back
      buffer and flips it in one pass, and a mouse move now recomputes and blits
      only a cursor-sized band instead of the whole screen (proven by a QEMU
      scenario that reads the `gfx: flip N rects, AREA px` the kernel logs).
      Since v0.1.13 the damage is per-window too: a running program repaints
      only its own window's rect, not the desktop (proven by a scenario that
      finds a sub-screen flip strictly between two of the program's rounds).
- [x] Start menu, and a launcher that starts programs from the initrd. The
      rows are not written in `desktop.c`: opening the panel enumerates the
      archive through `vfs_dirent()` and keeps the `bin/` entries, so a program
      added to the initrd is launchable with the mouse without the desktop
      hearing about it. A row click raises the terminal (that is where the
      program's stdout goes) and calls the shell's own launcher, so a click and
      a typed word resolve, run and report the same way — only the log says who
      asked. The button that used to be "show desktop" got promoted to an action
      row inside the menu, so nothing was dropped. Rows that do not fit the
      screen are counted in the footer instead of being truncated into names
      that do not exist. Proof: the kernel prints one `wm: menu row N name` per
      program, `tools/local-test.sh` requires every `USER_BINS` name from the
      Makefile to appear and `hidden 0`, and `tools/qemu-desktop.py menu` reads
      the index of `hello` out of that log, clicks *that* row, and greps
      `shell: launched 'hello' from the start menu` plus the program's own line.
      Since v0.1.8 the thing a row starts is a *process*: `menuproc` clicks
      `/bin/whoami` and requires the pid the kernel gave at spawn, the pid the
      program read back through `getpid()`, and the reap the shell owed it to be
      the same process, from the mouse
- [x] A *live* process that no longer freezes the screen (v0.1.10). A menu click
      or a typed word used to run the program to completion before the desktop
      painted again, so a long program held the whole desktop on its last frame.
      An interactive launch is now asynchronous: `proc_spawn_async()` places the
      process in the table as the shell's child and returns while it still has
      work to do, and the idle loop drives the CPU in bounded slices
      (`proc_sched_visit()`), resuming each live process from its saved frame
      (`usermode_resume()`) and handing the CPU back so the compositor paints
      between them. `/bin/hold` proves the interleave from the serial log: rounds
      of `[hold] round R pid P` before a `wm: frame painted` and after it, with
      the same pid, and its exit code arriving later on the reap. What was *not*
      done, and is the remaining half of this box: the live output still goes to
      the **one** existing terminal window (a stdout that keeps arriving, but not
      yet a window *per process*), the boot demos deliberately keep the
      synchronous `proc_run_program()`/`proc_run_all()` path (they finish before
      the idle loop visits), and the shell stays the single adopter and anchor
      these visits unwind to. A window that owns its *own* process is the next
      step.
- [x] A window that owns its *own* process (v0.1.11). The console sink is now a
      **router**: it dispatches a running program's `write(1)` on
      `proc_current_pid()` into that program's own text surface, while the
      shell's lines, the boot demos and un-launched children fall through to the
      terminal as before (serial still sees every byte, so no marker moves). Each
      surface is a real window — drag, minimise, maximise, close, taskbar — folded
      into the same `wins[]`/`order[]` arrays the fixed windows use, and it
      **outlives its process**: `shell_program_pump()` records the exit code by the
      never-reused pid, paints `EXIT n`, and the window stays until the user closes
      it. `/bin/hold`'s rounds and exit code 5 stream into its own window and it is
      still open after the process is reaped. Out of scope here: per-window *input*
      (the keyboard is still owned by the shell's line editor) — this release is
      about where a program's output goes, not about giving each program focus.
- [x] One version string. `src/kernel/version.h` is what the status window, the
      taskbar and `about` all draw, and the kernel says it on the boot log —
      which the test reads back out of that header, so a number a release left
      behind fails the run. Until now the desktop drew "0.1.0" and `about`
      answered "0.0.4", both stale by several releases: three copies of a
      version are three chances to be wrong, and an OS whose screen describes
      an older OS is lying to the person reading it
- [x] Mouse driver: 8042 aux channel, IRQ12 (cascade unmasked), 3-byte
      packets with the sign bits in the flags byte — verified headlessly by
      injecting `mouse_move` through the QEMU monitor and grepping the
      decoded packet
- [x] Cursor + one draggable window: grab the title bar, the window follows,
      clamped to the desktop (a drag flips the whole screen; a bare cursor move
      is clipped to its damage band since the double-buffer work above)
- [x] Window resizing (v0.1.14, completed v0.1.15): grab any window border and
      drag. v0.1.14 added the left, right and bottom edges and the two bottom
      corners; v0.1.15 added the top edge and the two top corners, so a window
      now resizes from all four sides. A grabbed edge follows the cursor while
      the opposite edge stays put (a top/left drag moves x/y and preserves the
      bottom/right), down to a `MIN_W`×`MIN_H` floor and a clamp to the desktop.
      A border press raises the window like any click. The top grab lives in the
      thin strip *above* `BTN_DY`, so it never clashes with the minimise /
      maximise / close buttons, while everything below it still drags the window
      — the last thing that kept the top edge out of v0.1.14. Proven headlessly
      by a real PS/2 border drag: `tools/qemu-desktop.py resize` raises the
      status window, grows its bottom-right corner, shrinks it far past the floor
      and requires it to stop at `MIN_W`×`MIN_H`, then grabs the top edge and
      grows it back taller without changing its width — all read back out of the
      kernel's own `wm: window resized WxH` line.
- [x] Resize cursor (v0.1.16): the pointer changes to a double-headed arrow over
      a draggable border — vertical on the top/bottom edges, horizontal on the
      left/right, and a diagonal on each corner — and stays a plain arrow anywhere
      a click would not resize. One hover hit-test (`hover_resize_edge`) walks the
      stack front to back and lets the frontmost *visible* window under the point
      decide, so the cursor promises exactly what the click delivers; a maximised
      window (which fills the desktop and cannot resize) keeps the arrow. Proven
      headlessly by `tools/qemu-desktop.py cursor`: it hovers the status window's
      left edge, its bottom-right corner and its body, and reads the border mask
      the kernel logs on each hover change — the geometry, not a screenshot.
- [x] Window snapping / Aero Snap (v0.1.17): drag a window's title bar onto a
      screen edge and it tiles. The cursor's *release point* picks the zone —
      left or right edge for a half, and the top or bottom corner of that edge
      for a quarter — within `SNAP_MARGIN` px of the border. The tile is computed
      from the work area (`H - TASKBAR_H`), never the full screen, so no snapped
      window can hide under the taskbar. The pre-snap rect already lives in
      `rx/ry/rw/rh`, and a drag started on a *snapped* window keeps it rather than
      overwriting it with the tile — so detaching a tiled window and snapping it
      again still restores to where the window really came from. A snapped window
      is still a normal window: it can be dragged out, resized, maximised,
      minimised and closed, and `set_maximised(0)` clears the snap. Proven
      headlessly by `tools/qemu-desktop.py snap`: a real PS/2 title-bar drag to
      the left edge, then a second drag of the tiled window to the top-right
      corner, asserting the zone code and the exact rect out of the kernel's own
      `wm: snapped <zone> <W>x<H> at <x>,<y>` line. Still open here: the snap
      *overlay* (the preview ghost Windows shows while you hover an edge), which
      needs its own damage rect and is its own next step.
- [ ] USB (xHCI) pointing devices, once there is a PCIe driver stack
- [ ] A settings app that looks embarrassingly familiar
- [ ] A text editor and a code editor as program windows — the store can write
      (v0.1.18) and a program's bytes can now drive the shell (v0.1.19), so the
      one thing still missing is userland reading the keyboard: fd 0 answers
      `-ENOSYS` and the shell owns every IRQ1 byte. Until a program can be *handed*
      the keys, an editor is a viewer with a save button, and that is not what
      this item means.
- [ ] A block device driver (ATA or NVMe), so `tmpfs` can stop being the only
      place a byte a program wrote can go

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
