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
- [x] The keyboard reaches a program (v0.1.20): `read(0)` is a real call, and it
      *waits*. Until this release the keyboard had exactly one customer — the
      shell — and the reason every "editor" on this roadmap stayed a checkbox was
      that a program could not be handed a keystroke at any price: fd 0 answered
      `-ENOSYS` and IRQ1 wrote into the one ring the idle loop drained into the
      command line. What ships is the smallest thing that makes the sentence true.
      - **A claim, not a routing table.** `keyboard.c` gained a second ring
        (`KB_CAP` 64) and one owner: a pid, or 0 for the console. A process
        becomes the owner by calling `read(0)`, and a new owner starts with an
        empty ring — unread keys are *not* handed over, because they were typed
        for somebody else. One owner is enough because one window is on top, and
        it is enough because there is then nothing to leak, expire or rebalance
        when a pid is recycled.
      - **The window manager keeps veto power.** The idle loop is the only arbiter
        (the IRQ path still just queues a decoded character, so a program can never
        be handed a byte mid-scancode) and it routes a key to the owner *only*
        while `desktop_key_pid()` says that pid's program window is frontmost and
        alive. Minimise it, click past it, or let its process die while its window
        stays to show the exit code, and typing goes back to the prompt on the next
        keystroke. With no framebuffer there is no z-order to consult, so on a text
        console the claim stands by itself rather than making `read(0)` useless.
      - **Sleeping is the part that matters.** An empty keyboard does not answer 0
        and does not spin: `proc_input_read()` saves the frame and blocks through
        the *same* switch `wait()` uses, so an idle editor costs no CPU and the
        compositor keeps painting. `PROC_BLOCKED` therefore needed a reason —
        `blocked_on` is `BLOCK_CHILD` or `BLOCK_INPUT` — because one state with two
        meanings let a keystroke wake a parent asleep in `wait()` and hand it a pid
        register nobody in `read()` was waiting for. Each wake source now checks
        the reason before it touches a sleeper.
      - **The answer is written when the reader runs, not when it was woken.** The
        bytes go into the caller's buffer in `wakeup_deliver()`, under *its* CR3
        and at the moment `usermode_resume()` is about to load its frame, and the
        count is whatever is queued then — a tick, another program's slice and two
        more keys can all land between the wake and the resume, and a `read()` that
        reports fewer bytes than it could have taken is short, not wrong. Fixing
        this path exposed a bug that predates the keyboard: `proc_sched_visit()`'s
        first-entry resume is how an idle-driven process actually comes back, and it
        never called `wakeup_deliver()`, so a parent woken exactly when the slice
        budget ran out kept `wake_pending`, never got its `*status` written, and
        could not be answered a second time. Delivery now happens wherever a saved
        frame becomes a running process, not only where a switch chooses one.
      - **Death gives the keyboard back.** Both ways a process ends (the exit arm
        of the switch, the in-place arm of `kill()`) release the claim, zero the
        blocked read's user pointer — an address that stops making sense the moment
        its page tables go — and log it. A process that died holding the keys would
        otherwise leave `cap_owner` pointing at a pid nothing owns, and the desktop
        would go deaf until some other program happened to call `read(0)`.
      - **`/bin/keys` is the witness, `qemu-desktop.py typing` the proof.** It
        prints `keys: waiting for keys pid N`, sleeps, and prints the whole line the
        typist meant. The scenario types `keys` at the prompt, requires the window's
        pid and the reader's pid to be the same number, requires the kernel to say
        *`asleep until a keystroke wakes it`* (a new line: calling a parked process
        a deadlock was a lie waiting to be printed), types `hello there`, and then
        asserts four separate things: the program got the exact text back, the bytes
        summed across its reads to everything typed including the Enter (chunking is
        scheduling, not a contract, so the sum is the honest claim), the prompt
        *never* saw it (`shell: running 'hello'` and `shell: no program /bin/hello`
        both counted at zero), and after the program exited, `echo done` ran as a
        command with nobody clicking anything.
      - The cost, written down: one claim at a time, so a second program that reads
        takes the keys and the first is woken with 0 bytes and says so
        (`keys: lost the keyboard`); nothing is echoed into the program's window on
        its behalf — an editor draws its own text; no shift/ctrl/arrows reach a
        program beyond what the existing decoder already turned into ASCII; no line
        editing, so backspace is the program's problem; a full ring hands the key to
        the prompt rather than dropping it; and typing still does not reach a
        program that was never launched, because focus is a claim, not a click.
