/* WindClone — process table + cooperative round-robin scheduler. See proc.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include <stddef.h>

#include "proc.h"
#include "paging.h"
#include "pmm.h"
#include "elf.h"
#include "vfs.h"
#include "serial.h"
#include "string.h"
#include "usermode.h"   /* USER_* window + the argv convention */
#include "wcabi.h"       /* WC_ECHILD / WC_EFAULT for proc_wait */

void panic(const char *why);

/* The privilege handover, provided by stubs.asm. usermode_enter() stashes the
 * kernel's rsp/rbp and iretq's into ring 3; user_exit_to_kernel() restores them
 * so a syscall can unwind back to whoever entered — the mechanism proc_run_all()
 * uses to get home once the last process exits. */
extern void usermode_enter(uint64_t entry, uint64_t user_rsp,
                           uint64_t argc, uint64_t argv);
extern void user_exit_to_kernel(void) __attribute__((noreturn));

/* Ring-3 selectors, matching the GDT layout in gdt.c and the values
 * usermode_enter() pushes: cs = 0x20|3, ss = 0x18|3, rflags.IF set. */
#define PROC_USER_CS     0x23ULL
#define PROC_USER_SS     0x1BULL
#define PROC_USER_FLAGS  0x202ULL

#define PROC_PRIO_DEFAULT 0

static struct proc table[PROC_MAX];
static int next_pid = 1;
static int current = -1;    /* index into table of the running process */
static int rr;              /* round-robin cursor: next index to try */
static int sched_active;    /* non-zero while proc_run_all drives the CPU */
static int shell_pid;       /* the shell's own slot, 0 until it is claimed */

/* The parent for a process created *outside* the scheduler. proc_create() is
 * called from kernel code when the shell starts a program, and "who is the
 * running process" has no answer there — so the launch path names the parent
 * explicitly instead of leaving it 0. It is set for the one create call and
 * cleared right after: the boot demos keep their parentless, kernel-started
 * meaning, and only what the shell launches gets a real owner. */
static int adopt_pid;

/* proc_run_program() asks for one specific child's exit code back; the sweep
 * that runs at the end of the group is the pass that holds it, so the answer
 * travels through these rather than through a second scan of the table. */
static int run_watch;
static int run_code;
static int run_done;   /* the watched pid was seen exiting — a code of -1 is a
                        * perfectly legal exit status, so "did it come back"
                        * cannot be folded into "what did it come back with" */

/* --- helpers ---------------------------------------------------------- */

/* An exit code of -9 is the whole point of a reap line, so the code prints
 * through serial's signed writer rather than through a local copy of it: the
 * shell reports the same number to the screen, and two printers of one claim
 * are two chances to disagree about how a process died. */

static void name_copy(char *dst, const char *src)
{
    unsigned i = 0;
    for (; i + 1 < PROC_NAME_MAX && src[i]; i++)
        dst[i] = src[i];
    dst[i] = '\0';
}

/* Write `len` bytes of a kernel buffer into the process's page tables at `va`,
 * walking page by page through the higher-half map. Used to lay the command
 * line on a stack that is not reachable through the live CR3 (the process's own
 * table is current for mapping, but we still run on the boot CR3). */
static int proc_poke(uint64_t va, const void *src, uint64_t len)
{
    const uint8_t *s = src;
    while (len) {
        uint64_t page = va & ~0xFFFULL;
        uint64_t off  = va & 0xFFFULL;
        uint64_t n    = 4096 - off;
        if (n > len)
            n = len;
        uint64_t phys = paging_translate(page);
        if (phys == 0)
            return -1;                    /* a stack page we did not map */
        memcpy((uint8_t *)pmm_phys_to_virt(phys) + off, s, (size_t)n);
        s += n;
        va += n;
        len -= n;
    }
    return 0;
}

/* Map the user stack into the process's table (recording every frame so exit
 * can reclaim it) and lay argc/argv on it exactly the way usermode.c does, so
 * the ELF contract in wcabi.h is identical for a scheduler-started program. */
