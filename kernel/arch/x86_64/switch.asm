; SPDX-License-Identifier: MIT
; XenonOS - thread context switch.
bits 64
section .text

extern KiThreadStartup

; void KiSwapContext(uint64_t *old_rsp, uint64_t new_rsp)
; Saves the callee-saved registers on the current stack, stores the stack
; pointer in *old_rsp, switches to new_rsp and restores the registers saved
; there. Returns in the context of the other thread.
global KiSwapContext
KiSwapContext:
    push rbp
    push rbx
    push r12
    push r13
    push r14
    push r15
    mov [rdi], rsp
    mov rsp, rsi
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret

; First code a new thread runs: KiSwapContext "returns" here.
global KiThreadStartupStub
KiThreadStartupStub:
    xor ebp, ebp
    and rsp, -16
    call KiThreadStartup
.hang:
    cli
    hlt
    jmp .hang