- [x] A text editor as a program window (v0.1.21): `/bin/note` opens `/tmp/demo.txt`
      by name, lets you type into it, and writes it back. It is the first feature
      that consumes everything the last three releases were about — a store that
      can write (v0.1.18), a program that can drive the shell (v0.1.19), and a
      program that can be handed the keyboard and sleep until a key lands
      (v0.1.20) — and it needed exactly three new things, none of them a syscall.
      - **Ctrl is a modifier, and a control byte is what a program gets.**
        `keyboard.c` had always decoded the ASCII table and thrown Shift's own
        scancodes away; Ctrl went into the same discard pile. It now has state
        (make 0x1D, break 0x9D), and Ctrl+letter delivers the byte the ASCII table
        already reserves for it: 0x13 for Ctrl+S, 0x02/0x06 for left/right,
        0x01/0x05 for line start/end, 0x0E/0x10 for down/up, 0x04 to delete, 0x11
        to save-and-quit. Every *other* Ctrl combination is swallowed rather than
        arriving as its bare letter, because a program that sees `s` when the user
        held Ctrl has been told a lie about what was typed. `shell_key` already
        ignores bytes below 32 except Enter and Backspace, so the prompt is
        untouched by the new traffic.
      - **A `\f` in a program's output is a screen, not a glyph.** An editor that
        appends a frame per keystroke to a 17-row log scrolls its own document out
        of the window on the seventh edit. The surface router now treats form feed
        as "this program is repainting, not reporting": it clears that slot's rows
        and logs `wm: screen cleared pid N` once, and `note` draws its whole frame —
        title bar, ruler, 12 body rows, status line — in a single 17-row `write()`.
        The serial console still receives every byte, so the tests can read the
        same window the user does.
      - **A 13 KiB image found a bug that had been sleeping since Phase 5.** The
        loader computes `from = i * PAGE_SIZE` — an offset *within the segment* —
        and used it as an offset *within the page* when it copied file bytes
        (`kv + from`). For the second page of a segment longer than 4 KiB that is a
        4 KiB-straddling write into the next frame the allocator handed out, which
        for `/bin/note` was the process's own L1 page table: the copy put ELF text
        into page-table slots, and the next translation returned a leaf of
        `0x3990b60f0040349f`. Every program in this OS had kept each PT_LOAD inside
        one page, so `from` was always 0 and nothing ever noticed. Fixed by copying
        into the page start and by counting only the bytes that belong to this page
        — the same in-page arithmetic the `.bss` clearing below it already did.
      - **What it is not, stated plainly.** No arrow keys on the wire yet (E0-prefixed
        scancodes are still swallowed; Ctrl+B/F/P/N/A/E are the stand-ins Windows
        users met in EDIT), no selection, no clipboard, no open/save-as *dialog* —
        the file name comes from the command line or a batch file, which is also how
        a `.bat` reaches it. The document was 512 bytes at this release (v0.1.23
        raised it to 2048) and 12 rows are visible, and
        there is no horizontal scroll: lines wrap in the buffer, not on screen. The
        rows are uppercased because the 5x7 font has no lowercase glyphs.
      - **The proof is the file, not the picture.** `tools/qemu-desktop.py editing`
        types `note demo` at the prompt over real PS/2, requires the window to open
        and the screen to clear, types text, moves the caret and checks the ruler
        row (`LN 1 COL 12` → `COL 21` → back to `COL 20`), edits a second line,
        shows `(MODIFIE)` become `(SAUVE)` under Ctrl+S, quits, requires the process
        to give the keyboard back and the shell to report exit 0, and requires that
        no keystroke leaked to the prompt (four specific words counted at zero). Then
        it asks the save tool to read `/tmp/demo.txt` back and compares the bytes and
        the length, so the claim is persistence through the VFS and not a screenshot.
      - **The harness needed a second monitor socket, and that is worth writing down
        because it is a testing fact every later scenario inherits.** QEMU's human
        monitor *accepts* `sendkey ctrl-e`, answers nothing, and injects no scancode:
        a scenario that typed Ctrl+S and then waited for the save was waiting for a
        keystroke that never left the emulator. QMP's `send-key` goes through the same
        8042 — hold the modifier, press the key, release both — and replies with JSON,
        so a key QEMU will not send fails the run at the key instead of 25 seconds
        later at an assertion. `Backspace` is spelled that way and `bspace` is not a
        qcode, which is how a lost delete key looked exactly like a guest bug. The
        channel lives next to the monitor socket, and the scenario reads the *raw*
        serial log for the byte comparison, because the driver puts a carriage return
        after every line feed and Python would hand that back as a second newline.
