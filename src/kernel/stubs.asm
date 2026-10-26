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
extern syscall_handler  ; C: void syscall_handler(struct syscall_frame *)

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

; =====================================================================
; Phase 4 — privilege transition plumbing
; =====================================================================

; --- load our GDT and reload every segment selector -------------------
; Selectors are constants here because this is the one moment where the
; CPU has no usable data segment: the far return is what gives CS its new
; value, and only then can the others follow.
;   rdi = pointer to the GDTR image (limit + 64-bit base)
global gdt_flush
gdt_flush:
    lgdt [rdi]
    push qword 0x08          ; kernel code selector
    lea  rax, [rel .reloaded]
    push rax                 ; far return: CS = 0x08
    retfq
.reloaded:
    mov  ax, 0x10            ; kernel data selector
    mov  ds, ax
    mov  es, ax
    mov  ss, ax
    mov  fs, ax
    mov  gs, ax
    ret

; --- load the task register with our TSS selector --------------------
;   di = selector
global tss_flush
tss_flush:
    mov ax, di
    ltr ax
    ret

; --- enter ring 3 ----------------------------------------------------
;   rdi = entry point, rsi = user stack pointer,
;   rdx = argc, rcx = argv (what _start sees in rdi/rsi, see wcabi.h)
;
; iretq pops rip/cs/rflags/rsp/ss when it crosses to a lower privilege
; level, so the five words are pushed in reverse. RFLAGS.IF stays set:
; the timer must keep ticking in userland, and that first tick landing on
; a ring-3 stack is exactly what the TSS is for.
;
; argc/argv are moved into place only AFTER the frame is pushed: the
; pushed rip and rsp are what the CPU loads, and rdi/rsi happen to hold
; them, so overwriting the argument registers last keeps the handover
; honest. The stack carries the same two values the psABI way, which is
; what a Linux binary actually reads.
;
; The current rsp/rbp are stashed first so sys_exit() can come back here:
; a syscall cannot "return" through iretq to a kernel caller, it has to
; unwind to the frame that called usermode_enter().
;
; rbx and r12-r15 are stashed for the same reason, and this is an ABI fix,
; not decoration: C callers are entitled to assume a function preserves the
; callee-saved registers, and everything ring 3 does afterwards — the
; process's own register values, the frame proc.c copies over a syscall
; frame on a context switch — lands in exactly those registers. Before the
; scheduler, nothing of a caller's happened to be live in them across the
; excursion, so the violation was invisible; with two processes it was not:
; a kernel pointer parked in r14 across proc_run_all() came back holding a
; dead process's context. user_exit_to_kernel gives them back.
global usermode_enter
usermode_enter:
    mov [rel user_kernel_rsp], rsp
    mov [rel user_kernel_rbp], rbp
    mov [rel user_saved_rbx], rbx
    mov [rel user_saved_r12], r12
    mov [rel user_saved_r13], r13
    mov [rel user_saved_r14], r14
    mov [rel user_saved_r15], r15
    mov ax, 0x1B             ; ring-3 data selector (0x18 | 3)
    mov ds, ax
    mov es, ax
    push qword 0x1B          ; ss
    push rsi                 ; rsp
    push qword 0x202         ; rflags: IF=1, bit 1 reserved-set
    push qword 0x23          ; cs = ring-3 code selector (0x20 | 3)
    push rdi                 ; rip
    mov  rdi, rdx            ; _start(argc,
    mov  rsi, rcx            ;        argv)
    iretq

; --- leave ring 3 and resume the kernel caller -----------------------
global user_exit_to_kernel
user_exit_to_kernel:
    mov ax, 0x10             ; back to kernel data before touching memory
    mov ds, ax
    mov es, ax
    mov rbx, [rel user_saved_rbx]
    mov r12, [rel user_saved_r12]
    mov r13, [rel user_saved_r13]
    mov r14, [rel user_saved_r14]
    mov r15, [rel user_saved_r15]
    mov rsp, [rel user_kernel_rsp]
    mov rbp, [rel user_kernel_rbp]
    ; int 0x80 is an interrupt gate: the CPU cleared IF on the way in and
    ; iretq is not what returns us to the kernel, so nothing else would
    ; ever turn interrupts back on. Without this `sti` the kernel resumes
    ; the idle loop with IF=0 and the first hlt() never wakes up again.
    sti
    ret

; --- resume a process from a saved frame (the idle-driven scheduler) ---
; proc_run_all() starts a program through usermode_enter(), which pushes a
; fresh iretq frame whose rip is the entry point. The idle-driven visit cannot
; do that: a process it preempted partway through is not at its entry, it is
; wherever the last switch left it, and the only faithful way back is to reload
; the exact frame that was saved. `rdi` points at a struct syscall_frame (a
; process's stored `ctx`), whose layout is byte-for-byte what the syscall_stub
; epilogue pops and iretq's below — the GPRs in this order, then vector +
; error_code, then the CPU-pushed rip/cs/rflags/rsp/ss. So `mov rsp, rdi` and
; running that same epilogue resumes the process exactly where it stopped.
;
; The kernel side is stashed into the SAME single anchor slots usermode_enter
; uses, and that single slot is what makes this safe. Every resume is issued
; from the one scheduler visit in the idle loop, so every later
; user_exit_to_kernel() lands back on that one anchor frame: the boot demos run
; to completion and get home before the idle loop ever visits, and a visit
; returns before the next one begins. There is never a second, nested visit to
; collide with the slot.
;
; rax must not be clobbered by the ring-3 segment setup — its saved value is the
; very result the process resumes with (0 from sched_yield, a pid from wait) —
; so DS/ES are set to the ring-3 data selector BEFORE rax is popped off the
; frame. iretq loads cs/ss/rsp but never ds/es, so a ring-3 process left
; pointing DS at the kernel's DPL0 data selector would fault on its first
; memory access; setting 0x1B here is what keeps it addressable.
global usermode_resume
usermode_resume:
    mov [rel user_kernel_rsp], rsp
    mov [rel user_kernel_rbp], rbp
    mov [rel user_saved_rbx], rbx
    mov [rel user_saved_r12], r12
    mov [rel user_saved_r13], r13
    mov [rel user_saved_r14], r14
    mov [rel user_saved_r15], r15
    mov ax, 0x1B
    mov ds, ax
    mov es, ax
    mov rsp, rdi
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
    add  rsp, 16
    iretq

; --- int 0x80: the system call gate ---------------------------------
; Same frame shape as the ISR stubs (vector + dummy error code, then the
; GPRs), so struct regs still applies; syscall.c reads it as
; struct syscall_frame, which adds the ss/rsp the CPU pushed on the
; privilege change.
global syscall_stub
syscall_stub:
    push qword 0             ; no error code
    push qword 0x80          ; vector
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
    call syscall_handler     ; may return a value in regs->rax
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

section .data
user_kernel_rsp: dq 0
user_kernel_rbp: dq 0
user_saved_rbx:  dq 0
user_saved_r12:  dq 0
user_saved_r13:  dq 0
user_saved_r14:  dq 0
user_saved_r15:  dq 0

; (the `global` declarations live inside the ISR_*/IRQ_STUB macros)

section .note.GNU-stack noalloc noexec nowrite progbits
