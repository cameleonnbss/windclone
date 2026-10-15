/* WindClone kernel linker script — higher-half ELF64.
 *
 * Based on the Limine project's own test kernel linker script
 * (BSD-0), adapted for a higher-half load address.
 *
 * The kernel is linked at 0xffffffff80100000; Limine maps it there and
 * gives us the HHDM for physical memory access. -mcmodel=kernel in the
 * Makefile must stay in sync with this address.
 */
OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(kmain)

KERNEL_VBASE = 0xffffffff80100000;

PHDRS
{
    text    PT_LOAD FLAGS(5);   /* R+X */
    rodata  PT_LOAD FLAGS(4);   /* R   */
    data    PT_LOAD FLAGS(6);   /* R+W */
}

SECTIONS
{
    . = KERNEL_VBASE;

    .text ALIGN(4K) : {
        *(.text .text.*)
    } :text

    .rodata ALIGN(4K) : {
        *(.rodata .rodata.*)
        *(.srodata .srodata.*)
        *(.sdata2 .sdata2.*)
    } :rodata

    .data ALIGN(4K) : {
        *(.data .data.*)
        *(.sdata .sdata.*)

        /* Limine scans these sections for boot requests — do not
         * let the linker garbage-collect them. */
        KEEP(*(.limine_requests_start_marker))
        KEEP(*(.limine_requests))
        KEEP(*(.limine_requests_end_marker))
    } :data

    .bss ALIGN(4K) : {
        *(.bss .bss.*)
        *(.sbss .sbss.*)
        *(COMMON)
    } :data

    /* End the image on a page boundary: guarantees the RW segment's
     * p_memsz is page-exact, so the bootloader maps every .bss page.
     * Without this, big .bss objects (like the 4 KiB IDT) can spill
     * into an unmapped tail page -> #PF -> triple fault during init. */
    . = ALIGN(4K);
    __kernel_end = .;

    /DISCARD/ : {
        *(.eh_frame*)
        *(.note .note.*)
        *(.interp)
        *(.comment)
    }
}