- [x] Per-cell colour in a program window, and **a code viewer** that uses it
      (v0.1.22): `/bin/view` opens a real C file out of the initrd and paints its
      keywords, types, numbers, strings and comments in different inks. The
      release is two decisions, and both were made to avoid a third.
      - **A colour request is a byte, not a syscall.** `FS` (`0x1C`) followed by a
        digit `0`–`7` moves the pen; every glyph after it lands in that ink. The
        alternative — `wc_color(fd, index)` — would have needed a new ABI number, a
        new kernel entry point and a new struct in the contract every program
        shares, and it would have been *worse*: an out-of-band request can arrive
        after the row it was meant to decorate, because writes and syscalls are
        ordered per process and a highlighter thinks in rows. FS is what ASCII
        reserved for "file separator", which is a control byte no document text has
        ever wanted, and it is the same trick `\f` was used for in v0.1.21: the
        request rides on the wire the program is already writing to. The parser
        keeps a one-byte flag rather than a two-byte window because FS can arrive at
        the end of one write and its digit at the start of the next, which is the
        normal case for a program that builds a frame row by row.
      - **One attribute byte per cell, and a palette of eight roles.** A surface
        grew an `attr[17][62]` beside its `rows`, scrolls with it, and is painted in
        *runs* of equal attribute at `x + column * 6` — the font is a fixed 5+1 grid,
        so a cell is an addressable column and a run is one `gfx_text` call instead
        of six. RGB per cell would have cost more RAM than the text it decorated, and
        a program can only name eight inks anyway; what it names is a *role* (keyword,
        string, number, comment) and not a hue, which is the right shape because
        highlighting is readable exactly when `int` and `return` are coloured for the
        same reason. Index 0 is the `0x22C55E` this OS has always drawn with, so the
        nineteen programs that never mention a colour paint the identical screen — a
        compatibility claim you can check rather than a promise.
      - **`wm: colour pid N` is logged by the parser, not by a byte counter.** The
        first version emitted it when a `0x1C` appeared in the buffer, which would
        have fired on a lone FS from a program that had not finished speaking: the
        line would have claimed a pen move that had not happened. The surface now
        latches "a complete pair was consumed", and the router reads that.
      - **The viewer classifies the whole file once, at open,** into a byte-per-byte
        table — block comment, line comment, string, char literal, number, keyword,
        type — so painting a row is a lookup and not a re-lex, which is what lets it
        repaint on every page-down. `view: highlighted 8 keywords in 56 lines` is
        counted over the file, not over the twelve rows on screen, and a file longer
        than the 2048-byte budget says `, truncated` in its own log line instead of
        quietly showing half.
      - **What it is not, stated plainly.** It is *read-only*: a highlighter that
        could also delete a user's source with one wrong key would need a dirty flag,
        an undo and a save dialog, and none of those are this program's job. Two word
        lists and a left-to-right scan, not a grammar: no nesting, no macro
        expansion, no context, and the one lie it tells on purpose is that a keyword
        inside a comment is coloured as a comment (which is what a reader wants, not
        what a compiler does). Line numbers cost five of the 56 cells, and lines
        longer than the rest of the budget are cut rather than wrapped, because a
        viewer that wraps is a viewer whose row count no longer matches the file's.
      - **The proof is the token, not the picture.** `tools/qemu-desktop.py highlight`
        types `view /src/hello.c`, requires `wm: colour pid`, and then looks for the
        escape *in front of the words it names* on the wire: `\x1c7` on the file's own
        first row, `\x1c2include`, `\x1c4` before `42`, `\x1c6int`, `\x1c3` before a
        quote. Those four tokens are on the second, third and fourth screens, so the
        three Ctrl+F page-downs that have to deliver them are what prove the whole
        file was classified and not the twelve rows that happened to be visible; the
        final `LIGNES 45-56 SUR 56` is computed from the line count the program
        reported at open, so a specimen that gains a line moves the expectation with
        it instead of breaking it.
      - **A test that measures the wrong frame has a bug in it, and this release
        found one.** The tag build of v0.1.22 failed on `damage` — four minutes
        after the *same commit* had passed it on `main`: "a mouse move still
        repainted the whole screen". The kernel had not changed and neither had the
        pixels; what was missing was a precondition. The preamble waits for the
        desktop marker, but the boot's own demo programs keep streaming into the
        terminal for seconds after it, and every one of those frames damages the
        window body — the largest sub-screen rectangle on the display. On a loaded
        runner the entire fifteen-second window was busy, so the small flip the
        scenario was looking for never existed. `wait_quiet()` now returns the log
        offset of the moment the compositor stops flipping — no flip line for two
        seconds is what a quiet desktop *is*, since a frame only happens when
        something is dirty, and it is measured on the flips rather than on the byte
        count because the idle loop prints a one-hertz heartbeat whatever happens.
        The assertion then moved from "some flip after the mark was small" to
        `first_flip_after()`: the first frame after the silence belongs to the
        cursor and to nothing else. The same precondition went into `progdamage`,
        where it closes the opposite hole — a terminal repaint from a boot program
        landing between two of `/bin/hold`'s rounds would have proved hold's damage
        without hold.
