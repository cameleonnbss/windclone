/* WindClone — IDT, CPU exceptions and the 8259 PIC.
 * SPDX-License-Identifier: MIT
 */
#ifndef WINDCLONE_IDT_H
#define WINDCLONE_IDT_H

#include <stdint.h>

/* One interrupt frame. The field order MUST mirror the push order in
 * src/kernel/stubs.asm (r15 pushed last => first in the struct).
 * Each stub pushes error-code/dummy FIRST and the vector SECOND, so
 * the vector sits closer to rsp (lower address) than error_code.
 * The CPU pushed rip/cs/rflags before our stub ran, so they sit right
 * above the error word — ring-0 handlers get a 3-word iret frame. */
struct regs {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector;
    uint64_t error_code;
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
};

void idt_init(void);          /* build + load the 256-entry IDT */
void pic_remap(void);         /* IRQ 0..15 -> vectors 32..47    */
void pic_mask(uint8_t irq);   /* silence one hardware line      */
void pic_unmask(uint8_t irq); /* allow one hardware line again  */

#endif