static int proc_setup_stack(struct proc *p, int argc,
                            const char *const *argv, uint64_t *rsp_out)
{
    uint64_t pages = USER_STACK_SIZE / 4096;
    for (uint64_t i = 0; i < pages; i++) {
        uint64_t va = USER_STACK_TOP - (i + 1) * 4096;
        uint64_t phys = pmm_alloc_frame();
        if (phys == 0) {
            serial_write("proc: out of frames while building the stack\n");
            return -1;
        }
        paging_map_page(va, phys, PTE_WRITE | PTE_USER);
        if (p->npages >= PROC_PAGES_MAX)
            return -1;
        p->pages[p->npages].va = va;
        p->pages[p->npages].phys = phys;
        p->pages[p->npages].writable = 1;
        p->npages++;
    }

    /* Same psABI shape as usermode.c build_stack(): strings top-down, then the
     * argc / argv[] / envp NULL / auxv word table, 16-byte aligned for entry. */
    uint64_t ptrs[USER_ARG_MAX];
    uint64_t words = 1 + (uint64_t)argc + 1 + 1 + 2;
    uint64_t floor = USER_STACK_BOTTOM + 16;
    uint64_t sp = USER_STACK_TOP - 16;

    for (int i = argc - 1; i >= 0; i--) {
        uint64_t l = strlen(argv[i]) + 1;
        if (l > USER_ARGV_BYTES || sp - l < floor + words * 8)
            return -1;
        sp -= l;
        if (proc_poke(sp, argv[i], l) != 0)
            return -1;
        ptrs[i] = sp;
    }

    sp &= ~7ULL;
    if (sp < floor + words * 8)
        return -1;
    sp -= words * 8;
    sp &= ~0xFULL;
    if (sp < floor)
        return -1;

    uint64_t table_words[USER_ARG_MAX + 5];
    uint64_t w = 0;
    table_words[w++] = (uint64_t)argc;
    for (int i = 0; i < argc; i++)
        table_words[w++] = ptrs[i];
    table_words[w++] = 0;   /* argv[argc] */
    table_words[w++] = 0;   /* envp[0]    */
    table_words[w++] = 0;   /* auxv AT_NULL */
    table_words[w++] = 0;
    if (proc_poke(sp, table_words, w * 8) != 0)
        return -1;

    *rsp_out = sp;
    return 0;
}

/* Reclaim a dead process's memory. Leaf frames first (image + stack), then the
 * private page-tree nodes. Never call this while the freed table is loaded in
 * CR3 — every caller switches away first. */
static void proc_free_space(struct proc *p)
{
    for (unsigned i = 0; i < p->npages; i++)
        if (p->pages[i].phys != 0)
            pmm_free_frame(p->pages[i].phys);
    p->npages = 0;
    paging_free_table_frames(p->tables, p->ntables);
    p->ntables = 0;
    p->cr3 = 0;
    p->p4_virt = 0;
}

/* Is [addr, addr+len) a buffer the *running* process can write — mapped in
 * the table we are switched into, user-accessible, writable? The short sibling
 * of syscall.c's user_ptr_ok(): proc.c needs it for the four bytes wait() asks
 * the kernel to fill in, and the walk must consult p4 (the working table), not
 * trust that a user address is reachable just because the caller is ring 3. */
static int user_write_ok(uint64_t addr, uint64_t len)
{
    if (!user_range_ok(addr, len))
        return 0;
    for (uint64_t page = addr & ~0xFFFULL; page < addr + len; page += 0x1000) {
        uint64_t flags = paging_flags(page);
        if (!(flags & PTE_PRESENT) || !(flags & PTE_USER) || !(flags & PTE_WRITE))
            return 0;
    }
    return 1;
}

/* The next process to run: lowest priority number wins; among equals, the one
 * at or after the round-robin cursor, wrapping. -1 when nothing is READY. */
static int pick_ready(int from)
{
    int best = -1, best_prio = 1 << 30;
    for (int k = 0; k < PROC_MAX; k++) {
        int i = (from + k) % PROC_MAX;
        if (table[i].state == PROC_READY && table[i].priority < best_prio) {
            best_prio = table[i].priority;
            best = i;
        }
    }
    return best;
}

/* --- creation --------------------------------------------------------- */

