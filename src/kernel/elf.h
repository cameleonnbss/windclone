/* WindClone — ELF64 process loader.
 *
 * This is what turns "we can jump to bytes in RAM" into "we can run an
 * executable": the file says where its pieces belong, how big they are once
 * in memory, and what may be done with them, and the loader honours all
 * three. Three things the flat-binary path could not do:
 *
 *   - separate segments. .text and .data arrive as two PT_LOAD entries at
 *     two addresses, with two sizes — p_filesz bytes copied, p_memsz -
 *     p_filesz zeroed. That is .bss, and it is why a program's globals start
 *     at zero without anyone shipping a megabyte of zeros in the file.
 *   - real protection. A segment without PF_W is mapped write-protected
 *     *after* it has been filled, and left that way. A program that writes to
 *     its own code faults instead of succeeding, which is the difference
 *     between a loader and a copier.
 *   - a size that is not a lie. The old path capped the image at 64 KiB
 *     because that was the loop it had. Here the cap is on pages mapped and
 *     segments read, and a bigger program just needs a bigger cap.
 *
 * Validation is the interesting part. Every field below came from a file a
 * human may have hand-edited, so each check is written as "what breaks
 * without it", not as ceremony:
 *   - class/data/type/machine, because a 32-bit or ARM or .o image has no
 *     business reaching the mapping loop;
 *   - e_phentsize == 56, because the walk steps by the *file's* idea of the
 *     header size, and a mismatch reads garbage as the second header;
 *   - (p_vaddr - p_offset) % 4096 == 0, because that congruence is what makes
 *     "copy from this file offset to that address, page by page" legal.
 *     Without it a page boundary lands in the middle of the file mapping and
 *     the segment gets split in the wrong place;
 *   - p_memsz >= p_filesz and the file range is inside the archive, because
 *     otherwise the copy runs off the end of the initrd;
 *   - the virtual range is inside the user window, because the kernel shares
 *     one address space with the program and a segment at 0xffffffff80...
 *     would have the program's bytes overwrite kernel memory;
 *   - e_entry lands in a mapped PT_LOAD, because an entry point outside the
 *     image is an immediate fetch fault, and a loader that discovers that
 *     after iretq reports a CPU bug instead of a bad file.
 *
 * Not handled, and not pretended otherwise: ET_DYN (PIE) images need a
 * load-time base and relocations, section headers are never read, and the
 * page tables have no NX — so "read-only" here means "not writable", and the
 * stack stays executable-by-absence-of-NX until the paging layer grows the
 * bit. Phase 7's Linux semantics start from this file.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef WINDCLONE_ELF_H
#define WINDCLONE_ELF_H

#include <stdint.h>

/* --- the parts of the format we read --------------------------------- */
#define ELF64IDENT   64        /* e_ident[] + the rest of the fixed header */
#define ELF_MAGIC0   0x7f
#define ELF_CLASS64  2
#define ELF_DATA_LSB 1
#define ET_EXEC      2
#define ET_DYN       3
#define EM_X86_64    0x3e

#define PT_LOAD      1
#define PF_X         1
#define PF_W         2
#define PF_R         4

struct elf64_ehdr {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
};

struct elf64_phdr {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
};

_Static_assert(sizeof(struct elf64_ehdr) == 64, "ELF64 header is 64 bytes");
_Static_assert(sizeof(struct elf64_phdr) == 56, "ELF64 program header is 56 bytes");
_Static_assert(__builtin_offsetof(struct elf64_ehdr, e_entry) == 24, "e_entry at 24");
_Static_assert(__builtin_offsetof(struct elf64_ehdr, e_phnum) == 56, "e_phnum at 56");

/* Ceilings a hand-edited file cannot exceed without being rejected. */
#define ELF_MAX_SEGMENTS 16
#define ELF_MAX_PAGES    64      /* 256 KiB of image per program, for now */

struct elf_report {
    uint64_t entry;        /* e_entry, verified to be inside the image */
    uint64_t segments;     /* PT_LOAD count actually mapped */
    uint64_t pages;        /* frames claimed */
    uint64_t bytes_loaded; /* file bytes copied */
    uint64_t bytes_zeroed; /* the .bss tail */
    uint64_t low, high;    /* virtual extent, page-aligned */
    uint64_t ro_pages;     /* mapped without PTE_WRITE */
};

/* Map and fill every PT_LOAD of the open file `fd`, then apply final
 * protections. Returns 0 and fills *rep, or -1 after saying why on the
 * serial console — including "this is not an ELF64 executable", which is why
 * there is no separate probe step: the loader is the check. Nothing is
 * entered here; the caller decides when to iretq. */
int elf_load(int fd, struct elf_report *rep);

#endif
