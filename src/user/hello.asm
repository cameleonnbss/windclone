; WindClone — the first program that is not the kernel.
;
; Assembled as a flat binary (nasm -f bin) with `org` matching USER_BASE,
; so the linker-less output is exactly the bytes the kernel copies into
; user pages. It does the two things Phase 4 promises: write() to the
; console and exit(code), both through the int 0x80 gate.
;
; ABI (see src/kernel/syscall.h):
;   rax = call number, rdi/rsi/rdx = args, result in rax
;
; SPDX-License-Identifier: MIT

BITS 64
org 0x400000

start:
    ; write(1, msg, len)
    mov  rax, 1                 ; SYS_WRITE
    mov  rdi, 1                 ; SYS_FD_STDOUT
    lea  rsi, [rel msg]         ; the kernel reads this through our pages
    mov  rdx, msg_len
    int  0x80

    ; exit(0)
    mov  rax, 2                 ; SYS_EXIT
    xor  rdi, rdi
    int  0x80

    hlt                         ; belt and braces: exit() already left us

msg:     db "hello from ring 3: this line was written by a user program", 10
msg_len  equ $ - msg
