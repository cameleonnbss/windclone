/* WindClone — the program's side of the syscall boundary.
 *
 * A freestanding program has no libc, so these three inline wrappers are the
 * whole standard library for now. They are correct only because of one fact
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

/* Does not come back: the kernel unwinds its own copy of this stack frame
 * instead of returning through it. The loop is for the compiler, not the CPU. */
static inline void wc_exit(int code)
{
    wc_call1(SYS_EXIT, code);
    for (;;)
        ;
}

/* Write a literal's bytes without a strlen() call to link against. `sizeof s -
 * 1` is computed at compile time, which is also why the strings below are
 * arrays and not pointers. */
#define wc_puts(s) wc_write(STDOUT_FILENO, (s), sizeof(s) - 1)

#endif
