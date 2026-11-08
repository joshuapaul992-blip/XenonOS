; SPDX-License-Identifier: MIT
; XenonOS - switch to a new stack and continue in a function that never returns.
bits 64
section .text

; void KiSwitchStack(void *stack_top, void (*continuation)(void))
global KiSwitchStack
KiSwitchStack:
    mov rsp, rdi
    xor ebp, ebp            ; end of the frame-pointer chain for stack traces
    call rsi
.hang:
    cli
    hlt
    jmp .hang
