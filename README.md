# WindClone

> An operating system built from scratch — Linux-style architecture,
> Windows-inspired look & feel, bootable ISO for any x86_64 machine.

![build status](https://github.com/cameleonnbss/windclone/actions/workflows/build.yml/badge.svg)

![WindClone desktop: two overlapping windows, a taskbar with window
buttons and desktop icons](docs/screenshots/desktop.png)

*The desktop as it boots: an info window and a terminal overlapping, the
terminal focused and running `/bin/hello` from the initrd in ring 3.
Screenshot taken by the test suite itself (`tools/qemu-desktop.py`).*

![WindClone start menu: thirteen program rows read out of /bin in the initrd,
above the DEMARRER button](docs/screenshots/start-menu.png)

*The start menu, opened by one click on DEMARRER. Every row is a `/bin`
entry that the kernel enumerated from the initrd at that moment — clicking
one starts it, as a process with its own pid and the shell as its parent. The
test suite clicks a row it found in its own serial log and checks the program
answered (`tools/qemu-desktop.py menu`), and clicks `/bin/whoami` to check the
pid the kernel handed out at spawn is the one the program reads back
(`menuproc`).*

**WindClone** is a hobby operating system project. Nothing is copied from
Linux or Windows: the kernel is written by us in C (with assembly for the
earliest stages), and Rust / C++ are planned for drivers and userland as the
project matures.

**The goal:** one ISO, downloadable from Releases, that boots into a system
that behaves like Windows from the user's side — a graphical shell with
windows, a taskbar and a start menu, a command line where the familiar
commands and utilities work, and enough compatibility to run real programs —
while the kernel underneath stays a Linux-style design we wrote ourselves.

⚠️ This is a learning project. Today it boots, manages its own memory
(frame allocator, page tables, `kmalloc`), handles hardware interrupts,
and ticks a timer — which is exactly where every real OS started.

## What boots today

`make` produces `windclone.iso`, a **hybrid BIOS + UEFI image** that boots on:

| Target            | Status |
|-------------------|--------|
| QEMU (BIOS)       | ✅ tested in CI |
| QEMU (UEFI/OVMF)  | ✅ tested in CI |
| Real PC, legacy BIOS CD/USB | ✅ (Limine El Torito + `bios-install` MBR) |
| Real PC, UEFI CD/USB        | ✅ |

On boot the kernel:
1. is loaded into 64-bit long mode by the [Limine](https://github.com/limine-bootloader/limine) bootloader,
2. verifies the boot memory map and higher-half direct mapping,
3. builds its own physical frame allocator (bitmap over the Limine memmap)
   and its own 4-level page tables — then maps the VGA text window (0xB8000,
   MMIO: Limine's HHDM only covers RAM) and brings up the screen console
   next to the COM1 serial one,
4. starts the `kmalloc` heap (lazily-paged arena at `0x4000000000`) and
   runs a heap self-test before anything is allowed to allocate,
5. prints firmware type + usable RAM,
6. audits the GDT, loads a 256-entry IDT with NASM exception/IRQ stubs,
   remaps the 8259 PIC and unmasks IRQ0,
7. takes the PIT timer at 100 Hz and prints a 1-second `uptime:` heartbeat
   from the interrupt-driven idle loop,
8. enumerates the PCI bus (vendor/device/class + BAR decode), arms the PS/2
   keyboard on IRQ1, and keeps a ring buffer of the last 48 boot messages —
   replayed on the serial console every 5 seconds,
9. parses the **initrd** the bootloader loaded as a module: `rootfs/` is
   packed into a plain ustar tar by `tools/mkinitrd.sh`, and the kernel walks
   it in place, lists its files and dumps `/etc/motd` — the first data in the
   system that is not part of the kernel image,
10. installs its own GDT (ring-3 selectors) and a TSS, puts a small descriptor
   table over the initrd (`open`/`read`/`write`/`close`, fd 0-2 being the
   console and fd 3+ the files), then loads `/bin/hello` and `/bin/motd` as
   real **ELF64** images — `PT_LOAD` segments honoured, `.bss` zeroed, code
   pages write-protected — and runs them **in ring 3**. They reach the
   console and the archive only through the `int 0x80` gate
   (`write`, `exit`, `open`, `read`, `close`, `getdirent`, and later
   `getpid`, `sched_yield`, `spawn`, `wait`, `kill`), every user pointer
   being checked against the page tables before the kernel touches it, and
   `exit` unwinds back to the kernel — the first privilege boundary in the
   system,
11. maps the linear framebuffer the bootloader set up (device memory, so
   uncached and outside the HHDM) and composites the **desktop**: a cached
   wallpaper (item 27; a gradient only if the surface cannot be allocated), desktop
   icons, a taskbar with a start button, and three
   overlapping windows — all drawn by the kernel with a 5x7 font it carries
   itself (no font files, no libc),
12. drives a **PS/2 mouse** on the 8042's aux channel (IRQ12, three-byte
   packets, sign bits decoded) and draws a cursor. The **window manager**
   answers it the way people expect: click a window to focus it, drag it by
   the title bar, minimise / maximise / close from the title-bar buttons,
   restore or raise it from a taskbar button, reopen a closed window from
   its desktop icon. Hit testing runs front to back and stops at the first
   window that owns the press. CI drives real PS/2 clicks through the QEMU
   monitor and greps the kernel's `wm:` markers — a headless run therefore
   proves the window manager, not just the mouse,
13. runs a **command prompt in a window**: a terminal with a line editor and
   a scrollback grid, focused by clicking it. Only `help`, `about` and `clear`
   are built in; anything else is looked for in `/bin` and started as a real
   program with the rest of the line as its `argv` — `ls` lists the archive
   through `getdirent`, `cat /etc/issue` prints a file, `echo zzz` answers
   `zzz`, and whatever a program writes lands in the terminal. CI types the
   commands through the emulated keyboard and greps what they printed.
14. runs **two programs at once**, and now **interrupts them mid-compute**.
   Each process gets its own 4-level page tables
   (`paging_clone_address_space()` shares the kernel's view, keeps the user
   window private), so two images can sit at the same `0x400000` and never
   see each other. The **round-robin scheduler** (`proc.c`) switches between
   them by saving a process's `syscall_frame`, loading the next one's `CR3`,
   and letting the return frame's `iretq` resume it — no new assembly, twice:
   a voluntary switch rides the `int 0x80` frame, and a timer tick landing on
   ring 3 rides the IRQ frame, which the CPU builds with the *same* layout.
   The boot spawns two copies of `/bin/tick` (print-and-yield) and two of
   `/bin/spin` (a 30-million-iteration hot loop that never yields); CI
   asserts the two processes' output line ranges *cross*, which a
   run-to-completion loop cannot fake — for the spins it is the only way they
   can interleave at all, so the tick really does preempt.
15. lets a program **not come back from a syscall**. `wait()` is the first
   call in this kernel that does not return into its caller: `/bin/waiter`
   spawns `/bin/exit 7` and `/bin/exit 9` — spawn now carries a real `argv`
   through kernel bounce buffers into the child's private address space —
   then sleeps as `PROC_BLOCKED` while the children burn their turns, and
   each child's exit wakes it with that child's pid, its exit code written
   through the pointer under the *parent's* page tables. The zombie's table
   slot survives until the parent reaps it, so `spawn → exit → wait → reap`
   closes the process trilogy the way Unix does it; CI cross-checks the
   pid/code pairs the parent saw against the ones the children printed.
16. lets a program **end another one**. `kill(pid, SIGKILL)` is the fourth
   verb: `/bin/reaper` spawns `/bin/bell` — a child that works for seconds and
   never quits by itself — yields until the bell demonstrably prints, kills it,
   and reaps it. The victim is off-CPU by definition (READY or BLOCKED), so the
   kernel retires it where it stands: address space back, slot marked `EXITED`
   with code `-SIGKILL`, the waiting parent woken. `/bin/vanish` covers the one
   case that cannot be a table edit — killing yourself holds the CPU and the
   trap frame, so it rides the exit arm and does not return. CI proves both by
   absence as well as presence: `[bell] done` and `[vanish] STILL HERE` must
   **not** be in the log, while the reaped pid and its `-9` must.
17. opens a **start menu** that is a launcher, not a picture of one: the rows
   are the `/bin` entries of the initrd, enumerated through the VFS each time
   the panel opens, so a program added to the archive is clickable without the
   desktop knowing its name. Clicking a row runs that program through the same
   path the command prompt uses — same lookup, same terminal for its output,
   same exit code — and the test proves it by reading the row index out of the
   kernel's own log line and clicking *that*, then checking `/bin/hello`
   answered. Nothing in `desktop.c` contains a program name.
18. gives the **command prompt a pid**. The shell used to drive the process
   table from outside it, and three refusals were the consequence: a program
   typed at the prompt could not `spawn`, `wait` or `kill`, because it had no
   entry in the table to be anybody's parent or child. `shell_init()` now claims
   one — pid 1, state `PROC_KERNEL`, no address space and no trap frame because
   its code runs on the kernel stack — and every program the prompt starts or
   the menu clicks is created as its child and run by the real scheduler.
   `/bin/whoami` is the proof from the other side of the boundary: the pid it
   reads back with `getpid()` must be the number the kernel printed when it
   spawned it, and its `kill(1)` must come back `-EPERM` rather than `-ESRCH`,
   because the shell genuinely is there. Typing `vanish` proves the other half:
   a program the shell starts can now be *killed*, so the `-9` arrives at the
   prompt and has to be reported as `-9` instead of a tidy, invented `0`.
19. **adopts an orphan.** A parent that dies while its child still runs used to
   leave that child pointing at a dead pid no `wait()` could ever name, so its
   exit code went unread. When a slot reaches `EXITED` — by exit or by kill — the
   scheduler now re-parents every one of its children onto an adopter (the shell
   slot today, the seam left for a future `/bin/init`) before waking anyone, so
   the adopter collects the code. `/bin/orphan` is the proof: it spawns
   `/bin/nest`, exits without waiting, and `/bin/nest` re-reads the new
   `getppid()` (`SYS_GETPPID`) each round to show, from inside the adopted
   process, that its parent changed underneath it — then its exit code 7 comes
   back through the shell's reap line.
20. **launches without freezing the screen.** A menu click or a typed word used
   to enter ring 3 and not come back until the program — and everything it
   spawned — had exited, so a program that kept running kept the whole desktop
   on its last frame: no repaint, no cursor, nothing. An interactive launch is
   now *asynchronous*: `proc_spawn_async()` places the process in the table as
   the shell's child and returns while it is still about to work, and the idle
   loop drives the CPU in bounded slices (`proc_sched_visit()`), resuming each
   live process from its *saved* frame (`usermode_resume()`) rather than
   restarting it, then hands the CPU back so the compositor paints before the
   next slice. `/bin/hold` is the witness: twenty rounds of `[hold] round R pid P`,
   each a yield and a short busy stretch, so its output streams into the terminal
   across several visits, and the serial log shows a `[hold]` round, then a
   `wm: frame painted`, then a later round of the same pid — the screen turned
   over *while* the process ran. The exit code arrives later than the launch, on
   the visit that finally runs it to its end, through the reap line. The boot
   demos keep the old synchronous `proc_run_all()` path untouched — they finish
   before the idle loop ever visits — and the output still goes to the one
   existing terminal window: a window that owns *its own* process is the next
   step, not this one.
21. gives a launched program **a window of its own.** The whole kernel used to
   have one console sink, so every program's `write(1)` landed in the single
   terminal grid. That sink is now a **router**: it reads `proc_current_pid()`,
   and the bytes a running program writes go to *that program's* text surface
   instead of the terminal, while the shell's own lines, the boot demos and any
   un-launched child still fall through to the terminal exactly as before (the
   serial port has already received every byte, so no `wm:` assertion changes).
   Each surface is a full desktop window — the same draggable title bar,
   minimise / maximise / close, and taskbar button the fixed windows use,
   because the program windows live in the very same `wins[]` / `order[]` arrays.
   And the window **outlives its process**: when the program stops owning a CPU,
   `shell_program_pump()` looks up its exit code by the never-reused pid, paints
   `EXIT n` under its final output, and logs `wm: program window pid P finished
   code C, N bytes`. The window stays on screen until the user clicks its close
   button, which frees the text slot — so dismissing a window is a choice, never
   a signal, and it does not touch the process. `/bin/hold` again is the witness:
   its twenty rounds and its exit code 5 stream into its own window, which is
   still open after the process is gone. Six such windows fit; the seventh launch
   still runs, its output just falls back to the terminal rather than vanishing.
22. composes **into a back buffer and repaints only what changed.** Every
   `gfx` primitive used to write straight into the mapped framebuffer, so a
   frame could be seen half-drawn, and one change — even a single-pixel cursor
   move — recomposed all 786 432 pixels of wallpaper, windows, taskbar and
   pointer. The primitives now draw into a **back buffer in RAM** (a few MB the
   kernel `kmalloc`s once the heap is up), so a visible frame is pushed to the
   display whole, in one `gfx_flip`; the compositor's work is invisible. That
   buffer persists between frames, so a repaint only recomputes the region that
   actually moved: `desktop_frame` builds a small set of **damage rectangles**,
   clips the full layered recompose to their bounding box, and blits just those
   boxes. A pure mouse move now touches a cursor-sized band, not the screen —
   while a click, a drag, the menu, or a program's output are structural and
   still flip the whole desktop, exactly as before. Recomposing the *whole*
   layer stack clipped to the damage box (rather than diffing pixels) is what
   keeps overlapping windows and the cursor's vacated trail correct by
   construction. If a machine cannot give us the buffer, the primitives fall
   back to drawing the framebuffer directly and every frame repaints whole —
   degraded, never blank. Each painted frame logs `gfx: flip N rects, AREA px`,
   and the tests read that number to prove a mouse move really costs a fraction
   of a full screen instead of trusting the claim.
23. lets a **running program repaint only its own window.** The back buffer made
   a mouse move cheap, but a program streaming output still forced a whole-screen
   flip: `desktop_frame` counted *any* program's new bytes as a structural change.
   Now each program surface carries its own dirty flag, and the compositor turns
   a dirty window into a single damage rectangle — the same trick the terminal
   body already used. The full layer stack is recomposed clipped to that box, so
   a window partly hidden behind another is still composited correctly, and only
   the window's rect is pushed to the screen. A running `/bin/hold` now turns
   over its 500x240 body instead of all 786 432 pixels. Because per-window damage
   means more rectangles per frame, the damage set grew and gained an overflow
   path: a frame that wants more rects than fit blits the whole union box at once
   — always correct (the pixels outside it are last frame's, unchanged), just a
   larger blit. The `progdamage` scenario proves it the honest way: it requires a
   sub-screen flip to land *strictly between two of hold's own rounds*, which is
   exactly the frame its output caused and which used to be full-screen.
24. runs **a text editor** — `/bin/note`. Type `note demo` at the prompt (or put
   it in a batch file) and a program window opens titled `NOTEPAD - /tmp/demo.txt`:
   it reads the file if the store has it, edits it in memory if it does not, and
   writes it back with Ctrl+S. That sentence needed three things the OS did not
   have. A program had to be able to *move* the caret, so `keyboard.c` now tracks
   Ctrl as a real modifier and turns Ctrl+letter into the control byte the ASCII
   table already reserves for it (Ctrl+S → 0x13, Ctrl+A/E for line start/end,
   Ctrl+B/F for left/right, Ctrl+D to delete, Ctrl+K to save-and-quit); the other
   Ctrl combinations are swallowed instead of being delivered as their letters. A
   program's window had to be able to show *its own* screen rather than a running
   log, so a `\f` in a program's output clears that window's surface — one screen
   per draw, 17 rows in a single write, so an editor repaints instead of scrolling
   away from itself. And the whole thing had to be one program using only syscalls
   that already exist: `open`/`read`/`write`/`close`/`getpid`, no new ABI, no
   runtime. Getting a 13 KB image to load also dug up a real kernel bug: the ELF
   loader treated a *segment*-relative offset as an *in-page* offset, so the
   second page of any segment longer than 4 KiB was copied 4 KiB past the end of
   its own frame — into the next frame the allocator had just handed out, which
   was the process's own page table. Every image before this editor kept each
   segment inside one page, so it had never fired. What the editor did *not* have
   at this release: arrow keys (Ctrl+B/F/P/N/A/E stand in), a selection, an
   open/save-as *dialog*, and any document longer than 512 bytes. The `editing`
   scenario drives the real PS/2 path — type the command, type text, press Ctrl+S,
   quit — and then reads `/tmp/demo.txt` back through the save tool, so the proof
   is that the bytes are still there, not that a window looked right.

25. paints **a syntax-highlighting code viewer** — `/bin/view`. Type
   `view /src/hello.c` and a program window opens on a real C file that ships in
   the initrd, with the keywords in cyan, the types in violet, the numbers in
   magenta, the strings in yellow and the comments dimmed. Colour needed one byte
   and no syscall: a program writes FS (`0x1C`, the control byte ASCII reserved
   for "file separator" and that no document text has ever wanted) followed by a
   digit `0`–`7`, and every glyph after it lands in that ink. It is in-band for
   the same reason `\f` is — the request travels on the same wire as the text it
   decorates, so it cannot be reordered against the row it belongs to, and the
   eight inks are a table in `shell.c` rather than RGB in every program. Index 0
   *is* the green this OS has drawn text with since the first window, which is why
   the nineteen programs that never mention a colour paint exactly the screen they
   painted before. The kernel keeps one attribute byte per cell, scrolls the
   attributes with the rows, and draws a row in runs of equal ink at
   `x + column*6` — the font is a fixed 5+1 grid, so a cell is an addressable
   column and not a pixel count. `wm: colour pid N` is logged by the parser when
   it *consumes* a complete pair, not by a byte counter that would fire on a lone
   FS. Like `/bin/note` this is one program on syscalls that already exist, and it
   was read-only on purpose: editing a highlighted file was the next step, and
   item 26 is that step. The `highlight` scenario types the path, then pages down
   with Ctrl+F three times, and asserts on the wire that the escape sits in front
   of `include`, `42`, `int` and a string quote — tokens that live on the second,
   third and fourth screens, so the page-downs are what prove the whole file was
   classified and not merely the twelve rows that happened to be visible.

26. edits **the file it highlights** — `/bin/note` put on the viewer's colours and
   a real file's name, and the two programs started sharing one classifier. The
   viewer and the editor are still two programs; what merged is the code that
   decides what a token is. None of it needed a syscall. The classifier moved
   into `src/user/highlight.h`, a header of `static inline` functions with the
   buffer passed in as an argument,
   so the viewer's read-only table and the editor's mutable document decide what a
   token is by the same code — colours that came from two tables would be two
   claims about the same bytes. `/bin/note` grew a 2048-byte document (the viewer
   already held that much; an editor smaller than its own viewer cannot open what
   it claims to edit), accepts one dot in a name so `note work.c` opens
   `/tmp/work.c` instead of inventing `/tmp/work.c.txt`, and paints with the
   FS+digit escape only when the name ends `.c` or `.h` — which is why every frame
   the `editing` test has checked since v0.1.21 is still byte for byte what it
   was. And `/bin/copy` is a *program* that moves a file from the initrd into the
   writable store, because the kernel already has `open` and `write` and needs no
   third way to say the same thing. It reads the whole source before writing a
   byte, so a mistyped name cannot destroy the destination.
   What came with a bigger buffer is a hazard a 512-byte note never met: a file
   can be longer than the document, and half a file written back over a whole one
   is a deletion that reports success — the store would be telling the truth about
   the bytes it was handed. So a document that is not the whole file says
   `(TRONQUE)` on the title row, refuses Ctrl+S out loud, and quits with exit code
   7 and its edits unsaved. Proving any of that needed a number a terminal cannot
   give: a program window is 62 columns wide and clips a source line silently, so
   the tests compare an FNV-1a digest over the bytes instead — printed by the
   copier, by the editor, and by a second copier that never met the first two.
   `/src/big.c` is a specimen written to be too long. The `editcode` scenario
   copies `/src/hello.c` into the store, opens it as source, checks the digest
   agrees three ways, presses Ctrl+E and reads the caret's column out of the
   editor's own ruler to work out where a typed `x` has to land, saves, and has a
   second process digest the result. The `edittrunc` scenario opens the specimen,
   makes the document dirty with one Ctrl+D, requires the save to be refused, and
   then proves the file still digests to exactly what it was before the editor saw
   it.

27. wears a **Windows-style skin** that is honest about what is paint and what is
   proof. The desktop is no longer a flat vertical gradient: the wallpaper is
   painted *once* into an off-screen RAM surface of exactly the composited size and
   blitted per frame inside the damage clip (`wm: wallpaper cached WxH` — the
   scenario checks that WxH against the boot banner, so the kernel is measured
   against itself), window corners are rounded and the taskbar is a translucent
   acrylic bar. Those are pixels, and pixels are not asserted. What *is* asserted
   is the tray clock, which stopped being the hard-coded `12:00` it had been for
   six releases and now decodes the real MC146818 CMOS chip on ports 0x70/0x71
   (`rtc: hardware clock HH:MM:SS`, `rtc: date day D month M year Y`). The driver
   earns its keep on two facts a naive one gets wrong: the clock fields are not
   registers 0,1,2 — the chip interleaves the alarm bytes, so seconds, minutes,
   hours sit at 0,2,4 and the date at 7,8,9 — and the datasheet's binary/BCD flag
   is *inverted* by the emulator every one of these tests runs under, so the driver
   trusts the bytes over the label (it re-decodes the other way when the flagged
   mode cannot be a real date) while still refusing a dead chip that answers 0xFF
   to everything. The scenario seeds the CMOS with a known instant and requires the
   hour and the date the kernel prints to *be* that instant, not merely to look
   plausible.
28. drives a real **Intel 82540EM network card**. The kernel finds the card on the
   PCI bus (`8086:100e`), turns on its memory space and bus mastering, maps BAR0
   uncached the same way the framebuffer is mapped, resets the link, and reads the
   burned-in MAC straight out of the receive-address registers (`e1000: link up,
   mac 52:54:00:12:34:56` — the scenario pins that exact address, so it fails if the
   registers only answer zero). It then stands up transmit and receive descriptor
   rings, giving every buffer exactly one physical frame so each is contiguous by
   construction and its DMA address is its own base (`e1000: rx ring 8 desc @0x…`).
   And it does not stop at registers answering: at the end of init it hands the card
   one real Ethernet frame, rings the doorbell, and refuses to claim success until
   the card has read that frame out of memory and written the descriptor's done bit
   back (`e1000: tx probe 60 bytes, descriptor done`). Because a done bit lands in
   RAM and not in our cache, the driver flushes before every look — the blunt,
   correct version of the per-descriptor flush a real driver uses. That release
   proved the card could *send*; the next one had to prove it could *hear*.
29. **hears the network come back**. Sending was half the story; a frame the other
   way only exists once QEMU's user-mode network (SLIRP) gets CPU time, and that
   happens only while the guest sleeps in `hlt` — so nothing here busy-spins. The
   idle loop is the pump: `net_visit` runs every wake-up, takes whatever landed in
   the receive ring, and re-asks on a half-second cadence. It speaks ARP, the
   smallest protocol that closes the loop (no checksums, and SLIRP always answers a
   `who-has` for its gateway). The client broadcasts `who-has 10.0.2.2`, the reply
   arrives through DMA, and the driver parses it and names the gateway's MAC
   (`arp: 10.0.2.2 is at 52:55:0a:00:02:02 - gateway resolved`). The scenario is
   ruthless about the two ways a lazy receive test lies: it rejects an all-zero MAC
   (parsed from an empty buffer) and rejects our *own* NIC's MAC (that would be the
   request echoed back, not an answer from another host). It does not hard-code the
   emulator's address — the witness is simply that a real, distinct reply came back
   through the card and was understood.
30. **pings its gateway in IPv4**. With the gateway's MAC known, the client builds a
   whole packet by hand: an IPv4 header and an ICMP echo request, each carrying the
   Internet checksum (one's-complement of the 16-bit one's-complement sum) computed
   over the bytes it will actually send. It ships that frame to 10.0.2.2, and when
   SLIRP's echo reply arrives it parses the header length, checks the packet is
   really addressed back to us, and reads the time-to-live off the live hop
   (`icmp: reply from 10.0.2.2: seq 0 ttl 255 - host reachable`). This is the first
   full IP round-trip in the kernel — not a link-layer frame but a packet the
   emulator's own network stack accepted and answered — and it is the rung under
   DHCP and TCP. The test guards the same two lies as ARP did and adds a third: a
   reply whose ttl is out of range means the packet was mis-parsed, not received, so
   the scenario rejects it rather than trusting a green line.
31. **negotiates a lease over UDP (DHCP)**. Above ICMP sits the transport layer, and
   the client sends its first datagram: an IPv4 packet (protocol 17) whose UDP header
   carries the client port 68 to the server port 67, a length, and a checksum computed
   over a pseudo-header — the two addresses, the protocol and the length — as well as
   the payload. Inside is a BOOTP DISCOVER with the DHCP magic cookie, our MAC, and the
   options that ask for an address, broadcast to 255.255.255.255. The emulator's own
   BOOTP server answers, the client matches the echoed transaction id and reads the
   offered address back out of the reply (`dhcp: offer 10.0.2.15 from 10.0.2.2 - lease
   understood`). For the first time the address is *negotiated*, not assumed — and the
   scenario refuses a zero yiaddr, which would mean a mis-read rather than a real lease.
   The one thing this deliberately does not fake is a full four-step REQUEST/ACK: the
   honest claim here is that a UDP datagram we built was accepted and answered.
32. **fetches a web page over TCP (HTTP)**. The transport layer with real state: the
   client runs a three-way handshake — SYN, SYN/ACK, ACK — against an actual HTTP
   server, then sends `GET / HTTP/1.0`, reads the response, and parses its status line
   (`http: status 200, 221 bytes read - first web fetch over tcp`). Every TCP segment
   carries a checksum over the same pseudo-header the UDP datagram used, with protocol
   6 this time, and the client tracks send and receive sequence numbers so the peer's
   stack accepts it. The harness serves the page itself: it starts a throwaway HTTP
   server on the host loopback, which the emulator's SLIRP exposes to the guest at
   10.0.2.2, so the connection is genuinely negotiated with a real TCP peer and not a
   mock. This is the primitive a package manager is built on — pull a file over HTTP —
   and the claim is scoped to what is proven: a stop-and-wait client that fetches one
   page and reads 200 back, not a full concurrent stack.
33. **turns the fetched page into a package a program opens (v0.1.30)**. Reading a
   status line is a proof the transport works; keeping the body is what a package
   manager is for. The client now accumulates the whole response — status line,
   headers, the blank line that ends them, and the bytes after it — and hands that
   body to the writable store as `/net/fetched`, so a file that arrived off the
   network becomes an ordinary file (`http: stored 43 body bytes into /net/fetched -
   a page installed from http`). A new `/bin/pkg` then opens it through the same VFS
   the editor and copier already use, paints it into its own window, and prints an
   FNV-1a digest of what it read. The headless test serves a page whose bytes it
   knows, recomputes that digest itself, and requires the rendered one to match: the
   count is the kernel's, but the *identity* of the bytes — TCP to the store to the
   VFS to a pixel on screen — is what makes this a package and not a picture of one.
34. **navigates on request, in a browser window with a real address bar (v0.1.31)**.
   One boot-time page is a download; a browser answers *this* address, *now*. `fetch
   /second.html` at the prompt queues the path to net.c, the same TCP client re-arms
   on the boot's lease with a fresh ephemeral port and ISS, builds `GET /second.html
   HTTP/1.0`, and the body replaces the first page in the store. A new `/bin/browser`
   then paints the markup — tags, names, quoted values and text in four inks — under
   a bar whose text is not an echo of the keyboard: a ring-3 program cannot call the
   kernel, so syscall 14 (`net_addr`) copies tcp.c's own record of the address it put
   on the wire into the program's buffer. The headless test serves a second page whose
   bytes differ from the boot page, and requires the rendered digest to match *it*, the
   tag lexer to have counted six tags, and the bar to read `10.0.2.2:8080/second.html`:
   request line, stored body and painted page are then one navigation, not three hopes.
   The honest scope: plain-text pages over one stop-and-wait connection, not a DOM, a
   renderer, or TLS — an address bar and the bytes behind it.
35. **answers its own file verbs — DIR, DEL, REN (v0.1.32)**.
   cmd.exe keeps these as *internal* commands rather than shipping three .exe's, and
   so does this shell: `dir` lists the writable store and then the archive with a byte
   total, `del` frees a store file's slot, `ren` moves a name in place while the bytes
   — and the slot's generation, so an open descriptor still reads its own file — stay
   where they are. The archive's rule reaches all three: `del /bin/hello` is refused
   as *read-only* rather than silently deleting a program from a store that never held
   it, and `del /tmp/ghost` says *no such file*; a rename onto a taken name says *the
   new name is already taken* instead of overwriting, because this shell has no prompt
   to ask with. Beneath them, `vfs_remove`/`vfs_rename` in the VFS and `tmpfs_rename`
   in the store — a file another descriptor still holds is `-EBUSY`, checked *before*
   anything moves, so a refused rename changes nothing. The headless test copies a
   real archive file into `/tmp`, watches `dir` count it, renames it and counts the
   same entry, deletes it and counts one fewer, then fires both read-only refusals.
   The honest scope: a flat RAM store, so no directories, no move across stores (that
   is copy-then-del, and `copy` already ships), and nothing survives a reboot.

36. **unpacks a real ZIP archive (v0.1.33)**.
   `unzip /src/sample.zip /tmp` reads the whole archive into memory, walks its
   sequential local file headers, and for every **stored** (uncompressed) member
   writes the bytes into the writable store under `<dest>/<name>` — each one
   printed with an FNV-1a digest the test recomputes from the archive's own bytes,
   so "unzipped" means byte-for-byte and not merely "a count appeared". A
   **deflated** member is *refused* rather than corrupted: this userland carries no
   inflate engine, and an extractor that quietly wrote garbage under the original
   name would be worse than one that says what it cannot do. Names are kept flat —
   an entry carrying `/` or `..` is refused, so no archive can make it escape the
   directory it was pointed at — and everything it unpacks lands in the store, which
   is why the v0.1.32 verbs finish the proof: `copy` reads a member back out through
   a *second* process at the same digest, and `del` removes it (a name the read-only
   archive also holds is refused, so a successful delete proves it was the store's).
   `tools/make-sample-zip.py` regenerates the committed specimen; the test opens that
   same file with Python's own `zipfile` and derives every expectation from it. The
   honest scope: stored entries only (no inflate yet), flat members (no subdirectory
   trees), one archive per run — the primitive, wired end to end and proven by a
   digest rather than a length.

37. **paints a real bitmap into a window (v0.1.34)**.
   Every window so far was made of glyphs, which is a terminal and the right thing
   for `ls` — but a picture is not a sequence of glyphs. The surface gained a second,
   in-band channel: a program writes RS (`0x1E`, the byte ASCII reserved for exactly
   this), then `R`, then four hex digits for width and height, then `W*H` RGB24
   pixels. No new syscall and no new ABI number to freeze, and the pixel buffer is
   the *kernel's* (a `kmalloc`'d array per window, freed when the window clears or
   closes) rather than a fifth of the program's own 256 KiB image. Once a record is
   open its bytes are colours and nothing else — a `0x0A` inside a pixel is a green
   value, not a newline — so that branch runs before every other rule in the byte
   parser. `/bin/imgview /src/logo.bmp` is the first producer: it decodes a 24-bit
   uncompressed BMP for real, including the four-byte row padding, the bottom-up row
   order (the first row in the file is the *bottom* of the picture) and the BGR that
   must become the framebuffer's RGB — the three bugs a bitmap decoder actually has.
   The test decodes the same committed file in Python itself and requires the
   guest's FNV-1a digest over the bytes it handed the kernel to match, so a wrong
   stride, a flipped order or a forgotten channel swap fails instead of displaying
   something plausible. Any other layout is *refused with its real numbers* rather
   than painted as garbage. The honest scope: 24-bit BI_RGB only, capped at
   128x80 pixels per picture, one image per run — pixels on screen, requested and
   verified byte for byte, not a scaled/alpha/blt-capable compositor.

An unhandled exception prints a full register dump (rip/cs/rflags + all GPRs)
plus a frame-pointer backtrace on the serial console instead of silently
triple-faulting.

## Quick start

You need a Linux build host (native, WSL2, VM, or Docker — see
[`REQUIREMENTS.md`](REQUIREMENTS.md)).

```sh
# 1. one-time: fetch the bootloader pieces
./tools/fetch-limine.sh

# 2. build kernel + ISO
make

# 3. boot it in QEMU (serial console included)
make run

# bonus: watch the two-stage NASM bootloader: MBR loads stage-2 from disk
make mbr && make run-mbr
```

Write the ISO to a USB stick to boot it on real hardware:

```sh
cp windclone.iso /dev/sdX     # careful: this overwrites the whole device
```

## Releases

Every `v*` tag runs the same smoke suite as CI and publishes the bootable ISO
to [Releases](../../releases) — the image attached is the one that just booted
on BIOS and UEFI, with a `SHA256SUMS` beside it:

```sh
git tag v0.1.0 && git push origin v0.1.0
```

## Repository layout

```
src/kernel/        C kernel: entry (kmain), console + memory managers,
                   IDT/PIC/PIT, PCI + PS/2 drivers, initrd/VFS, ELF64 loader,
                   per-process page tables + preemptive round-robin scheduler
                   (proc), interrupt stubs (NASM), string + port-I/O helpers
src/user/          the programs that run in ring 3 — hello, motd, ls, cat,
                   echo, tick, spin, exit, waiter, bell, reaper, vanish,
                   whoami, orphan, nest, hold, save, keys, note, view, copy — with no libc of their own: a header of inline
                   `int 0x80` wrappers and ten static inline string and digest
                   helpers
src/boot/          hand-written NASM boot code (two-stage educational MBR)
rootfs/            what goes into the initrd: packed to a ustar tar, loaded
                   by the bootloader as a module, read in place by the kernel,
                   and — since `/bin/copy` — a place a file can be edited *from*
linkers/           ELF64 linker scripts — kernel.ld (higher-half kernel),
                   user.ld (programs linked at 0x400000, small model)
vendor/limine.h    vendored bootloader API header (BSD-0, upstream Limine)
tools/             fetch-limine.{sh,ps1}, build-iso.sh, mkinitrd.sh,
                   local-test.sh (smoke suite, same one CI runs),
                   qemu-desktop.py (drives the desktop with real PS/2 clicks)
limine.conf          boot entries shipped inside the ISO
docs/              ROADMAP.md, CONTRIBUTING.md, screenshots/
.github/workflows/ CI: build ISO + boot smoke tests in QEMU
Makefile           kernel, ISO, MBR, run targets
```

## Roadmap (short version)

The full plan with checkboxes lives in [`docs/ROADMAP.md`](docs/ROADMAP.md).

1. ✅ Boot to long mode, serial + VGA console
2. ✅ GDT audit, IDT + exceptions, PIC remap, PIT timer uptime
3. ✅ Physical + virtual memory managers (`pmm`, own page tables, `kmalloc`)
4. ✅ PS/2 keyboard, PCI enumeration
5. ✅ Initrd, VFS, ELF64 processes, nine syscalls, `cat`/`ls`/`echo` as real
   programs, and a preemptive round-robin scheduler running two processes at
   once with the timer switching them
6. ⏳ Shell in C, drivers starting to move to Rust
7. ⏳ POSIX/Linux compatibility layer (ELF loader + Linux syscall subset)
8. ⏳ Graphical "windows" shell — earning the *Wind* in WindClone

## Why these choices?

* **Limine instead of writing our own boot chain (for now):** getting from
  16-bit real mode to paging is 90 % of most hobby-OS projects' boot bugs.
  Limine is a proven, spec-clean loader; we keep a hand-written MBR
  (`src/boot/mbr.asm`) so the low-level story is still learned in the open.
* **C kernel first:** the most documented territory in OS development
  (OSDev wiki, Philipp Oppermann's blog). Rust enters the picture for
  drivers once we have processes to isolate them into.
* **Hybrid ISO:** "bootable on anything" was the founding requirement.

## License

MIT (this project). `vendor/limine.h` is BSD-0 licensed, © Mintsuki and
contributors. No code from Linux or Windows is used or intended to be
reverse-engineered.