int proc_create(const char *path, int argc, const char *const *argv,
                int priority)
{
    const char *self[1];
    if (argv == NULL || argc <= 0) {
        self[0] = path;
        argv = self;
        argc = 1;
    }
    if (argc > USER_ARG_MAX) {
        serial_write("proc: too many arguments\n");
        return -1;
    }

    struct proc *p = NULL;
    for (int i = 0; i < PROC_MAX; i++)
        if (table[i].state == PROC_FREE) { p = &table[i]; break; }
    if (p == NULL) {
        serial_write("proc: no free process slot\n");
        return -1;
    }

    int64_t fd = vfs_open(path);
    if (fd < 0) {
        serial_write("proc: cannot open ");
        serial_write(path);
        serial_write("\n");
        return -1;
    }

    p->npages = 0;
    p->ntables = 0;
    p->state = PROC_FREE;

    uint64_t *old_working = paging_get_working();
    paging_set_table_sink(p->tables, &p->ntables, PROC_TABLES_MAX);

    uint64_t cr3 = paging_clone_address_space();
    p->cr3 = cr3;
    p->p4_virt = (uint64_t)(uintptr_t)pmm_phys_to_virt(cr3);
    paging_set_working((uint64_t *)(uintptr_t)p->p4_virt);

    struct elf_report rep;
    uint64_t rsp = 0;
    int rc = -1;
    if (elf_load_pages((int)fd, &rep, p->pages, PROC_PAGES_MAX,
                       &p->npages) == 0 &&
        proc_setup_stack(p, argc, argv, &rsp) == 0) {
        rc = 0;
    }

    vfs_close((int)fd);
    paging_set_table_sink(NULL, NULL, 0);
    paging_set_working(old_working);

    if (rc != 0) {
        /* A partial build owns nothing yet worth keeping: give back every frame
         * the failed load allocated and free the slot. */
        proc_free_space(p);
        serial_write("proc: failed to build ");
        serial_write(path);
        serial_write("\n");
        return -1;
    }

    /* The synthetic birth context: an iretq frame whose rip/rsp land _start on
     * its entry, with argc/argv in rdi/rsi as wcabi.h promises. Resuming a
     * process that has already yielded overwrites this with its real frame. */
    memset(&p->ctx, 0, sizeof p->ctx);
    p->ctx.rip = rep.entry;
    p->ctx.user_rsp = rsp;
    p->ctx.rflags = PROC_USER_FLAGS;
    p->ctx.cs = PROC_USER_CS;
    p->ctx.user_ss = PROC_USER_SS;
    p->ctx.vector = 0x80;
    p->ctx.rdi = (uint64_t)argc;
    p->ctx.rsi = rsp + 8;

    /* The reaper link: a process spawned while the scheduler runs belongs to
     * the process that spawned it, so its zombie waits for *that* parent's
     * wait() and nobody else's. A program the *shell* starts has no running
     * parent — the shell is kernel code — so the launch path names one through
     * adopt_pid, and its children become the shell's. Boot-created demo
     * processes have neither (current < 0, no adopt) and stay parentless. */
    if (sched_active && current >= 0)
        p->parent = table[current].pid;
    else
        p->parent = adopt_pid;
    p->wait_out = 0;
    p->wake_code = 0;
    p->wake_pending = 0;

    p->priority = priority;
    p->exit_code = 0;
    p->pid = next_pid++;
    p->state = PROC_READY;
    name_copy(p->name, path);

    serial_write("proc: spawned ");
    serial_write(path);
    serial_write(" as pid ");
    serial_write_dec((uint64_t)p->pid);
    serial_write(", pages ");
    serial_write_dec(p->npages);
    serial_write(", entry ");
    serial_write_hex(rep.entry);
    serial_write("\n");
    return p->pid;
}

int proc_spawn_path(const char *path)
{
    const char *argv[1] = { path };
    return proc_create(path, 1, argv, PROC_PRIO_DEFAULT);
}

/* --- the shell's own slot ---------------------------------------------
 *
 * The shell has always driven the table without being in it, and that is the
 * reason three separate lies had to be told: spawn and kill answered
 * -WC_EPERM from a prompt-started program (nothing would ever schedule the
 * child, so promising one was worse than refusing), wait answered
 * -WC_ECHILD, and every leftover slot had to be *swept* at the end of a run
 * because no parent existed to reap it. Claiming a slot removes all three: the
 * shell is now a process in the only sense this kernel means — an entry the
 * table knows about — and every program it starts is a child of it.
 *
 * It is emphatically not a schedulable context. The shell's code runs on the
 * kernel stack from the idle loop, so its slot has no address space, no page
 * tables and no trap frame; giving it PROC_READY would make the scheduler try
 * to run all of that and fault. PROC_KERNEL is a state the scheduler, the
 * killer and the sweep each have to refuse to touch, and each of those three
 * refusals is a line of code with a reason attached.
 */
int proc_shell_start(void)
{
    if (shell_pid != 0)
        return shell_pid;

    for (int i = 0; i < PROC_MAX; i++) {
        if (table[i].state != PROC_FREE)
            continue;
        struct proc *p = &table[i];
        memset(p, 0, sizeof *p);
        p->pid = next_pid++;          /* 1, because the shell is claimed first */
        p->state = PROC_KERNEL;
        name_copy(p->name, "shell");
        shell_pid = p->pid;
        serial_write("proc: shell is pid ");
        serial_write_dec((uint64_t)shell_pid);
        serial_write(", owner of every program it starts\n");
        return shell_pid;
    }

    serial_write("proc: no slot left for the shell\n");
    return 0;
}

int proc_shell_pid(void)
{
    return shell_pid;
}

/* --- the scheduler ---------------------------------------------------- */

