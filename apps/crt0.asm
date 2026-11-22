; crt0.asm - program start-up: call main(), then exit with its return value.
bits 32
global _start
extern main
section .text
_start:
    call main
    mov ebx, eax        ; exit code
    mov eax, 0          ; SYS_EXIT
    int 0x80
.hang:
    jmp .hang
section .note.GNU-stack noalloc noexec nowrite progbits
