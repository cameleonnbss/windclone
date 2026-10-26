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
 * The table also holds one entry that is not a process in that sense: the
 * desktop shell's own slot, pid 1. It has no address space and no trap frame
 * because the shell's code runs on the kernel stack from the idle loop — what
 * it has is *parenthood*. Every program the prompt starts or the start menu
 * clicks is created as its child, which is what lets those programs use
 * spawn/wait/kill at all and lets the shell reap them instead of having the
 * end-of-run sweep forget them. See proc_shell_start() and proc_run_program().
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
    PROC_EXITED,      /* ran to exit(); the zombie, reaped by parent's wait */
    PROC_BLOCKED,     /* asleep in wait(): only a child's exit wakes it     */
    PROC_KERNEL       /* a slot owned by kernel code, not by a CPU context:
                       * the desktop shell's own entry. It has no address
                       * space and no trap frame, so the scheduler must never
                       * pick it, `kill` must never end it, and the sweep at
                       * the end of a run must never take it. Everything the
                       * shell starts is a child of this slot.             */
};

/* The switch a syscall asks for, passed to the scheduler's one switch point:
 * retire the caller as READY (yield), as EXITED (exit, with a code), or as
 * BLOCKED (wait found no zombie yet and there are live children). */
#define SWITCH_YIELD 0
#define SWITCH_EXIT  1
#define SWITCH_BLOCK 2

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
    int      parent;                /* pid that may reap this zombie; 0 = none  */
    int      armed;                 /* 1 = a wait wake already promises this pid */
    uint64_t wait_out;              /* blocked waiter's int *user pointer, or 0 */
    int      wake_code;             /* exit code staged for that pointer        */
    int      wake_pending;          /* 1 = deliver wake_code on next resume     */
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

/* wait(int *code_out) from ring 3. Reaps a zombie child (slot back to FREE,
 * child's exit code written to *code_out) and returns its pid; blocks the
 * caller when it still has live children that have not exited; answers
 * -WC_ECHILD when it has none. Blocking rewrites `f` like yield does, so the
 * syscall "returns" only when a child's exit wakes this process. Only valid
 * while the scheduler is driving the CPU. */
int  proc_wait(struct syscall_frame *f, uint64_t uout);

/* kill(pid, sig) from ring 3 — one process ending another. Only SIGKILL is
 * implemented (there is no delivery, no masking, no handler: this is
 * termination, not Unix in full, and the roadmap says so).
 *
 * The kill is *synchronous*, and the reason it can be is worth stating: the
 * killer is the running process, so any other target is by definition NOT on
 * the CPU — it is READY or BLOCKED, holding only a saved frame and memory no
 * CR3 points at. That means the kernel can retire it in place exactly as an
 * exiting process retires itself (free its address space, mark it EXITED with
 * code -SIGKILL, wake its parent) without touching a trap frame at all.
 * Deferring death to the victim's next syscall boundary would have needed a
 * "owed" flag and would leave a compute loop unkillable; killing a running
 * process asynchronously instead means surgery on the tick's frame. Neither
 * is necessary, so neither is done.
 *
 * Killing yourself is the one case that is not in-place: it goes through the
 * exit arm of the switch, so it does not return.
 *
 * Returns 0, -WC_EINVAL (a signal other than SIGKILL), -WC_ESRCH (no such
 * pid, or a pid already exited and awaiting its parent's wait — a zombie
 * cannot be killed twice) or -WC_EPERM (not under the scheduler, so there is
 * no table to kill from). */
int  proc_kill(int pid, int sig, struct syscall_frame *f);

/* The pid of the running process, or 0 when the scheduler is not driving
 * anything. 0 is the honest answer for the boot self-check's usermode_run(),
 * which has no table entry; a program the desktop shell starts now has one, so
 * it answers with a real pid. */
int  proc_current_pid(void);

/* Non-zero while proc_run_all() owns the exit path — the flag that tells
 * SYS_EXIT to route through the scheduler instead of unwinding to the shell's
 * caller. */
int  proc_scheduler_active(void);

/* Spawn a process by path from inside the scheduler (SYS_SPAWN). Returns the
 * new pid, or -1. The caller keeps running; the child enters the READY set. */
int  proc_spawn_path(const char *path);

/* --- the shell as a process -------------------------------------------
 * The desktop's command line is kernel code, so it never had an entry in the
 * table it drives: every program the prompt or the start menu started was an
 * excursion into ring 3 with no parent, which is why spawn/kill/wait could
 * only refuse and why a leftover slot had to be swept rather than reaped.
 * These calls give it one, and run the shell's programs underneath it. */

/* Claim the shell's slot — once, idempotent, and the first claim takes pid 1.
 * Returns the pid, or 0 when the table has no room (which would mean the boot
 * got that far with no process table at all). */
int  proc_shell_start(void);

/* The shell slot's pid, or 0 before proc_shell_start() has run. */
int  proc_shell_pid(void);

/* Start `path` as a child of the shell, run its whole process group to
 * completion (the scheduler owns the CPU until nothing is runnable), then reap
 * what the shell is owed and hand back the started program's exit code —
 * which may be negative, because a program the shell starts can now be killed.
 * 0 = it ran and came back (`*exit_code` is its code); -1 = the program could
 * not be built; -2 = it was started but never returned, so there is no code to
 * report and inventing one would be reporting a death this kernel did not
 * witness. This is what the shell's launch path calls instead of
 * usermode_run(). Only valid from kernel context — a process cannot start a
 * sibling's session. */
int  proc_run_program(const char *path, int argc, const char *const *argv,
                      int *exit_code);

/* Timer-driven preemption. Called from the IRQ0 path with the interrupted
 * frame after the tick is accounted and EOI'd. Returns non-zero only when it
 * actually switched: in that case it has loaded another process's CR3 and
 * overwritten `f`, so the stub's iretq resumes that process instead. When the
 * interrupted context is not ring 3, or no other process is runnable, it is a
 * no-op and returns 0 — the same process simply continues. */
int  proc_timer_preempt(struct syscall_frame *f);

#endif
