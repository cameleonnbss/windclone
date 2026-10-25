; WindClone — /bin/hello: the first program that is not the kernel.
;
; It used to be assembled with `nasm -f bin` and an `org`, one blob of bytes
; the kernel copied to an address it picked itself. Now it is an elf64 object
; linked by linkers/user.ld into a real ELF64, which is the smallest program
; that still exercises the format: the string lives in .rodata, a different
; PT_LOAD from the code that prints it, so the kernel has to place two things
; at two addresses with two protections to run eight instructions.
;
; ABI (src/kernel/wcabi.h):
;   rax = call number, rdi/rsi/rdx/r10 = args, result in rax
;
; SPDX-License-Identifier: MIT

BITS 64

section .text
global _start
_start:
    ; write(1, msg, len)
    mov  rax, 1                 ; SYS_WRITE
    mov  rdi, 1                 ; STDOUT_FILENO
    lea  rsi, [rel msg]         ; the kernel reads this through our pages
    mov  rdx, msg_len
    int  0x80

    ; exit(0)
    mov  rax, 2                 ; SYS_EXIT
    xor  rdi, rdi
    int  0x80

    hlt                         ; belt and braces: exit() already left us

section .rodata
msg:     db "hello from ring 3: this line was written by a user program", 10
msg_len  equ $ - msg