/* A woken parent carries its exit code through the table, not through a
 * register: the child that woke it may not run again for a while, and the
 * parent's `int *status` is only writable under the *parent's* page tables.
 * So the write is deferred to the moment of resume, when q's CR3 is live and
 * the walker sees the stack slot wait() pointed at. A stale or unroutable
 * pointer is dropped honestly — a switch is not an excuse for a fault. */
static void wakeup_deliver(struct proc *q)
{
    if (!q->wake_pending)
        return;
    q->wake_pending = 0;
    if (q->wait_out != 0 && user_write_ok(q->wait_out, sizeof(int)))
        memcpy((void *)(uintptr_t)q->wait_out, &q->wake_code, sizeof(int));
}

/* A child died — by its own exit() or by somebody's kill() — and its parent may
 * be sitting in wait(). When it is, the pid and the code are planted as the
 * return value of the call that blocked, exactly the way the yield arm returns
 * 0, and the zombie's slot is retired here: the wake *is* the reap. Called
 * BEFORE pick_ready so a woken parent is part of the set the scheduler chooses
 * from.
 *
 * One promise per parent is the invariant, and `wake_pending` is what holds it.
 * A second child that dies while the first wake is still undelivered (the parent
 * READY, a tick not yet resumed it) must not overwrite the planted pid, so this
 * refuses it — and that second death leaves its zombie EXITED, an ordinary reap
 * candidate for the parent's next wait().
 *
 * The table used to keep a second copy of that promise, an `armed` flag on the
 * zombie a wake spoke for, and proc_wait's scan had to skip armed slots so a
 * parent could not collect one pid twice. The cost was invisible and real: a
 * woken parent had already been *answered* but had not reaped anything, so its
 * child's slot stayed EXITED with no process able to name it again, and burned a
 * table slot until the sweep. With the retirement below there is only ever one
 * place a promise lives — the parent that holds it — so the flag is gone rather
 * than kept as a decoration. */
static void wake_parent(int c, int code)
{
    struct proc *cc = &table[c];
    if (cc->parent == 0)
        return;                 /* a boot demo has no parent to notify */
    for (int i = 0; i < PROC_MAX; i++) {
        struct proc *pp = &table[i];
        if (pp->pid != cc->parent)
            continue;
        if (pp->state == PROC_BLOCKED && !pp->wake_pending) {
            int pid = cc->pid;
            pp->state = PROC_READY;
            pp->ctx.rax = (uint64_t)pid;
            pp->wake_code = code;
            pp->wake_pending = 1;

            /* The promise has been made, so the zombie has been answered.
             * Everything a slot kept alive for — its pid and its exit code — is
             * on its way to the one process allowed to collect them. */
            cc->state = PROC_FREE;
            cc->parent = 0;
            cc->exit_code = 0;
            cc->wait_out = 0;
            cc->wake_pending = 0;
            cc->wake_code = 0;
            cc->name[0] = '\0';
        }
        break;
    }
}

/* --- adoption ----------------------------------------------------------
 *
 * A process that dies while its children are still alive leaves something
 * behind, and until now that something had no owner: the child kept the pid of
 * a dead parent in its `parent` field, which meant no wait() could ever name it,
 * no kill() could be blamed on it, and its exit code — the one number a dead
 * process still carries — was written into a slot nobody would read. The end-of
 * run sweep freed the memory and the table forgot the relationship ever existed.
 * That is the hole Unix closes by having a process that never dies adopt what is
 * left, and this kernel now has one: the shell's slot, pid 1.
 *
 * One role, one function, so the answer can change without touching the rule.
 * The shell is the adopter because it is the only process in this system that
 * outlives every program it starts — it is kernel code, it cannot be killed
 * (proc_kill answers -WC_EPERM for its slot), and it is claimed before a single
 * key can be typed. A future /bin/init that really is pid 1 in ring 3 takes over
 * by being what this returns; nothing else about adoption would change.
 */
static int proc_adopter_pid(void)
{
    return shell_pid;
}

/* Place every child of the process at `c` with the adopter. Called at the moment
 * of death — from the exit arm of the switch and from the in-place arm of kill(),
 * which are the only two ways a process ends — and never anywhere else, so a
 * parent cannot lose a child by any route the table does not already witness.
 *
 * Three details are load-bearing:
 *
 *  - a zombie is moved too, not only a live child. A parent can die between the
 *    moment a child exits and the moment it would have been waited for, and the
 *    pair (pid, exit code) is the whole reason the slot still exists — dropping
 *    the relationship there would lose the one number the child left behind.
 *  - the adopter's own slot is skipped, and so is PROC_KERNEL: nothing here can
 *    make the shell anybody's child, and a live process is never adopted by the
 *    parent it already has.
 *  - when there is no adopter at all (before shell_init() has claimed its slot),
 *    the relationship is reported rather than silently dropped. The boot demos
 *    run after that claim, so this branch is not expected to print — but "we
 *    invented no parent" is the honest alternative to a line that would lie.
 */
