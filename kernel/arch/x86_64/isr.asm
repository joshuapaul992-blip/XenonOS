; SPDX-License-Identifier: MIT
; XenonOS - x86-64 interrupt entry stubs.
;
; Every vector gets a small stub that pushes an error code (0 if the CPU did
; not push one) and the vector number, then jumps to a common routine that
; saves the general registers as a KTRAP_FRAME and calls KiDispatchInterrupt.

bits 64
section .text

extern KiDispatchInterrupt

%macro ISR_NOERR 1
isr_%1:
    push qword 0
    push qword %1
    jmp isr_common
%endmacro

%macro ISR_ERR 1
isr_%1:
    push qword %1
    jmp isr_common
%endmacro

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
    cld
    mov rdi, rsp            ; KTRAP_FRAME *
    call KiDispatchInterrupt
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
    add rsp, 16             ; vector + error code
    iretq

; Vectors where the CPU pushes an error code: 8, 10-14, 17, 21, 29, 30.
%assign i 0
%rep 256
  %if i == 8 || (i >= 10 && i <= 14) || i == 17 || i == 21 || i == 29 || i == 30
    ISR_ERR i
  %else
    ISR_NOERR i
  %endif
  %assign i i+1
%endrep

section .data
global KiIsrTable
KiIsrTable:
%assign i 0
%rep 256
    dq isr_ %+ i
  %assign i i+1
%endrep

section .text
; void KiLoadGdt(void *gdtr, uint16_t code_sel, uint16_t data_sel)
global KiLoadGdt
KiLoadGdt:
    lgdt [rdi]
    mov ax, dx
    mov ds, ax
    mov es, ax
    mov ss, ax
    xor eax, eax
    mov fs, ax
    mov gs, ax
    ; reload CS with a far return
    movzx rsi, si
    push rsi
    lea rax, [rel .reload]
    push rax
    retfq
.reload:
    ret
