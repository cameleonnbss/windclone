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

/* open(path) — copy the name out first, then resolve it.
 *
 * The copy is the security-relevant part and it is shared with open2 below, so
 * it lives in one function: a program must not be able to make the kernel read a
 * path that changes underneath it, and it must not be able to trip a fault by
 * pointing at a page that ends before its string does. */
static int64_t copy_user_path(uint64_t upath, char *path, size_t cap)
{
    if (!user_ptr_ok(upath, 1, 0))
        return WC_EFAULT;

    size_t i = 0;
    int terminated = 0;
    for (; i + 1 < cap; i++) {
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
        if (i + 1 >= cap)
            return WC_ENAMETOOLONG;
        return WC_EFAULT;          /* the mapping ended before the NUL did */
    }
    if (i == 0)
        return WC_EINVAL;
    return (int64_t)i;
}

static int64_t sys_open(uint64_t upath)
{
    char path[USER_PATH_MAX];

    int64_t n = copy_user_path(upath, path, sizeof path);
    if (n < 0)
        return n;
    return vfs_open(path);
}

/* open2(path, flags) — the same border crossing with the intent attached. The
 * flags argument is a *number*, so the rule is the other one: clamp and mask,
 * never trust. Anything outside the WC_O_* set is dropped here rather than
 * passed on, so a program cannot turn an unimplemented bit into behaviour the
 * VFS did not choose. */
static int64_t sys_open2(uint64_t upath, uint64_t flags)
{
    char path[USER_PATH_MAX];

    int64_t n = copy_user_path(upath, path, sizeof path);
    if (n < 0)
        return n;
    return vfs_open_flags(path, flags & (WC_O_ACCMODE | WC_O_CREAT |
                                         WC_O_TRUNC | WC_O_EXCL));
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
 * exactly like the boot demos' NULL argv.
 *
 * Outside the scheduler there is still nothing that would ever run the child,
 * so the answer stays -WC_EPERM rather than a pid for a process nobody will
 * schedule. What changed is how wide that "outside" is: the desktop shell now
 * owns a slot in the table (proc.h) and starts its programs with the scheduler
 * running, so a program typed at the prompt or clicked in the start menu has a
 * parent, a pid, and working process verbs. The only caller left in the refusal
 * is the boot self-check, which runs one program through usermode_run() and
 * never asks to spawn anything. */
static char spawn_argbuf[USER_ARG_MAX][USER_PATH_MAX];
static const char *spawn_argv[USER_ARG_MAX];

static int64_t sys_spawn(uint64_t upath, uint64_t argc, uint64_t uargv)
{
    char path[USER_PATH_MAX];

    if (!proc_scheduler_active())
        return WC_EPERM;

    int64_t name_len = copy_user_path(upath, path, sizeof path);
    if (name_len < 0)
        return name_len;

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
        /* fd 0 is the keyboard, and it is the one descriptor a program can *wait*
         * for. That is why the call is made here instead of inside sys_read(): a
         * function that must return a number cannot also save the caller's frame
         * and switch away, and proc_input_read() does exactly that when the keys
         * have not arrived yet. The buffer is validated against *this* process's
         * page tables before anything can block — a bad pointer is the caller's
         * -WC_EFAULT, never a kernel fault — and the bytes themselves are written
         * later, under the reader's own CR3, by wakeup_deliver(). Every other fd
         * keeps the ordinary non-blocking path, including fd 0 outside the
         * scheduler, where there is no table to sleep in and read() still answers
         * -WC_ENOSYS. */
        if (f->rdi == 0) {
            uint64_t rn = f->rdx > USER_BUF_MAX ? USER_BUF_MAX : f->rdx;
            if (!user_ptr_ok(f->rsi, rn, 1)) {
                f->rax = (uint64_t)WC_EFAULT;
                break;
            }
            proc_input_read(f, f->rsi, rn);
            break;
        }
        f->rax = (uint64_t)sys_read(f->rdi, f->rsi, f->rdx);
        break;

    case SYS_OPEN:
        f->rax = (uint64_t)sys_open(f->rdi);
        break;

    case SYS_OPEN2:
        /* rsi is the caller's flags word. It is only ever read here, never by
         * SYS_OPEN, which is the whole reason this call needed a new number: a
         * binary built before v0.1.18 leaves garbage in rsi, and garbage that
         * means "O_CREAT" would be a file appearing in someone else's
         * filesystem. */
        f->rax = (uint64_t)sys_open2(f->rdi, f->rsi);
        break;

    case SYS_CLOSE:
        f->rax = (uint64_t)sys_close(f->rdi);
        break;

    case SYS_GETDIRENT:
        f->rax = (uint64_t)sys_getdirent(f->rdi, f->rsi);
        break;

    case SYS_GETPID:
        /* The scheduler's answer. It used to be 0 for everything the shell
         * started, because the shell's single-program path had no table entry
         * to name; programs launched from the prompt or the start menu are in
         * the table now, so they get a real pid. 0 remains honest for the boot
         * self-check's usermode_run(), which is still not a process. */
        f->rax = (uint64_t)proc_current_pid();
        break;

    case SYS_GETPPID:
        /* The parent as the table owns it at this instant, which is the point:
         * a process whose parent died has been re-parented to the adopter by
         * then (proc.c), so this answers with pid 1 instead of with the pid of
         * something that no longer exists. It is the only call here whose result
         * can change while a program runs, and no argument to validate — the
         * answer is a number the caller cannot influence. */
        f->rax = (uint64_t)proc_current_ppid();
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

    case SYS_KILL:
        /* Ending somebody else is a table edit, not a switch — the victim is by
         * definition not on the CPU, so the call returns into the killer with
         * 0 or -WC_ESRCH/-WC_EINVAL like any other. The exception is self-kill:
         * proc_kill() routes that through the exit arm and never returns, so
         * the assignment below simply does not happen for the caller. */
        f->rax = (uint64_t)proc_kill((int)f->rdi, (int)f->rsi, f);
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