- [x] A **code editor that writes back** — one window that highlights and edits
      the same file (v0.1.23). `/bin/note` now opens a file under the name that was
      typed (`note work.c`, dot and all), paints it with the colours `/bin/view`
      paints, and Ctrl+S puts the bytes back where they came from. Four decisions,
      and the interesting ones are about what a test is actually able to see.
      - **One classifier instead of two.** `view` decided what every token was at
        open and `note` owned a mutable buffer, and an editor whose colours come
        from a different table than its own text is showing two answers about one
        file. So the tables moved into `src/user/highlight.h` as `static inline`
        functions that take the buffer as an argument, and the viewer was
        re-pointed at them — its rules did not change, and the `highlight` scenario
        passes untouched. The editor re-scans on every repaint, because a letter
        typed in front of `int` makes it `oint`, and a highlighter that still
        called it a type would be painting what the file used to be.
      - **A file had to be able to arrive.** The initrd is a boot archive and `/tmp`
        is a RAM store, and until this release nothing could carry a byte from one
        to the other: the editor could only edit what a keyboard had typed into it.
        `/bin/copy` is a *program*, not a shell builtin and not a syscall, because
        `open()` and `write()` already exist and the kernel needs no third way to
        say the same thing. It reads the whole source before it opens the
        destination, so `copy misspelled.c important.c` leaves important.c exactly
        as it was.
      - **Colour on a text file is a claim, so it is gated on the name.** Only `.c`
        and `.h` put FS+digit on the wire. That is not cosmetic: the `editing`
        scenario has compared the editor's frames since v0.1.21, and an escape in
        front of a shopping list would have made those bytes mean the highlighter
        instead of the text. `.txt` frames are byte for byte what they were.
      - **The hazard a bigger buffer brought, and the refusal.** `NOTE_DOC_MAX` is
        2048 and `TMPFS_FILE_MAX` is 4096, so a file can be longer than the document
        that opens it. Writing the part it holds back over the whole file is a
        deletion wearing the face of a save — and the store would report a clean
        write, because it would be telling the truth about the bytes it was handed.
        So `load()` keeps reading past a full buffer to *count* what it could not
        hold, the title row says `(TRONQUE)`, Ctrl+S is refused in words, the editor
        stays open, and Ctrl+Q ends with exit code 7 and the edits unsaved.
        `/src/big.c` is a specimen written on purpose to be too long.
      - **A digest, because a 62-column window cannot show a long line.** A program
        surface clips a row that is wider than it is, so the byte-for-byte
        comparison that worked on a ten-byte note cannot work on real source. The
        tests compare an FNV-1a digest over the bytes instead — computed
        independently in Python from the file on disk, and printed by the copier, by
        the editor, and by a second copier that never met the first two. The offset
        where the typed character has to land is read out of the editor's own ruler
        row rather than assumed, so a CRLF checkout and an LF one both produce the
        same expectation.
      What is still missing, and why it is the next work: the arrow keys still are
      not on the wire (E0-prefixed scancodes `keyboard.c` swallows, and putting them
      there means an escape sequence per program or a key-code channel beside the
      byte stream), there is no open/save-as *dialog* — which means a second window
      per program slot or a modal drawn inside the first — and a document is still
      one buffer, so 2048 bytes is the ceiling and paging a file through the VFS is
      the honest next cost.
