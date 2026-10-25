/* WindClone — system calls (Phase 4).
 *
 * The ABI is the one thing here that cannot change later without breaking
 * every program built for this system, so it is fixed on day one:
 *
 *   rax = call number, rdi/rsi/rdx/r10 = arguments, result back in rax.
 *   Entered with `int 0x80` (a DPL-3 interrupt gate, so ring 3 may use it).
 *
 * The numbers, the argument registers and the error values are not here: they
 * live in wcabi.h, the one header the kernel and a user program both include,
 * so a call number can never drift apart from its userland declaration.
 *
 * Five calls now — write, read, open, close, exit. They are the whole of
 * Phase 4's promise: ring-3 code asking the kernel for bytes it may not touch
 * itself, and a controlled way back into the kernel.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef WINDCLONE_SYSCALL_H
#define WINDCLONE_SYSCALL_H

#include <stdint.h>

#include "wcabi.h"     /* SYS_*, *_FILENO, WC_* — the shared half of the ABI */

/* Interrupt frame as the stub builds it. The first part mirrors
 * struct regs exactly (see idt.h); the two fields after it are only
 * present when the CPU crossed a privilege boundary — which is exactly
 * the case for every syscall. */
struct syscall_frame {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;   /* rax = call number / result */
    uint64_t vector;
    uint64_t error_code;
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t user_rsp;   /* only valid for ring-3 -> ring-0 transitions */
    uint64_t user_ss;
};

void syscall_init(void);   /* install the DPL-3 gate at vector 0x80 */

#endif
