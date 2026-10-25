/* WindClone — system call dispatch (Phase 4).
 *
 * A syscall is not a function call: `int 0x80` from ring 3 makes the CPU
 * switch stacks (TSS.RSP0), push the user frame, and enter kernel_cs.
 * stubs.asm saves the GPRs and calls syscall_handler() with that frame; the
 * value we leave in frame->rax is what the program sees in rax when iretq
 * puts it back in ring 3.
 *
 * Everything below runs with user-controlled arguments, so it obeys one rule
 * per argument class:
 *
 *   - a *number* is clamped, never trusted (a 4 GiB length is not a 4 GiB
 *     copy);
 *   - an *address* is checked against the live page tables before it is
 *     touched. That is not paranoia about a wild pointer in a good program:
 *     the ELF loader now maps .text read-only, so "the page exists" and "the
 *     page may be written" are different questions, and a read() aimed at the
 *     caller's own code would fault *inside the kernel* — a page fault with no
 *     fixup table is a panic. Answer -WC_EFAULT instead;
 *   - a *string* is copied into a kernel buffer byte by byte, stopping at the
 *     first unmapped page or the first cap, so no vfs call ever walks a
 *     pointer the program can change underneath it.
 *
 * The file layer itself is not here: open/read/write/close forward to the VFS
 * in vfs.c, which is the same one the shell and the ELF loader use. Syscall.c
 * is only the border crossing.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include <stddef.h>

#include "syscall.h"
#include "vfs.h"
#include "usermode.h"
#include "proc.h"
#include "paging.h"
#include "idt.h"
#include "serial.h"
#include "string.h"

extern void syscall_stub(void);

/* One page is as much as a single call may move, and one screen-width path is
 * as long as a name may be. Both caps exist so the address check below can
 * stay a short loop instead of a scan of the whole requested range. */
#define USER_BUF_MAX  4096ULL
#define USER_PATH_MAX 128

void syscall_init(void)
{
    /* type_attr 0xEE = present, DPL 3, 64-bit interrupt gate. DPL 3 is the
     * whole point: a ring-0 gate would give the program a #GP instead. */
    idt_set_gate_dpl(0x80, (const void *)syscall_stub, 3);
    serial_write("syscall: int 0x80 gate installed (DPL 3)\n");
}

/* Can the kernel reach [addr, addr+len) the way a program can — mapped, user
 * accessible, and (when asked) writable? Callers must clamp `len` first. */
static int user_ptr_ok(uint64_t addr, uint64_t len, int writable)
{
    if (!user_range_ok(addr, len))
        return 0;

    uint64_t page = addr & ~0xFFFULL;
    uint64_t end = addr + len;
    for (; page < end; page += 0x1000) {
        uint64_t flags = paging_flags(page);
        if (!(flags & PTE_PRESENT))
            return 0;
        /* A mapping the program could not reach at CPL 3 is not its buffer,
         * whatever window it sits in. */
        if (!(flags & PTE_USER))
            return 0;
        if (writable && !(flags & PTE_WRITE))
            return 0;
    }
    return 1;
}

/* write(fd, buf, len) */
static int64_t sys_write(uint64_t fd, uint64_t buf, uint64_t len)
{
    if (len > USER_BUF_MAX)
        len = USER_BUF_MAX;
    if (!user_ptr_ok(buf, len, 0))
        return WC_EFAULT;
    return vfs_write((int)fd, (const void *)(uintptr_t)buf, len);
}

/* read(fd, buf, len) — buf must be writable, which is the whole reason
 * user_ptr_ok() bothers with the WRITE bit at all. */
static int64_t sys_read(uint64_t fd, uint64_t buf, uint64_t len)
{
    if (len > USER_BUF_MAX)
        len = USER_BUF_MAX;
    if (!user_ptr_ok(buf, len, 1))
        return WC_EFAULT;
    return vfs_read((int)fd, (void *)(uintptr_t)buf, len);
}

/* open(path) — copy the name out first, then resolve it. */
static int64_t sys_open(uint64_t upath)
{
    char path[USER_PATH_MAX];

    if (!user_ptr_ok(upath, 1, 0))
        return WC_EFAULT;

    size_t i = 0;
    int terminated = 0;
    for (; i + 1 < USER_PATH_MAX; i++) {
        /* Re-check per byte rather than once for the whole cap: a 3-byte name
         * at the end of a page must not be rejected because the 125 bytes
         * after its NUL happen to be unmapped. */
        if (!user_ptr_ok(upath + i, 1, 0))
            break;
        path[i] = *(const char *)(uintptr_t)(upath + i);
        if (path[i] == '\0') {
            terminated = 1;
            break;
        }
    }
    if (!terminated) {
        if (i + 1 >= USER_PATH_MAX)
            return WC_ENAMETOOLONG;
        return WC_EFAULT;          /* the mapping ended before the NUL did */
    }
    if (i == 0)
        return WC_EINVAL;

    return vfs_open(path);
}

static int64_t sys_close(uint64_t fd)
{
    return vfs_close((int)fd);
}

/* getdirent(index, struct wc_dirent *out) — the one call that hands a program
 * a structure instead of bytes. The kernel fills its own copy, checks the
 * caller's address, then moves it in one piece: the VFS is never given a
 * ring-3 pointer, so a half-written entry cannot exist even if the program
 * passes a buffer one byte from the end of its stack. */