- [x] The desktop **skin and a clock that tells the truth** (v0.1.24). Two code
      paths the chrome depends on, both proven without a screenshot; the rounded
      corners and the acrylic bar are paint, and paint is not a claim.
      - **A wallpaper painted once, not every frame.** The compositor had been
        grinding a vertical gradient into the back buffer on every damage rect.
        `gfx` grew an off-screen target model (`gfx_alloc_surface`,
        `gfx_set_target`, `gfx_blit_surface`) so the "bloom" is rendered a single
        time into a RAM surface of the composited size (`wm: wallpaper cached
        WxH`) and blitted per frame inside the clip. The scenario checks that size
        against the boot banner, so the kernel is measured against itself, and the
        damage maths are untouched — rects come from window geometry, not pixel
        diffs, which is why `damage` and `progdamage` still pass byte for byte.
      - **A real CMOS clock, earned the hard way.** The tray had shown a literal
        `12:00` since it was first drawn. It now decodes the MC146818 on 0x70/0x71
        and the boot log says so (`rtc: hardware clock HH:MM:SS`, `rtc: date day D
        month M year Y`). Two traps, both live: the time fields are *not* registers
        0,1,2 — the chip interleaves the alarm bytes, so seconds/minutes/hours sit
        at 0/2/4 and day/month/year at 7/8/9 — and the datasheet's binary/BCD bit
        (flag B, bit 1) is inverted by the very emulator the whole suite runs under,
        so the driver trusts the bytes over the label and re-decodes the other way
        when the flagged mode cannot be a date, while a dead chip answering 0xFF
        still fails both modes and is refused. The `chrome` scenario seeds the CMOS
        with a known instant and requires the printed hour and date to be that
        instant.
      What comes next is the phase's real edge: no network. It is being crossed one
      honest slice at a time — the driver and a proven transmit is v0.1.25, ARP and
      a proven receive is v0.1.26, an IPv4/ICMP ping round-trip is v0.1.27, UDP plus a
      DHCP lease is v0.1.28, a TCP handshake with an HTTP GET is v0.1.29, and the
      package manager over it plus a window showing the result are the releases that
      follow.
- [x] The **network card wakes up** (v0.1.25). The first honest slice of that edge:
      not a stack, but a driver that finds real silicon and moves a frame through
      it. `pci` grew a public lookup/config API (`pci_find`, `pci_bar`,
      `pci_enable_mmio_and_bus_master`), and a new `e1000` module drives the Intel
      82540EM: it maps BAR0 uncached like the framebuffer, resets the link, reads
      the burned-in MAC from the receive-address registers, and builds tx/rx
      descriptor rings — one physical frame per buffer so each is contiguous by
      construction. The boot log carries the proof (`e1000: found 82540EM`,
      `e1000: link up, mac 52:54:00:12:34:56`, `e1000: rx ring 8 desc @0x…`) and
      the boot smoke test pins the *exact* MAC, so a card that only answers zero
      fails. It does not stop at registers answering: init ends by handing the card
      one real Ethernet frame, ringing the doorbell, and refusing to report success
      until the card has DMA'd it out of memory and written the descriptor done bit
      back (`e1000: tx probe 60 bytes, descriptor done`) — and because that done
      bit lands in RAM, not in our cache, the driver flushes before every look.
      The trap worth recording: a tight guest spin never yields to QEMU's main
      loop, so SLIRP never gets the CPU to answer — which is why *receiving* is
      deliberately not claimed here. It is next.
