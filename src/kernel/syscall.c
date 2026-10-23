/* WindClone — system call dispatch (Phase 4).
 *
 * A syscall is not a function call: `int 0x80` from ring 3 makes the CPU
 * switch stacks (TSS.RSP0), push the user frame, and enter kernel_cs.
 * stubs.asm saves the GPRs and calls syscall_handler() with that frame;
 * the value we leave in frame->rax is what the program sees in rax when
 * iretq puts it back in ring 3.
 *
 * Everything below therefore runs with user-controlled arguments. Even
 * for two calls, the rules are the same as they will be in Phase 7:
 *   - never trust a user pointer (we only read it here, and the pages it
 *     walks are the ones we mapped, so a bad pointer faults in *our* page
 *     fault handler rather than corrupting something);
 *   - never let a failed call return garbage — it returns -1.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include <stddef.h>

#include "syscall.h"
#include "idt.h"
#include "serial.h"
#include "vga.h"
#include "usermode.h"

extern void syscall_stub(void);

void syscall_init(void)
{
    /* type_attr 0xEE = present, DPL 3, 64-bit interrupt gate. DPL 3 is the
     * whole point: a ring-0 gate would give the program a #GP instead. */
    idt_set_gate_dpl(0x80, (const void *)syscall_stub, 3);
    serial_write("syscall: int 0x80 gate installed (DPL 3)\n");
}

/* write(fd, buf, len) — the first thing a program may ask the kernel for.
 * fd 1/2 go to the serial console, which is also what CI reads. */
static int64_t sys_write(uint64_t fd, const char *buf, uint64_t len)
{
    if (fd != SYS_FD_STDOUT && fd != SYS_FD_STDERR)
        return -1;
    if (len > 4096)                 /* one page: no unbounded user reads */
        len = 4096;

    serial_write_len(buf, (size_t)len);
    return (int64_t)len;
}

void syscall_handler(struct syscall_frame *f)
{
    switch (f->rax) {
    case SYS_WRITE:
        f->rax = (uint64_t)sys_write(f->rdi, (const char *)(uintptr_t)f->rsi, f->rdx);
        break;

    case SYS_EXIT:
        /* Never returns: unwinds to whoever called usermode_enter(). */
        usermode_exit((int)f->rdi);
        break;

    default:
        serial_write("syscall: unknown call ");
        serial_write_dec(f->rax);
        serial_write(" from ring 3\n");
        f->rax = (uint64_t)-1;
        break;
    }
}
