; WindClone — educational stage-2 bootloader.
;
; The MBR (stage-0) reads this file's sectors from LBA1 into physical
; memory 0x80000 and jumps to CS:IP = 0x8000:0. We re-point DS at 0x8000
; so our labels (= offsets from 0x80000) resolve through DS:SI.
;
; Real bootloaders grow through exactly these stages; the next ones
; (A20 gate, switching to protected mode, then long mode) are what
; Limine already does for our ISO — see docs/ROADMAP.md.
;
; SPDX-License-Identifier: MIT

BITS 16
; assembled with ORG 0: loaded at 0x8000:0 by the MBR

start:
    ; Our labels are offsets inside this blob (loaded at physical
    ; 0x80000), so the data segment must be 0x8000 for DS:SI to hit them.
    mov ax, 0x8000
    mov ds, ax
    mov es, ax
    xor ax, ax
    mov ss, ax
    mov sp, 0x7C00           ; reuse the stack area under the MBR

    mov si, msg_stage2
    call print_string
    jmp $                    ; end of the educational line for now

print_string:
    lodsb
    or al, al
    jz .done
    mov ah, 0x0E
    int 0x10
    jmp print_string
.done:
    ret

msg_stage2:
    db 13, 10
    db "WindClone stage-2 alive: MBR read me from LBA1 via int 13h."
    db 13, 10
    db "Two-stage loading is how every big bootloader chains code.", 0
