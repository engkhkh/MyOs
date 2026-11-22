; boot.asm - Multiboot entry point. GRUB/QEMU loads us, we set up a stack and jump to C.
MBALIGN  equ 1 << 0
MEMINFO  equ 1 << 1
%ifdef GRUB_VIDEO
VIDEO    equ 1 << 2          ; ask the bootloader (GRUB) for a graphics mode
%else
VIDEO    equ 0               ; QEMU's -kernel loader rejects the video request
%endif
FLAGS    equ MBALIGN | MEMINFO | VIDEO
MAGIC    equ 0x1BADB002
CHECKSUM equ -(MAGIC + FLAGS)

section .multiboot
align 4
    dd MAGIC
    dd FLAGS
    dd CHECKSUM
    dd 0, 0, 0, 0, 0    ; load addresses (unused: we are an ELF file)
    dd 0                ; mode type: linear graphics
    dd 1024, 768, 32    ; preferred width, height, colour depth

section .bss
align 16
stack_bottom:
    resb 16384
stack_top:

section .text
global _start
extern kernel_main
_start:
    mov esp, stack_top
    push ebx            ; multiboot info pointer
    push eax            ; multiboot magic
    call kernel_main
    cli
.hang:
    hlt
    jmp .hang
section .note.GNU-stack noalloc noexec nowrite progbits
