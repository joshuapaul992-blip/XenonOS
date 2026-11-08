/* XenonOS kernel linker script: higher-half kernel loaded by Limine. */
OUTPUT_FORMAT(elf64-x86-64)
ENTRY(KiSystemStartup)

PHDRS
{
    limine_requests PT_LOAD;
    text    PT_LOAD;
    rodata  PT_LOAD;
    data    PT_LOAD;
}

SECTIONS
{
    . = 0xffffffff80000000;
    __kernel_start = .;

    .limine_requests : {
        KEEP(*(.limine_requests_start))
        KEEP(*(.limine_requests))
        KEEP(*(.limine_requests_end))
    } :limine_requests

    . = ALIGN(CONSTANT(MAXPAGESIZE));
    .text : {
        __text_start = .;
        *(.text .text.*)
        __text_end = .;
    } :text

    . = ALIGN(CONSTANT(MAXPAGESIZE));
    .rodata : {
        __rodata_start = .;
        *(.rodata .rodata.*)
        __rodata_end = .;
    } :rodata

    . = ALIGN(CONSTANT(MAXPAGESIZE));
    .data : {
        __data_start = .;
        *(.data .data.*)
    } :data
    .bss : {
        *(.bss .bss.*)
        *(COMMON)
    } :data

    __kernel_end = .;

    /DISCARD/ : { *(.eh_frame*) *(.note .note.*) *(.comment) }
}