- [x] The network **listens** (v0.1.26). The receive half, and the trap that
      shapes it: a frame coming back only exists once QEMU's user-mode network
      (SLIRP) gets CPU, which happens only while the guest sleeps in `hlt`, so a
      boot-time spin could never read one. `net` is an idle-loop pump — `net_visit`
      runs every wake-up, drains the receive ring, and re-asks on a half-second
      cadence. It speaks ARP, the smallest protocol that closes the loop: it
      broadcasts `who-has 10.0.2.2`, the reply lands through DMA, and the driver
      names the gateway MAC (`arp: 10.0.2.2 is at … - gateway resolved`). The
      scenario refuses the two lazy ways a receive test lies — an all-zero MAC
      (empty buffer) and our own NIC's MAC (the request echoed back) — and does not
      hard-code the emulator's address, because the claim is that a real, distinct
      reply came back and was understood, not that it equalled a number the test
      wrote down.
- [x] The network **pings** (v0.1.27). With the gateway MAC resolved, the client now
      builds a whole IPv4 packet by hand — header plus ICMP echo request, each with
      the Internet checksum computed over the bytes it actually sends — ships it to
      10.0.2.2, and parses the echo reply back out of the receive ring
      (`icmp: reply from 10.0.2.2: seq 0 ttl 255 - host reachable`). This is the
      first full IP round-trip: not a link-layer frame but a packet the emulator's
      own network stack accepted and answered. Getting it honest caught a real bug —
      the ICMP checksum was first written to the wrong offset, so SLIRP silently
      refused the malformed request; the fix moved the store to the checksum field,
      which is what turned "never answered" into a live reply. The scenario keeps the
      two ARP guards and adds a third: an out-of-range ttl means a mis-parse, not a
      receive, so it is rejected rather than trusted.
- [x] UDP + a **DHCP lease** (v0.1.28). The transport layer, at last: the client
      builds its first UDP datagram — an IPv4 packet (protocol 17) whose UDP header
      carries ports 68→67, a length, and a checksum over a pseudo-header — wrapping a
      BOOTP DISCOVER with the DHCP magic cookie, our MAC, and the option that asks for
      an address, broadcast to 255.255.255.255. The emulator's BOOTP server answers, the
      client matches the echoed transaction id and reads the offered address out of the
      reply (`dhcp: offer 10.0.2.15 from 10.0.2.2 - lease understood`). The address is
      now negotiated, not assumed. The scenario refuses a zero yiaddr as a mis-read, and
      the claim stays honest: this is a DISCOVER/OFFER round-trip, not yet a full
      four-step REQUEST/ACK — that would overstate what the receive path has proven.
- [x] TCP + an **HTTP GET** (v0.1.29). The transport layer with real state: the client
      runs a SYN / SYN-ACK / ACK three-way handshake against an actual HTTP server, sends
      `GET / HTTP/1.0`, and reads the response back (`http: status 200, 221 bytes read -
      first web fetch over tcp`). Each segment carries a pseudo-header checksum (protocol
      6) and the client tracks send/receive sequence numbers so the peer accepts it. The
      harness serves the page itself — a throwaway HTTP server on the host loopback that
      SLIRP exposes at 10.0.2.2 — so the connection is negotiated with a live TCP peer,
      not a mock. The honest scope: a stop-and-wait client that fetches one page, not a
      full concurrent stack. What it unlocks is the package manager, which is next.
