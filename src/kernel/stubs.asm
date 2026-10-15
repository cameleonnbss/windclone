; WindClone — interrupt stubs (exceptions + hardware IRQs), linked into
; the kernel ELF as an elf64 object (nasm -f elf64).
;
; Contract with idt.c / struct regs:
;   - the CPU has already pushed ss/rsp/rflags/cs/rip (+ error code for
;     some exceptions) by the time a stub runs;
;   - each stub pushes a vector number (and a dummy 0 when the CPU did
;     not supply an error code) so every frame looks identical;
;   - we then save all GPRs and pass rsp in rdi (System V ABI) to C.
;
; SPDX-License-Identifier: MIT

BITS 64
section .text

extern isr_handler      ; C: noreturn, fatal exception report (struct regs *)
extern irq_handler      ; C: void irq_handler(struct regs *) — IRQ in regs->vector

; --- the 32 CPU exceptions -------------------------------------------
; %1 = vector. ISR_NOERR adds a dummy error code so the stack layout
; is uniform; ISR_ERR leaves the CPU-pushed error code in place.
%macro ISR_NOERR 1
global isr_stub_%1
isr_stub_%1:
    push qword 0
    push qword %1
    jmp isr_common
%endmacro

%macro ISR_ERR 1
global isr_stub_%1
isr_stub_%1:
    push qword %1       ; vector; the error code is already on the stack
    jmp isr_common
%endmacro

ISR_NOERR 0             ; divide error
ISR_NOERR 1             ; debug
ISR_NOERR 2             ; non-maskable interrupt
ISR_NOERR 3             ; breakpoint
ISR_NOERR 4             ; overflow
ISR_NOERR 5             ; bound range exceeded
ISR_NOERR 6             ; invalid opcode
ISR_NOERR 7             ; device not available
ISR_ERR   8             ; double fault
ISR_NOERR 9             ; (old) coprocessor segment overrun
ISR_ERR   10            ; invalid TSS
ISR_ERR   11            ; segment not present
ISR_ERR   12            ; stack-segment fault
ISR_ERR   13            ; general protection fault
ISR_ERR   14            ; page fault
ISR_NOERR 15            ; reserved
ISR_NOERR 16            ; x87 floating-point exception
ISR_ERR   17            ; alignment check
ISR_NOERR 18            ; machine check
ISR_NOERR 19            ; SIMD floating-point exception
ISR_NOERR 20            ; virtualisation exception
ISR_NOERR 21            ; reserved
ISR_NOERR 22            ; reserved
ISR_NOERR 23            ; reserved
ISR_NOERR 24            ; reserved
ISR_NOERR 25            ; reserved
ISR_NOERR 26            ; reserved
ISR_NOERR 27            ; reserved
ISR_NOERR 28            ; reserved
ISR_NOERR 29            ; reserved
ISR_NOERR 30            ; reserved
ISR_ERR   31            ; software-completed exception

isr_common:
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    mov  rdi, rsp         ; struct regs * (arg 1, SysV ABI)
    cld                   ; SysV ABI: direction flag must be clear
    call isr_handler      ; noreturn — never comes back
    ; (unreachable, but keep the frame valid in case it ever does)
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    add  rsp, 16          ; drop vector + error code
    iretq

; --- the 16 hardware IRQs (remapped by PIC to vectors 32..47) --------
%macro IRQ_STUB 1         ; %1 = IRQ number 0..15
global irq_stub_%1
irq_stub_%1:
    push qword 0          ; no error code from hardware
    push qword %1 + 32    ; vector = IRQ + remap offset
    jmp irq_common
%endmacro

IRQ_STUB 0              ; PIT timer
IRQ_STUB 1              ; keyboard
IRQ_STUB 2              ; cascade (slave PIC)
IRQ_STUB 3
IRQ_STUB 4
IRQ_STUB 5
IRQ_STUB 6
IRQ_STUB 7
IRQ_STUB 8              ; RTC
IRQ_STUB 9
IRQ_STUB 10
IRQ_STUB 11
IRQ_STUB 12
IRQ_STUB 13
IRQ_STUB 14             ; ATA primary
IRQ_STUB 15             ; ATA secondary

irq_common:
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    mov  rdi, rsp
    cld
    call irq_handler
    pop  r15
    pop  r14
    pop  r13
    pop  r12
    pop  r11
    pop  r10
    pop  r9
    pop  r8
    pop  rbp
    pop  rdi
    pop  rsi
    pop  rdx
    pop  rcx
    pop  rbx
    pop  rax
    add  rsp, 16
    iretq

; (the `global` declarations live inside the ISR_*/IRQ_STUB macros)

section .note.GNU-stack noalloc noexec nowrite progbits