static void proc_adopt_children(int c)
{
    int dead = table[c].pid;
    int adopter = proc_adopter_pid();

    if (dead == 0)
        return;

    for (int i = 0; i < PROC_MAX; i++) {
        struct proc *p = &table[i];
        if (p->state == PROC_FREE || p->state == PROC_KERNEL)
            continue;
        if (p->parent != dead)
            continue;

        if (adopter == 0) {
            serial_write("proc: pid ");
            serial_write_dec((uint64_t)p->pid);
            serial_write(" outlived its parent ");
            serial_write_dec((uint64_t)dead);
            serial_write(" and no adopter exists\n");
            continue;
        }

        p->parent = adopter;

        serial_write("proc: pid ");
        serial_write_dec((uint64_t)p->pid);
        serial_write(" adopted by pid ");
        serial_write_dec((uint64_t)adopter);
        serial_write(" (parent ");
        serial_write_dec((uint64_t)dead);
        serial_write(" is gone)\n");
    }
}

/* The single switch point. Save the running context, retire the caller
 * (READY on yield, EXITED on exit, BLOCKED on wait), and either resume another
 * process by rewriting `f` for the stub's iretq, or — when none is left —
 * load the kernel table, reap the exit, and unwind to proc_run_all().
 * Interrupts are off here (int 0x80 is an interrupt gate), so nothing re-enters
 * mid-switch. */
static void sched_switch(struct syscall_frame *f, int mode, int code)
{
    int c = current;
    table[c].ctx = *f;
    if (mode == SWITCH_EXIT) {
        table[c].exit_code = code;
        table[c].state = PROC_EXITED;
    } else if (mode == SWITCH_BLOCK) {
        table[c].state = PROC_BLOCKED;
    } else {
        table[c].state = PROC_READY;
        table[c].ctx.rax = 0;      /* sched_yield() returns 0 when resumed */
    }

    /* The trilogy's last edge, and the kill's: a child that dies while its
     * parent sits in wait() wakes it. See wake_parent() for why exactly one
     * promise per zombie is the thing that makes this safe to attempt always.
     *
     * Adoption goes first, because the wake below may retire this very slot: the
     * children have to be placed while the parent they belong to is still
     * describable. A process that dies does not take its children with it —
     * they keep running, and somebody now owns the exit code they will each
     * leave behind. */
    if (mode == SWITCH_EXIT) {
        proc_adopt_children(c);
        wake_parent(c, code);
    }

    int n = pick_ready(rr);
    if (n >= 0) {
        struct proc *q = &table[n];
        if (mode == SWITCH_EXIT)
            proc_free_space(&table[c]);   /* safe: leaving c's CR3 now */
        current = n;
        q->state = PROC_RUNNING;
        rr = (n + 1) % PROC_MAX;
        paging_set_working((uint64_t *)(uintptr_t)q->p4_virt);
        paging_switch(q->cr3);
        *f = q->ctx;
        wakeup_deliver(q);         /* parent's CR3 is live: deliver now */
        return;                    /* stub iretq resumes q */
    }

    /* Nobody runnable. Either the last process exited (caller EXITED, every
     * other slot already reaped long ago) — the honest end, unwind home — or
     * the caller just blocked and nothing else can ever run: a cycle wait can
     * only sleep through. Report that truthfully instead of pretending, then
     * unwind; the blocked slot dies with the scheduler, which is exactly what
     * "this demo would hang forever" means in words. */
    int blocked = 0;
    for (int i = 0; i < PROC_MAX; i++)
        if (table[i].state == PROC_BLOCKED)
            blocked = 1;
    if (blocked)
        serial_write("sched: deadlock — a process is waiting but nothing is runnable\n");

    paging_use_kernel();
    if (mode == SWITCH_EXIT)
        proc_free_space(&table[c]);
    current = -1;
    user_exit_to_kernel();
}

void proc_yield(struct syscall_frame *f)
{
    if (current < 0) {
        /* Not under the scheduler: nothing to switch to, so just report
         * success rather than leaving the call number in rax. */
        f->rax = 0;
        return;
    }
    sched_switch(f, SWITCH_YIELD, 0);
}