- [x] The fetched page becomes a **package a program opens** (v0.1.30). The GET now
      keeps the whole response and hands its body to the writable store as
      `/net/fetched`, so a file that came off the network is an ordinary file a process
      can `open()`. A new `/bin/pkg` reads it back through the same VFS the editor and
      copier use, paints it in its own window, and prints an FNV-1a digest; the harness
      serves a page it knows the bytes of, recomputes the digest, and requires them to
      match — the count is the kernel's, the identity of the bytes is the proof. The
      honest scope: one fixed page fetched at boot into one buffer, not a repo, a
      resolver, or a download-on-demand tool — the primitive, wired end to end.
- [x] **Browser-style window with a real address bar** (v0.1.31). `fetch <path>` at
      the prompt queues a navigation to the idle-loop pump; the same TCP client re-arms
      on the boot's lease — new ephemeral port, new ISS, `GET <path> HTTP/1.0` — and the
      body lands in the store over the old page. `/bin/browser` paints the markup in four
      inks under a bar that shows the kernel's own record of the address, read over the
      fourteenth syscall (`net_addr`), so a ring-3 program reports a request that went on
      the wire instead of echoing one that was typed. The scenario serves a second page
      with different bytes and requires the rendered digest, the tag count and the bar
      text to all name *it*. Honest scope: plain text, one connection, no DOM — the half
      of a browser that is fetchable, not the rendering engine.
- [x] **DIR, DEL and REN reach the writable store** (v0.1.32). cmd.exe keeps these as
      internal commands, and so does this shell: `dir` lists the store then the archive
      with a byte total, `del` frees a store slot, `ren` moves a name in place — bytes
      and generation untouched, so an open descriptor keeps reading its own file. The
      archive's read-only rule gates all three (`del /bin/hello` is refused as
      read-only, not a silent program-eating success), a missing name is `ENOENT`, a
      rename onto a taken name is `EEXIST` rather than an overwrite, and a file another
      descriptor holds is `EBUSY`, checked before anything moves. Beneath: `vfs_remove`
      / `vfs_rename` + `tmpfs_rename`. The scenario copies an archive file into `/tmp`,
      counts it via `dir`, renames and re-counts (same entry), deletes and re-counts
      (one fewer), then fires both refusals. Honest scope: a flat RAM store — no
      directories, no cross-store move (that is copy-then-del), nothing survives reboot.
- [x] **A real ZIP archive unpacks into the store** (v0.1.33). `/bin/unzip` reads an
      archive whole, walks its sequential local file headers, and writes every
      **stored** member into the writable store under `<dest>/<name>`, printing an
      FNV-1a digest the test recomputes from the archive's own bytes — so a byte that
      round-tripped is proven, not counted. A **deflated** member is *refused* rather
      than corrupted (no inflate engine in a libc-less userland), a name carrying `/`
      or `..` is refused (no escaping the target directory), and an oversized or
      size-disagreeing entry is refused with its real numbers. Everything lands in the
      store, so the v0.1.32 verbs close the loop: `copy` re-reads a member through a
      second process at the same digest, and `del` removes it — a refusal against the
      same name in the read-only archive is what proves the deleted one was the store's.
      `tools/make-sample-zip.py` regenerates the committed specimen; the scenario opens
      that same file with Python's `zipfile` and derives every expectation from it, and
      this was the free 24th slot in the start menu (room is 24 on a 1024×768 panel, 23
      programs shipped before it), so `hidden 0` still holds. Honest scope: stored only,
      flat members, one archive per run — the primitive, wired end to end.
- [x] **A bitmap is decoded and blitted, not typed** (v0.1.34). A surface used to be
      glyphs only; a program now opens a second, in-band channel by writing RS
      (`0x1E`) + `R` + four hex digits for the size + `W*H` RGB24 pixels, so no ABI
      number moves and the pixel buffer stays the kernel's (one `kmalloc`'d array per
      window, freed on clear and on close). While a record is open its bytes *are*
      colours: `0x0A` mid-pixel is a green value and not a newline, which is why that
      branch is checked before every other rule in the parser. `/bin/imgview` is the
      first producer and decodes a 24-bit BI_RGB BMP honestly — four-byte row padding,
      bottom-up rows, BGR→RGB — the three mistakes a bitmap decoder actually makes; the
      scenario decodes the same committed specimen in Python and requires the guest's
      FNV-1a over the bytes it handed the kernel to match, so a shifted row, a flipped
      order or a swapped channel fails loudly instead of showing something plausible.
      Anything it cannot show (other depth, other compression, oversized, truncated
      pixel span) is refused with its real numbers. `MENU_MAX` went 24 → 28 for the 25th
      program, still under the panel's 29-row room, so `hidden 0` keeps holding. Honest
      scope: 24-bit BI_RGB, 128×80 per picture, a file on the machine — a bitmap that
      arrives as pixels and lands as pixels, not the compositor above it.
