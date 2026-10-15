; WindClone — educational stage-0 bootloader: a hand-written 512-byte MBR.
;
; This file is NOT used by the ISO (Limine does the heavy lifting there).
; It exists so we understand, from assembly, how a PC starts:
;   1. Firmware reads LBA 0 of the disk (512 bytes) and jumps to 0x7C00.
;   2. We stay in 16-bit real mode, print via the BIOS teletype service,
;      then use BIOS disk service int 13h to read the NEXT sectors from
;      the disk (that's stage-2, src/boot/stage2.asm, sitting at LBA1)
;      and jump into them at 0x8000.
;
; That load-more-sectors-then-jump trick is the entire idea behind every
; multi-stage bootloader in existence, including Limine's own BIOS path.
;
; Build & run:  make mbr && make run-mbr
; SPDX-License-Identifier: MIT

BITS 16
ORG 0x7C00

STAGE2_SEG  equ 0x8000        ; load stage-2 at physical 0x80000
STAGE2_CNT  equ 4             ; sectors we give stage-2 (2 KiB)

start:
    cli                     ; no interrupts until we set up our own
    xor ax, ax
    mov ds, ax              ; data segment  = 0 (real mode flat)
    mov ss, ax
    mov sp, 0x7C00          ; stack grows down below the boot sector
    sti

    mov [boot_drive], dl    ; BIOS tells us which disk we booted from

    mov si, msg_stage0
    call print_string

; --- load stage-2: BIOS int 13h, AH=02h (read sectors, CHS) ----------
; Retry a few times: real floppy/USB firmware is flaky on the first try.
; NOTE: set ES/AX *before* AH/AL — `mov ax, ...` would otherwise wipe
; the function number and sector count we just placed in AH/AL.
    mov byte [retry], 3
.load_loop:
    mov ax, STAGE2_SEG      ; es:bx = 0x8000:0 -> physical 0x80000
    mov es, ax              ; (stage-2 lands well above our 0x7C00)
    xor bx, bx
    mov ah, 0x02
    mov al, STAGE2_CNT
    mov ch, 0               ; cylinder 0
    mov cl, 2               ; sector 2 (CHS is 1-based; = LBA1)
    mov dh, 0               ; head 0
    mov dl, [boot_drive]
    int 0x13
    jnc .loaded
    mov [last_err], ah          ; keep BIOS error code for the fail message
    mov [last_drv], dl
    dec byte [retry]
    jz fail_disk
    xor ah, ah             ; reset the disk subsystem
    int 0x13
    jmp .load_loop
.loaded:
    mov si, msg_jump
    call print_string
    jmp dword STAGE2_SEG:0  ; far jump: new CS:IP -> physical 0x80000

fail_disk:
    mov si, msg_disk_fail
    call print_string
    mov al, [last_drv]      ; show drive byte, then error code byte
    call print_hex
    mov al, [last_err]
    call print_hex
    jmp $

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

; --- print AL as two hex digits (tiny diagnostic helper) -------------
print_hex:
    push bx
    mov bl, al
    shr al, 4               ; high nibble first
    call print_nibble
    mov al, bl
    and al, 0x0F            ; then low nibble
    call print_nibble
    pop bx
    ret
print_nibble:
    cmp al, 9
    jbe .num
    add al, 'A' - 10
    jmp .out
.num:
    add al, '0'
.out:
    mov ah, 0x0E
    int 0x10
    ret

boot_drive: db 0
retry:      db 0
last_err:   db 0
last_drv:   db 0

msg_stage0:
    db 13, 10
    db "WindClone stage-0 (MBR): 16-bit real mode, jumping to disk read."
    db 13, 10, 0

msg_jump:
    db "stage-2 loaded at 0x8000, transferring control...", 13, 10, 0

msg_disk_fail:
    db "Disk read failed - cannot load stage-2.", 13, 10, 0

times 510-($-$$) db 0       ; pad to the signature offset
dw 0xAA55                   ; BIOS magic: "this is a boot sector"
