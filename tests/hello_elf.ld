OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)
PHDRS {
    text PT_LOAD FLAGS(5);
    data PT_LOAD FLAGS(6);
}
SECTIONS {
    . = 0x400000;
    .text : { *(.text) } :text
    . = 0x401000;
    .data : { *(.data) } :data
    .bss (NOLOAD) : { *(.bss) } :data
    /DISCARD/ : { *(.note*) *(.comment) *(.eh_frame*) }
}