- [x] **inflate (DEFLATE) in `/bin/unzip`, so a *real* archive opens** (v0.1.35).
      `src/user/inflate.h` is a self-contained RFC 1951 decoder in freestanding C —
      stored, canned-table and dynamic-Huffman blocks, the code-length code with its
      three repeat opcodes, and the LZ77 pass with its overlapping matches — with no
      zlib, no new syscall and both bit orders (numbers low-bit-first, codes
      high-bit-first) written down rather than implied. A deflated member is written
      only if the stream decoded *and* the result is the length the header claimed;
      anything else is refused with both numbers. `tools/make-sample-zip.py` now
      hand-builds the members' streams bit by bit, because zlib only emits dynamic
      blocks and stored runs, and every stream must satisfy `zlib.decompress(...) ==
      the intended bytes` before it ships: the reference decoder certifies the test
      data, so a disagreeing guest digest can only mean the guest is wrong. The
      scenario unpacks five members (two stored, three deflated) and reads the
      canned-table one back through `copy`. Honest scope: methods 0 and 8, flat names,
      4 KiB per member, 8 KiB per archive; bzip2/LZMA entries are still refused.
- [x] **subdirectory entries in `/bin/unzip`** (v0.1.36) — a wheel's
      `demo/__init__.py` now unpacks. An entry name is taken as a relative path,
      because the store keys files by their whole path (a nested name is one more
      key, not a filesystem); a folder record (a name ending in `/`) is skipped,
      and the dangerous shape — a `..` component or a leading `/`, the zip-slip —
      is still refused by name and never written. The per-entry log line leads
      with the digest so a long path cannot push the eight hex digits (the proof)
      off the 56-byte cap, and the nested path is confirmed separately by reading
      the member back out of the store through `copy`. Getting there surfaced a
      real input bug: `keyboard.c` swallowed scancode `0xAA` as a power-on BAT
      echo, but in the set-1 stream that byte is the LeftShift *release*, so the
      first typed underscore left Shift stuck and every later key came out
      uppercase — fixed, which is what lets a program type a real `__init__.py`.
      Honest scope: methods 0 and 8 under relative paths, `..` refused not
      sanitised, this store still has no directories.
- [ ] cmd.exe-shaped batch semantics: `@echo off`, `SET` + `%VAR%`, `IF`/`FOR`,
      `ERRORLEVEL`, `>` and `|`. `bat <NAME>` runs `/tmp/NAME.BAT` line by line
      today; this is what makes a script written for Windows behave rather than
      merely echo. Proven by a typed script and a byte-for-byte serial transcript.
- [ ] the rest of the verbs a person types: `TYPE`, `CD`, `MKDIR`, `MOVE`
- [ ] a small Python-language interpreter (`/bin/python`) for pure-Python
      programs — the honest answer to "Python scripts run here". Not CPython:
      the subset is ints/strings/lists/dicts/f-strings/functions/`import`, and a
      release never claims a package it has not run. `pip install rich` needs
      TLS and a stdlib depth this userland does not have (inflate landed in
      v0.1.35, so a compressed wheel can be unpacked); the sequence and why are
      in `docs/WINDOWS-VM-PLAN.md`.
- [ ] `file <path>` reports a PE (machine, subsystem, imports) — inspection is
      honest; execution is the Wine problem and is not on this roadmap
- [ ] Images in the browser window: decode and blit a bitmap the fetched page refers to
- [ ] USB (xHCI) pointing devices, once there is a PCIe driver stack
- [ ] A settings app that looks embarrassingly familiar
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