static int64_t sys_getdirent(uint64_t index, uint64_t up)
{
    struct wc_dirent d;

    if (!user_ptr_ok(up, sizeof d, 1))
        return WC_EFAULT;

    int64_t r = vfs_dirent(index, &d);
    if (r != 0)
        return r;

    memcpy((void *)(uintptr_t)up, &d, sizeof d);
    return 0;
}

/* spawn(path, argc, argv) — start another program from the archive and return
 * its pid. The name is copied out per byte exactly as open() does, so a
 * program cannot move the string under the kernel mid-call.
 *
 * argv arrives as a pointer array in the *caller's* address space, and the
 * child gets a *private* one — so every string is copied twice: out of ring 3
 * into these kernel bounce buffers (validated per byte against the caller's
 * live tables), then from the buffers onto the child's stack by proc_create.
 * A static arena rather than the stack because USER_ARG_MAX × USER_PATH_MAX
 * is a KiB-scale request and a syscall handler should not grow that per call.
 * argc <= 0 keeps the old path-only contract: the child runs as its own name,
 * exactly like the boot demos' NULL argv. */
static char spawn_argbuf[USER_ARG_MAX][USER_PATH_MAX];
static const char *spawn_argv[USER_ARG_MAX];

static int64_t sys_spawn(uint64_t upath, uint64_t argc, uint64_t uargv)
{
    char path[USER_PATH_MAX];

    if (!user_ptr_ok(upath, 1, 0))
        return WC_EFAULT;

    size_t i = 0;
    int terminated = 0;
    for (; i + 1 < USER_PATH_MAX; i++) {
        if (!user_ptr_ok(upath + i, 1, 0))
            break;
        path[i] = *(const char *)(uintptr_t)(upath + i);
        if (path[i] == '\0') {
            terminated = 1;
            break;
        }
    }
    if (!terminated) {
        if (i + 1 >= USER_PATH_MAX)
            return WC_ENAMETOOLONG;
        return WC_EFAULT;
    }
    if (i == 0)
        return WC_EINVAL;

    int n = (int)argc;
    if (n <= 0) {
        int solo = proc_spawn_path(path);
        return solo < 0 ? WC_ENOENT : (int64_t)solo;
    }
    if (n > USER_ARG_MAX)
        return WC_EINVAL;
    if (!user_ptr_ok(uargv, (uint64_t)n * 8, 0))
        return WC_EFAULT;

    for (int a = 0; a < n; a++) {
        uint64_t ua = *(const uint64_t *)(uintptr_t)(uargv + (uint64_t)a * 8);
        if (!user_ptr_ok(ua, 1, 0))
            return WC_EFAULT;
        size_t j = 0;
        int done = 0;
        for (; j + 1 < USER_PATH_MAX; j++) {
            if (!user_ptr_ok(ua + j, 1, 0))
                break;
            spawn_argbuf[a][j] = *(const char *)(uintptr_t)(ua + j);
            if (spawn_argbuf[a][j] == '\0') {
                done = 1;
                break;
            }
        }
        if (!done)
            return WC_EINVAL;      /* unterminated or over-long argument */
        spawn_argv[a] = spawn_argbuf[a];
    }

    int pid = proc_create(path, n, spawn_argv, 0);
    return pid < 0 ? WC_ENOENT : (int64_t)pid;
}

void syscall_handler(struct syscall_frame *f)
{
    switch (f->rax) {
    case SYS_WRITE:
        f->rax = (uint64_t)sys_write(f->rdi, f->rsi, f->rdx);
        break;

    case SYS_READ:
        f->rax = (uint64_t)sys_read(f->rdi, f->rsi, f->rdx);
        break;

    case SYS_OPEN:
        f->rax = (uint64_t)sys_open(f->rdi);
        break;

    case SYS_CLOSE:
        f->rax = (uint64_t)sys_close(f->rdi);
        break;

    case SYS_GETDIRENT:
        f->rax = (uint64_t)sys_getdirent(f->rdi, f->rsi);
        break;

    case SYS_GETPID:
        /* The scheduler's answer; 0 when a program runs under the shell's
         * single-program path, which has no process table entry. */
        f->rax = (uint64_t)proc_current_pid();
        break;

    case SYS_SCHED_YIELD:
        /* Cooperative switch: proc_yield() replaces *f with the next process's
         * context (and loads its CR3), so the stub's iretq resumes *that*
         * program. Do not touch f after this — it no longer describes the
         * caller. The yielding process sees 0 when it is later resumed. */
        proc_yield(f);
        break;

    case SYS_SPAWN:
        f->rax = (uint64_t)sys_spawn(f->rdi, f->rsi, f->rdx);
        break;

    case SYS_WAIT:
        /* The one call that may not return into the caller: proc_wait() either
         * reaps a zombie right here (result in rax, ordinary return) or saves
         * this frame and switches, so the iretq below resumes *another*
         * process and this call "returns" only when a child's exit wakes it. */
        f->rax = (uint64_t)proc_wait(f, f->rdi);
        break;

    case SYS_EXIT:
        /* Under the scheduler, exit retires the process and moves on; it only
         * "returns" into the next context. Outside it (a shell command) there
         * is no table, so unwind to the single-program caller as before. */
        if (proc_scheduler_active())
            proc_exit(f, (int)f->rdi);
        else
            usermode_exit((int)f->rdi);
        break;

    default:
        serial_write("syscall: unknown call ");
        serial_write_dec(f->rax);
        serial_write(" from ring 3\n");
        f->rax = (uint64_t)WC_ENOSYS;
        break;
    }
}