/* Timer-driven preemption — the step beyond the cooperative switch.
 *
 * A tick that lands on a ring-3 process interrupts it mid-instruction, and the
 * IRQ stub has just saved a frame that is layout-identical to the one a syscall
 * saves (same GPR order, and a privilege change pushed user_rsp/user_ss just
 * like int 0x80 does). So the same move works: retire the *running* process by
 * saving this frame and marking it READY, then resume another runnable one by
 * loading its CR3 and copying its frame over the one on the stack; the stub's
 * pop + iretq then returns into *that* process. Nothing here frees memory or
 * touches the exit path — a preempted process stays fully alive.
 *
 * If the interrupted context is the kernel (RPL != 3), the scheduler is not
 * driving the CPU, or this is the only runnable process, we change nothing and
 * return 0 so the caller just goes back to the same process. We run with IF
 * clear (IRQ0 is an 0x8E interrupt gate), so no second tick re-enters here. */
int proc_timer_preempt(struct syscall_frame *f)
{
    if (!sched_active || current < 0 || (f->cs & 3) != 3)
        return 0;

    int c = current;
    table[c].ctx = *f;
    table[c].state = PROC_READY;

    int n = pick_ready(rr);
    if (n < 0 || n == c) {
        /* Nobody else to run: give c its RUNNING state back and return. */
        table[c].state = PROC_RUNNING;
        return 0;
    }

    struct proc *q = &table[n];
    current = n;
    q->state = PROC_RUNNING;
    rr = (n + 1) % PROC_MAX;
    paging_set_working((uint64_t *)(uintptr_t)q->p4_virt);
    paging_switch(q->cr3);
    *f = q->ctx;
    wakeup_deliver(q);
    return 1;
}

void proc_exit(struct syscall_frame *f, int code)
{
    if (current < 0) {
        /* Not under the scheduler: behave like the single-program path. */
        extern void usermode_exit(int code) __attribute__((noreturn));
        usermode_exit(code);
    }
    sched_switch(f, SWITCH_EXIT, code);
}

/* wait(int *code_out) — the blocking half, and the only way a zombie dies.
 *
 * Three honest answers, in this order:
 *
 * 1. A child already EXITED (a zombie: memory returned at exit, table slot
 *    kept precisely for this moment): hand its pid and exit code back, reap
 *    the slot to FREE, and return without switching. Same-turn reaps matter —
 *    the parent's first wait after the wake-up still has a second child's
 *    zombie to take, and a real Unix behaves exactly the same way. The zombie
 *    whose death *woke* this call is not on this list: it was retired by
 *    wake_parent() when it planted the pid and the code, so the slot is already
 *    FREE by the time the parent resumes.
 * 2. No zombie but a live child (READY/RUNNING/BLOCKED): save this frame the
 *    way yield does, go BLOCKED, and switch. The call "returns" when a child
 *    exits — sched_switch plants the pid in ctx.rax and stages the code for
 *    wakeup_deliver() to write once this process's CR3 is live again.
 * 3. No children at all: -WC_ECHILD, because wait() is not a sleep timer.
 *
 * The uout pointer is validated here against *this* process's page tables (we
 * run under its CR3) and again at delivery, under the same one later. */
int proc_wait(struct syscall_frame *f, uint64_t uout)
{
    if (!sched_active || current < 0)
        return WC_ECHILD;          /* wait() outside the scheduler cannot block */

    struct proc *p = &table[current];

    for (int i = 0; i < PROC_MAX; i++) {
        struct proc *z = &table[i];
        if (z->state == PROC_EXITED && z->parent == p->pid) {
            int pid = z->pid, code = z->exit_code;
            if (uout != 0) {
                if (!user_write_ok(uout, sizeof code))
                    return WC_EFAULT;
                memcpy((void *)(uintptr_t)uout, &code, sizeof code);
            }
            z->state = PROC_FREE;
            z->parent = 0;
            z->wait_out = 0;
            z->wake_pending = 0;
            return pid;
        }
    }

    int live = 0;
    for (int i = 0; i < PROC_MAX; i++)
        if (table[i].parent == p->pid &&
            (table[i].state == PROC_READY ||
             table[i].state == PROC_RUNNING ||
             table[i].state == PROC_BLOCKED))
            live = 1;
    if (!live)
        return WC_ECHILD;

    if (uout != 0 && !user_write_ok(uout, sizeof(int)))
        return WC_EFAULT;
    p->wait_out = uout;
    sched_switch(f, SWITCH_BLOCK, 0);
    /* the switch never returns to this frame */
    return WC_ECHILD;
}

