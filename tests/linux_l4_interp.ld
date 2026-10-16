OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)
PHDRS { text PT_LOAD FILEHDR PHDRS FLAGS(5); }
SECTIONS {
 . = SIZEOF_HEADERS;
 .text : { *(.text*) *(.rodata*) } :text
 /DISCARD/ : { *(.note*) *(.comment) *(.eh_frame*) }
}
