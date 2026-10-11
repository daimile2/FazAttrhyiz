/* Higher-half kernel: VMA in the high half, LMA low in physical RAM.
 * Limine loads by p_paddr (LMA), so LMA MUST be a real physical address.
 * Without AT() the linker defaults LMA=VMA=0xffffffff80000000, which is
 * beyond any real RAM and makes Limine refuse the file. */
KERNEL_VIRT = 0xffffffff80000000;
KERNEL_PHYS = 0x100000;
OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(kmain)
SECTIONS
{
    . = 0xffffffff80000000;
    __kernel_start = .;

    .limine_requests : AT(ADDR(.limine_requests) - KERNEL_VIRT + KERNEL_PHYS) ALIGN(8) {
        KEEP(*(.limine_requests_start))
        KEEP(*(.limine_requests))
        KEEP(*(.limine_requests_end))
    }

    /* __text_start/__text_end sit INSIDE the section, after ALIGN(4096), so
     * they bound the real page-aligned span. Symbols placed before a section
     * header capture the location counter BEFORE alignment and would be off
     * by whatever padding the previous section left behind — which would
     * make paging.c protect (or fail to protect) the wrong pages.
     *
     * The trailing ALIGN(4096) guarantees .text and .rodata never share a
     * page, so clearing W on text cannot accidentally make read-only data
     * writable, or vice versa. */
    .text : AT(ADDR(.text) - KERNEL_VIRT + KERNEL_PHYS) ALIGN(4096) {
        __text_start = .;
        *(.text .text.*)
        . = ALIGN(4096);
        __text_end = .;
    }

    .entry.text : AT(ADDR(.entry.text) - KERNEL_VIRT + KERNEL_PHYS) ALIGN(4096) {
        __entry_text_start = .;
        *(.entry.text)
        . = ALIGN(4096);
        __entry_text_end = .;
    }
    .entry.data : AT(ADDR(.entry.data) - KERNEL_VIRT + KERNEL_PHYS) ALIGN(4096) {
        __entry_data_start = .;
        *(.entry.data)
        . = ALIGN(4096);
        __entry_data_end = .;
    }

    .rodata : AT(ADDR(.rodata) - KERNEL_VIRT + KERNEL_PHYS) ALIGN(4096) {
        *(.rodata .rodata.*)
    }

    .data : AT(ADDR(.data) - KERNEL_VIRT + KERNEL_PHYS) ALIGN(4096) {
        *(.data .data.*)
    }

    __bss_start = .;
    .bss (NOLOAD) : AT(ADDR(.bss) - KERNEL_VIRT + KERNEL_PHYS) ALIGN(4096) {
        *(COMMON)
        *(.bss .bss.*)
    }
    __bss_end = .;
    __kernel_end = .;

    /DISCARD/ : {
        *(.eh_frame*)
        *(.note*)
        *(.comment)
    }
}
