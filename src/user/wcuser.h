/* WindClone — the program's side of the syscall boundary.
 *
 * A freestanding program has no libc, so these inline wrappers are the whole
 * standard library for now. They are correct only because of one fact
 * about this kernel's entry path, worth stating before anyone "optimises" it:
 *
 *   int 0x80 lands in syscall_stub (src/kernel/stubs.asm), which pushes all
 *   sixteen GPRs, calls C with the frame, and pops them again — so every
 *   register the compiler had live comes back untouched, and so does rflags.
 *   That is why the asm below declares no register clobbers at all. It is NOT
 *   because syscalls are cheap; a kernel that entered with SYSCALL instead
 *   (which overwrites rcx and r11) would need "rcx", "r11" here *and* a
 *   kernel-side promise to preserve rdi/rsi/rdx/r8/r9/r10. See wcabi.h.
 *
 * "memory" is the one clobber that is genuinely required: the kernel writes
 * into the buffer passed to read(), and gcc must not keep a cached copy of
 * those bytes in a register across the call.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef WINDCLONE_WCUSER_H
#define WINDCLONE_WCUSER_H

#include "wcabi.h"

static inline long wc_call1(long nr, long a0)
{
    long ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(nr), "D"(a0)
                     : "memory");
    return ret;
}

static inline long wc_call2(long nr, long a0, long a1)
{
    long ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(nr), "D"(a0), "S"(a1)
                     : "memory");
    return ret;
}

static inline long wc_call3(long nr, long a0, long a1, long a2)
{
    long ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(nr), "D"(a0), "S"(a1), "d"(a2)
                     : "memory");
    return ret;
}

static inline long wc_write(long fd, const void *buf, unsigned long len)
{
    return wc_call3(SYS_WRITE, fd, (long)buf, (long)len);
}

static inline long wc_read(long fd, void *buf, unsigned long len)
{
    return wc_call3(SYS_READ, fd, (long)buf, (long)len);
}

static inline long wc_open(const char *path)
{
    return wc_call1(SYS_OPEN, (long)path);
}

static inline long wc_close(long fd)
{
    return wc_call1(SYS_CLOSE, fd);
}

/* One entry of the archive, by index. Returns 0 when `out` holds a name, and
 * -WC_ENOENT past the last file — which is how a caller loop finds the end
 * without a separate "how many" call. The kernel copies the whole struct into
 * the buffer below, so it must be a real `struct wc_dirent`, not a pointer to
 * something the compiler may lay out differently. */
static inline long wc_getdirent(unsigned long index, struct wc_dirent *out)
{
    return wc_call2(SYS_GETDIRENT, (long)index, (long)out);
}

/* The calling process's id — the one thing a program cannot read from its own
 * address space and has to ask the kernel for. */
static inline long wc_getpid(void)
{
    return wc_call1(SYS_GETPID, 0);
}

/* Give the CPU up and come back later. This is the cooperative half of the
 * scheduler: the kernel switches to the next runnable process and returns here
 * as if the call had simply finished. Returns 0. */
static inline long wc_sched_yield(void)
{
    return wc_call1(SYS_SCHED_YIELD, 0);
}

/* Start another program from the archive; returns its pid, or a -WC_* error.
 * The new process shares nothing with us but the CPU: it gets its own address
 * space and its own copy of the argv convention we were handed. */
static inline long wc_spawn(const char *path)
{
    return wc_call1(SYS_SPAWN, (long)path);
}

/* The same, with a real command line: argc words from argv[] are copied into
 * the child's address space by the kernel, so the strings may live in our own
 * stack or .rodata — the child never points back at them. */
static inline long wc_spawn_args(const char *path, long argc, char **argv)
{
    return wc_call3(SYS_SPAWN, (long)path, argc, (long)argv);
}

/* Block until one of our children exits, then reap it: returns the child's
 * pid and, if code_out is not NULL, stores its exit code through code_out.
 * -WC_ECHILD when there is no child to wait for. This is the call that makes
 * the scheduler a *blocking* one — the CPU goes to another process while we
 * sleep, and this line of user code resumes only when a child dies. */
static inline long wc_wait(int *code_out)
{
    return wc_call1(SYS_WAIT, (long)code_out);
}

/* Does not come back: the kernel unwinds its own copy of this stack frame
 * instead of returning through it. The loop is for the compiler, not the CPU. */
static inline void wc_exit(int code)
{
    wc_call1(SYS_EXIT, code);
    for (;;)
        ;
}

/* End another process. Only SIGKILL exists, and it cannot be caught, blocked
 * or ignored — there is no delivery at all, the victim is simply retired. 0 on
 * success, -WC_ESRCH for a pid that is not alive (including one that already
 * exited and awaits its parent's wait), -WC_EINVAL for another signal.
 *
 * Killing your own pid is how a program exits with "I was killed" as its code;
 * that call does not return, so treat it exactly like wc_exit(). */
static inline long wc_kill(long pid, long sig)
{
    return wc_call2(SYS_KILL, pid, sig);
}

/* Write a literal's bytes without a strlen() call to link against. `sizeof s -
 * 1` is computed at compile time, which is also why the strings below are
 * arrays and not pointers. */
#define wc_puts(s) wc_write(STDOUT_FILENO, (s), sizeof(s) - 1)

#endif