/* kill(pid, SIGKILL) — one process ending another, and the reason the process
 * table has to be reachable *from* ring 3 rather than only driven by the kernel.
 *
 * The kill is synchronous, and the reason it can be is worth stating: the killer
 * is the running process, so any other target is by definition NOT on the CPU —
 * it is READY or BLOCKED, holding a saved frame and an address space no CR3
 * points at. So the kernel can retire it where it stands, exactly as an exiting
 * process retires itself (give the memory back, mark the slot EXITED with the
 * code -SIGKILL, wake its parent) without touching a trap frame at all. Two
 * alternatives were rejected on purpose: deferring death to the victim's next
 * syscall needs an "owed" flag and leaves a compute loop unkillable, and killing
 * a *running* process asynchronously means surgery on the tick's frame. Neither
 * is necessary here, so neither is done.
 *
 * Killing yourself is the one case that is not in-place: it goes through the
 * exit arm of the switch, so it does not return.
 *
 * A victim's own children are not killed with it, and they are not left ownerless
 * either: proc_adopt_children() places them with the adopter before the victim's
 * parent is told, which is what makes an exit code that nobody was going to read
 * into one the shell reaps and prints. A kill that ends a parent is therefore not
 * an excuse for losing a grandchild — see the adoption block above. */
int proc_kill(int pid, int sig, struct syscall_frame *f)
{
    if (!sched_active || current < 0)
        return WC_EPERM;        /* no table to kill from outside the scheduler */
    if (sig != SIGKILL)
        return WC_EINVAL;       /* there is no delivery, so no other signal */

    int t = -1;
    for (int i = 0; i < PROC_MAX; i++) {
        if (table[i].state != PROC_FREE && table[i].pid == pid) {
            t = i;
            break;
        }
    }
    if (t < 0 || table[t].state == PROC_EXITED)
        return WC_ESRCH;        /* never existed, or already a zombie — which
                                 * is not dead twice, it is waiting to be reaped */

    /* The shell's slot is the one entry kill cannot take. It has no address
     * space, no trap frame and nobody holding a CPU to end, so there is no
     * "retire it where it stands" to do — and were it freed, every program
     * this screen started would belong to a parent that no longer exists.
     * EPERM rather than ESRCH, because the process really is in the table:
     * refusing is honest, pretending it was never born is not. */
    if (table[t].state == PROC_KERNEL) {
        serial_write("proc: refused to kill pid ");
        serial_write_dec((uint64_t)pid);
        serial_write(" (the shell owns it)\n");
        return WC_EPERM;
    }

    /* Reported before either arm, because both are a killing and the log line
     * is the kernel's own witness: for a self-kill the two pids are equal, and
     * once the switch below runs there is no "by pid" left to print. */
    serial_write("proc: killed pid ");
    serial_write_dec((uint64_t)pid);
    serial_write(" (SIGKILL) by pid ");
    serial_write_dec((uint64_t)table[current].pid);
    serial_write("\n");

    if (t == current) {
        /* Self-kill: an ordinary exit whose code says it was killed. */
        sched_switch(f, SWITCH_EXIT, -sig);
        return WC_EPERM;        /* not reached */
    }

    struct proc *v = &table[t];
    v->state = PROC_EXITED;
    v->exit_code = -sig;
    v->wait_out = 0;
    v->wake_pending = 0;        /* a sleeper's pending wake dies with it */
    proc_free_space(v);         /* safe: the victim's CR3 is not loaded */
    proc_adopt_children(t);     /* same order as the exit arm: place, then tell */
    wake_parent(t, -sig);
    return 0;
}

int proc_current_pid(void)
{
    return current >= 0 ? table[current].pid : 0;
}

/* The parent as the table sees it *now*, which is the interesting part: an only
 * child of a program that died answers with the adopter's pid from that moment
 * on, not with the pid it was born under. A program can therefore report, from
 * ring 3, the very re-parenting the kernel performed on its behalf — and a test
 * can require the two numbers to agree across the boundary instead of trusting
 * either side alone. 0 when there is no table entry (the boot self-check's
 * single-program path) or when nothing owns this process (a kernel-started demo). */
int proc_current_ppid(void)
{
    return current >= 0 ? table[current].parent : 0;
}

int proc_scheduler_active(void)
{
    return sched_active;
}

