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

/* --- helpers ---------------------------------------------------------- */

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

/* --- the scheduler ---------------------------------------------------- */

/* The single switch point. Save the running context, retire the caller
 * (READY on yield, EXITED on exit), and either resume another process by
 * rewriting `f` for the stub's iretq, or — when none is left — load the kernel
 * table, reap the exit, and unwind to proc_run_all(). Interrupts are off here
 * (int 0x80 is an interrupt gate), so nothing re-enters mid-switch. */
static void sched_switch(struct syscall_frame *f, int exiting, int code)
{
    int c = current;
    table[c].ctx = *f;
    if (exiting) {
        table[c].exit_code = code;
        table[c].state = PROC_EXITED;
    } else {
        table[c].state = PROC_READY;
        table[c].ctx.rax = 0;      /* sched_yield() returns 0 when resumed */
    }

    int n = pick_ready(rr);
    if (n >= 0) {
        struct proc *q = &table[n];
        if (exiting)
            proc_free_space(&table[c]);   /* safe: leaving c's CR3 now */
        current = n;
        q->state = PROC_RUNNING;
        rr = (n + 1) % PROC_MAX;
        paging_set_working((uint64_t *)(uintptr_t)q->p4_virt);
        paging_switch(q->cr3);
        *f = q->ctx;
        return;                            /* stub iretq resumes q */
    }

    /* Nobody runnable: the last process has exited. Return the kernel to its
     * own view before freeing anything, then unwind to proc_run_all(). */
    paging_use_kernel();
    if (exiting)
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
    sched_switch(f, 0, 0);
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
    return 1;
}

void proc_exit(struct syscall_frame *f, int code)
{
    if (current < 0) {
        /* Not under the scheduler: behave like the single-program path. */
        extern void usermode_exit(int code) __attribute__((noreturn));
        usermode_exit(code);
    }
    sched_switch(f, 1, code);
}

int proc_current_pid(void)
{
    return current >= 0 ? table[current].pid : 0;
}

int proc_scheduler_active(void)
{
    return sched_active;
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
    serial_write("sched: all processes exited\n");
}
