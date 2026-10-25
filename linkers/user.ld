/* WindClone — userland link script.
 *
 * The address here and USER_BASE in src/kernel/usermode.h are the same
 * number written twice, once in a language the linker reads. It is the one
 * duplicated constant in the build, and it fails loudly if it ever drifts:
 * elf.c rejects any PT_LOAD below USER_BASE, so a stale base produces
 * "elf: segment is below USER_BASE" instead of a program that runs at the
 * wrong address and faults somewhere else entirely.
 *
 * Sections are page-aligned and kept in protection order — text, then
 * read-only data, then writable data, then .bss. That is what makes binutils
 * emit one PT_LOAD per protection domain, which is the whole point of moving
 * off a flat binary: the kernel then knows which bytes may be written.
 *
 * SPDX-License-Identifier: MIT
 */
ENTRY(_start)

SECTIONS
{
    . = 0x400000;

    .text : ALIGN(4096) {
        *(.text.start)
        *(.text)
        *(.text.*)
    }

    .rodata : ALIGN(4096) {
        *(.rodata)
        *(.rodata.*)
    }

    .data : ALIGN(4096) {
        *(.data)
        *(.data.*)
    }

    /* .bss occupies addresses but not file bytes: elf.c zeroes exactly
     * p_memsz - p_filesz of it, which is what makes this section free. */
    .bss : ALIGN(4096) {
        __bss_start = .;
        *(.bss)
        *(.bss.*)
        *(COMMON)
        . = ALIGN(8);
        _end = .;
    }

    /* Not code, not data, not needed to run anything. Discarding them keeps
     * the image inside the loader's page ceiling and the file honest about
     * its own size. Symbol tables are not listed here: the Makefile strips
     * them with -Wl,-s, and a script that discards .shstrtab would leave the
     * section header table pointing at nothing. */
    /DISCARD/ : {
        *(.comment)
        *(.note.gnu.property)
        *(.eh_frame)
        *(.eh_frame_hdr)
        *(.gcc_except_table)
        *(.debug_*)
    }
}
