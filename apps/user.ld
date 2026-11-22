/* Programs are loaded at 1 GB, inside their own private address space. */
ENTRY(_start)
SECTIONS
{
    . = 0x40000000;
    .text   : { *(.text*) }
    .rodata : { *(.rodata*) }
    .data   : { *(.data*) }
    .bss    : { *(COMMON) *(.bss*) }
}