/* Every leftover in the table at the end of a run is a dead end: an EXITED slot
 * nobody was ever the parent of (a boot demo, which the kernel started with no
 * adopter to name), or a BLOCKED sleeper the deadlock report already named.
 * Nobody can reap them — wait() only works from a running process — so without
 * this sweep those slots are burned for the rest of the session, and the boot
 * demos alone (tick, spin, waiter, reaper) would run PROC_MAX dry halfway
 * through. Memory needs no ceremony here: an EXITED slot gave its frames back at
 * exit, a BLOCKED one still holds them and proc_free_space() hands those over.
 * next_pid is deliberately NOT reset: pids are never reused, so a program holding
 * an old pid can never point at a new process by accident.
 *
 * Three slots are handled differently, and each reason is load-bearing. The
 * shell's own entry is not a leftover of this run — it is what starts the next
 * one — so it is skipped entirely. A zombie that an exiting parent handed over by
 * adoption is no longer one of the dead ends above: it arrived here as the
 * shell's child, so the line says what happened ("shell: reaped pid P code C")
 * and the code travels back to proc_run_program() through run_code. And a slot
 * already retired by the wake that answered it (wake_parent) is simply FREE, so
 * this pass never sees it. Before the shell had a slot this sweep was the only
 * reaper the desktop ever had, and before adoption an orphan reached this point
 * ownerless and was freed in silence — which is the claim that stopped being
 * true, and the reason it is written down here rather than left to the log. */
static void proc_table_sweep(void)
{
    for (int i = 0; i < PROC_MAX; i++) {
        struct proc *p = &table[i];
        if (p->state == PROC_FREE || p->state == PROC_KERNEL)
            continue;

        if (shell_pid != 0 && p->state == PROC_EXITED && p->parent == shell_pid) {
            serial_write("shell: reaped pid ");
            serial_write_dec((uint64_t)p->pid);
            serial_write(" code ");
            serial_write_signed(p->exit_code);
            serial_write("\n");
            if (run_watch != 0 && p->pid == run_watch) {
                run_code = p->exit_code;
                run_done = 1;
            }
        }

        proc_free_space(p);
        p->state = PROC_FREE;
        p->parent = 0;
        p->wait_out = 0;
        p->wake_pending = 0;
        p->name[0] = '\0';
    }
}

void proc_run_all(void)
{
    int first = pick_ready(0);
    if (first < 0) {
        serial_write("proc: nothing to schedule\n");
        return;
    }

    serial_write("sched: round-robin over the process table\n");
    sched_active = 1;
    current = first;
    table[first].state = PROC_RUNNING;
    rr = (first + 1) % PROC_MAX;

    struct proc *p = &table[first];
    paging_set_working((uint64_t *)(uintptr_t)p->p4_virt);
    paging_switch(p->cr3);
    usermode_enter(p->ctx.rip, p->ctx.user_rsp, p->ctx.rdi, p->ctx.rsi);

    /* Reached only when the last process exited and unwound here. */
    sched_active = 0;
    current = -1;
    paging_use_kernel();
    proc_table_sweep();
    serial_write("sched: all processes exited\n");
}

/* The shell's way of running something — and the difference it makes is not
 * cosmetic. The path this replaces, usermode_run(), loads one image into the
 * kernel's own address space, enters ring 3, and comes back when the program
 * calls exit(); the program has no pid, so the process verbs have nothing to
 * work with and must refuse. Here the program goes through proc_create() as a
 * child of the shell's slot, so it has an address space, a pid, a parent that
 * reaps it, and a scheduler that will actually run anything it spawns.
 *
 * The run ends when the scheduler finds nothing READY — the started program and
 * everything it spawned have finished (or one of them blocked with nobody left
 * to wake it, which sched_switch reports honestly as a deadlock). Then the
 * sweep reaps the shell's children, and the code of the one this call started
 * comes back through run_code.
 *
 * Returns 0 with an exit code, -1 when the program could not be built at all,
 * and -2 when it was started but never came back: three different answers to
 * three different things that can go wrong, and a shell that printed "exit 0"
 * for the third would be reporting a death it did not witness. */
int proc_run_program(const char *path, int argc, const char *const *argv,
                     int *exit_code)
{
    if (proc_shell_start() == 0)
        return -1;
    if (sched_active || current >= 0) {
        /* Not from a process. proc_run_all() unwinds to its own frame through
         * the exit path, so a nested call would strand a scheduler inside a
         * scheduler; the syscall handler already refuses spawn outside the
         * table for the same reason. */
        serial_write("proc: a process cannot start a shell session\n");
        return -1;
    }

    run_code = -1;
    run_done = 0;
    run_watch = 0;

    adopt_pid = shell_pid;
    int pid = proc_create(path, argc, argv, PROC_PRIO_DEFAULT);
    adopt_pid = 0;
    if (pid < 0)
        return -1;

    run_watch = pid;
    proc_run_all();
    run_watch = 0;

    if (!run_done) {
        /* Started, never seen exiting: it was still BLOCKED (or READY behind a
         * deadlock report) when the scheduler came home. Saying "exit code -1"
         * would invent an ending this kernel did not witness. */
        serial_write("proc: pid ");
        serial_write_dec((uint64_t)pid);
        serial_write(" never returned to its parent\n");
        return -2;
    }

    if (exit_code != NULL)
        *exit_code = run_code;
    return 0;
}
