/* WindClone — the process table and a cooperative round-robin scheduler.
 *
 * Phase 4 and 5 proved a program could be *loaded* and *run*; what it never
 * had was two of them alive at once. That was not laziness, it was a hard
 * limit: one address space and one fixed load base (0x400000) meant a second
 * program would overwrite the first, so `usermode_run()` finished one before
 * starting the next. This file breaks that limit.
 *
 * Each process now owns its own page tables. paging_clone_address_space()
 * copies the kernel's view wholesale — text, data, the HHDM, the heap at
 * 256 GiB, the VGA window — so the kernel keeps running unchanged no matter
 * whose CR3 is loaded, and hands back a *private, empty* user window for the
 * loader to fill. Two processes can therefore hold the same virtual address
 * 0x400000 and never see each other's bytes. That is the isolation the earlier
 * phases deliberately did not have, and it is what makes a scheduler mean
 * something rather than being a run-to-completion loop with extra steps.
 *
 * The switch itself needs no new assembly. A syscall enters through
 * syscall_stub, which builds a full `struct syscall_frame` on the kernel's
 * IRQ stack and iretq's out of it when the handler returns. So to switch from
 * A to B we save A's frame into A's table slot, load B's CR3, copy B's saved
 * frame over the one on the stack, and simply return: the stub's iretq then
 * resumes B. The kernel IRQ stack is re-loaded from the TSS on every ring 3 ->
 * ring 0 entry, so nothing of a suspended process is left stranded on it —
 * which is exactly why a *cooperative* switch (taken inside a syscall) is
 * clean here and why timer-driven preemption is the separate next step.
 *
 * Scheduling policy is round-robin over the READY set, with a numeric priority
 * as the tie-break (lower number runs first) — the "priority round-robin" the
 * roadmap named, honest about being cooperative for now.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef WINDCLONE_PROC_H
#define WINDCLONE_PROC_H

#include <stdint.h>

#include "syscall.h"   /* struct syscall_frame — the saved context type */
#include "elf.h"       /* struct elf_page + ELF_MAX_PAGES */

#define PROC_MAX         8      /* resident processes at once */
#define PROC_NAME_MAX    24
#define PROC_PAGES_MAX   (ELF_MAX_PAGES + 8)   /* image + the four stack pages */
#define PROC_TABLES_MAX  24     /* P4 + PDPT + PD + per-2MiB page tables */

enum proc_state {
    PROC_FREE = 0,    /* slot unused                                       */
    PROC_READY,       /* runnable, waiting its turn                        */
    PROC_RUNNING,     /* owns the CPU now                                  */
    PROC_EXITED       /* ran to exit(); reaped when the table slot is freed */
};

/* One process. The context is a whole syscall_frame: for a process that has
 * already run it is what the stub will pop on resume; for one that has not run
 * yet it is a synthetic frame whose rip/rsp land _start on its entry point. */
struct proc {
    int      pid;
    int      state;
    int      priority;
    uint64_t cr3;                 /* physical base of the process P4        */
    uint64_t p4_virt;             /* HHDM pointer to it, to edit later      */
    struct syscall_frame ctx;
    struct elf_page pages[PROC_PAGES_MAX];   /* image + stack leaf frames */
    unsigned npages;
    uint64_t tables[PROC_TABLES_MAX];        /* private page-table frames */
    unsigned ntables;
    int      exit_code;
    char     name[PROC_NAME_MAX];
};

/* Load `path` into a fresh address space, set up its stack and command line,
 * and make it runnable. argc/argv follow the usermode_run() convention: NULL
 * or argc <= 0 means "run it as its own name". Returns the pid, or -1. */
int  proc_create(const char *path, int argc, const char *const *argv,
                 int priority);

/* Enter ring 3 on the first runnable process and drive the cooperative
 * round-robin until every process has exited. Runs on the kernel (boot) stack
 * and returns there when the last process calls exit(). A no-op when there is
 * nothing runnable. */
void proc_run_all(void);

/* Called from the syscall handler when a process gives up the CPU, or exits.
 * These rewrite `f` so the stub's iretq resumes the next process, or unwind
 * back to proc_run_all() when there is none. proc_exit does not return. */
void proc_yield(struct syscall_frame *f);
void proc_exit(struct syscall_frame *f, int code);

/* The pid of the running process, or 0 when the scheduler is not driving
 * anything (so the shell's own ring-3 runs report a plain, honest 0). */
int  proc_current_pid(void);

/* Non-zero while proc_run_all() owns the exit path — the flag that tells
 * SYS_EXIT to route through the scheduler instead of unwinding to the shell's
 * caller. */
int  proc_scheduler_active(void);

/* Spawn a process by path from inside the scheduler (SYS_SPAWN). Returns the
 * new pid, or -1. The caller keeps running; the child enters the READY set. */
int  proc_spawn_path(const char *path);

#endif
