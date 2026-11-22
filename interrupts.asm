; interrupts.asm - CPU exception, hardware IRQ and system-call entry stubs.
;
; Every stub pushes (error code, vector number) and jumps to isr_common, which
; saves all registers and calls isr_handler(struct regs *) in C.
;
; Task switching: isr_handler RETURNS the stack pointer to resume. Normally it
; is the same one we passed in; when the scheduler picks another task it returns
; that task's saved stack instead, so the "iret" at the end continues the other
; task. If the other task was in user mode (ring 3), iret also restores its user
; stack and drops the CPU back to ring 3.
bits 32
extern isr_handler
global isr_stub_table
global isr128

%macro ISR_NOERR 1
isr%1:
    push dword 0
    push dword %1
    jmp isr_common
%endmacro

%macro ISR_ERR 1
isr%1:
    push dword %1
    jmp isr_common
%endmacro

; CPU exceptions 0-31 (8,10-14,17,21 push an error code themselves)
ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_ERR   21
%assign i 22
%rep 10
ISR_NOERR i
%assign i i+1
%endrep
; Hardware IRQs 0-15 remapped to vectors 32-47
%assign i 32
%rep 16
ISR_NOERR i
%assign i i+1
%endrep
; System calls: int 0x80
ISR_NOERR 128

isr_common:
    pusha
    push ds
    push es
    push fs
    push gs
    mov ax, 0x10            ; kernel data segment
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    push esp                ; argument: pointer to struct regs
    call isr_handler
    mov esp, eax            ; resume whichever task the scheduler chose
    pop gs
    pop fs
    pop es
    pop ds
    popa
    add esp, 8              ; drop vector + error code
    iret

section .rodata
isr_stub_table:
%assign i 0
%rep 48
    dd isr %+ i
%assign i i+1
%endrep

section .text
; void gdt_flush(struct gdt_ptr *p) - load a new GDT and reload segment registers
global gdt_flush
gdt_flush:
    mov eax, [esp + 4]
    lgdt [eax]
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    jmp 0x08:.flush
.flush:
    ret

; void tss_flush(void) - tell the CPU where the TSS is (selector 0x28)
global tss_flush
tss_flush:
    mov ax, 0x28
    ltr ax
    ret

global idt_load
idt_load:
    mov eax, [esp + 4]
    lidt [eax]
    ret

section .note.GNU-stack noalloc noexec nowrite progbits
