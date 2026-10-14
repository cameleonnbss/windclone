; WindClone — educational stage-0 bootloader: a hand-written 512-byte MBR.
;
; This file is NOT used by the ISO (Limine does the heavy lifting there).
; It exists so we understand, from assembly, how a PC starts:
;   1. Firmware reads LBA 0 of the disk (512 bytes) and jumps to 0x7C00.
;   2. We stay in 16-bit real mode, print via the BIOS teletype service,
;      then hang. Later roadmap phases extend this into a real loader
;      (A20 gate, GDT, long mode...) — or we let Limine do it, which is
;      what the actual ISO build does.
;
; Build & run:  make mbr && make run-mbr
; SPDX-License-Identifier: MIT

BITS 16
ORG 0x7C00

start:
    cli                     ; no interrupts until we set up our own
    xor ax, ax
    mov ds, ax              ; data segment  = 0 (real mode flat)
    mov ss, ax
    mov sp, 0x7C00          ; stack grows down below the boot sector
    sti

    mov si, msg_bios
    call print_string

    jmp $                   ; infinite loop = "halt" in real mode

; --- BIOS teletype: print NUL-terminated string at DS:SI ------------
print_string:
    lodsb                   ; load next byte, advance SI
    or al, al
    jz .done
    mov ah, 0x0E
    int 0x10
    jmp print_string
.done:
    ret

msg_bios:
    db 13, 10
    db "WindClone stage-0 (MBR) reached. CPU is in 16-bit real mode."
    db 13, 10
    db "This is how every x86 PC begins: firmware -> LBA0 -> 0x7C00.", 13, 10
    db 0

times 510-($-$$) db 0       ; pad to the signature offset
dw 0xAA55                   ; BIOS magic: "this is a boot sector"
