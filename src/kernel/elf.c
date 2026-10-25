/* WindClone — ELF64 loader implementation. See elf.h for the rules.
 *
 * Two passes, because protection has to be decided after the whole image is
 * in: fill everything with the page writable (the kernel must be able to copy
 * into it), then lower the flags the file asked for. Doing it in one pass
 * would map .text read-only and then fault on the very memcpy that fills it.
 *
 * A page is a window into the file, not into the segment: p_vaddr and
 * p_offset are congruent modulo 4096, so the page that holds the start of a
 * segment begins *before* it, and those leading bytes are in the file too.
 * Copying the whole page is what Linux's mmap-based loader does; skipping the
 * pad would put the segment at the wrong distance from its own page start.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include <stddef.h>

#include "elf.h"
#include "usermode.h"
#include "vfs.h"
#include "paging.h"
#include "pmm.h"
#include "serial.h"
#include "string.h"

#define PAGE_SIZE 4096ULL
#define PAGE_MASK (~0xFFFULL)

/* e_ident indices, spelled out because <elf.h> is a hosted header. */
#define EI_CLASS  4
#define EI_DATA   5
#define EI_VERSION 6
#define EI_OSABI   7

/* One mapped page, as pass 2 needs to remember it. */
struct mapped_page {
    uint64_t va;
    uint64_t phys;
    int      writable;
};

static struct mapped_page pages[ELF_MAX_PAGES];
static unsigned page_count;

/* Put the previous program's image back. There is one address space and one
 * list of pages, so "start a second program" means "the first one's frames
 * come back first": without this, the second image would be copied into pages
 * the first left read-only — a kernel write into a read-only leaf, which
 * faults inside the kernel rather than returning an error. Freeing them is
 * also what stops running ten programs from costing ten images.
 *
 * A failed load leaves its partial pages here too, which is the point: they
 * come back on the next attempt instead of leaking. */
static void release_previous(void)
{
    for (unsigned i = 0; i < page_count; i++) {
        paging_unmap_page(pages[i].va);
        if (pages[i].phys != 0)
            pmm_free_frame(pages[i].phys);
    }
    page_count = 0;
}

static void reject(const char *why, uint64_t detail)
{
    serial_write("elf: ");
    serial_write(why);
    if (detail) {
        serial_write(" (");
        serial_write_hex(detail);
        serial_write(")");
    }
    serial_write("\n");
}

static int remember_page(uint64_t va, uint64_t phys, int writable)
{
    for (unsigned i = 0; i < page_count; i++) {
        if (pages[i].va == va) {
            /* Shared boundary page: if either segment wants it writable, it
             * stays writable. A leaf cannot be two things at once, and
             * refusing to load the program is worse than one permissive bit
             * on one page — which is also what Linux's mprotect-on-the-last
             * mapping of a shared page ends up doing. */
            if (writable)
                pages[i].writable = 1;
            return 0;
        }
    }
    if (page_count == ELF_MAX_PAGES)
        return -1;
    pages[page_count].va = va;
    pages[page_count].phys = phys;
    pages[page_count].writable = writable;
    page_count++;
    return 0;
}

/* Byte span of the segment measured from its first page, in memory. */
static uint64_t seg_first_page(const struct elf64_phdr *ph)
{
    return ph->p_vaddr & PAGE_MASK;
}

static int check_header(const struct elf64_ehdr *eh, uint64_t file_size)
{
    static const char want[] = { ELF_MAGIC0, 'E', 'L', 'F' };

    for (int i = 0; i < 4; i++)
        if (eh->e_ident[i] != (uint8_t)want[i]) {
            reject("not an ELF image: bad magic", 0);
            return -1;
        }
    if (eh->e_ident[EI_CLASS] != ELF_CLASS64) {
        reject("only ELFCLASS64 is loadable", eh->e_ident[EI_CLASS]);
        return -1;
    }
    if (eh->e_ident[EI_DATA] != ELF_DATA_LSB) {
        reject("only little-endian images are loadable", eh->e_ident[EI_DATA]);
        return -1;
    }
    if (eh->e_type != ET_EXEC) {
        /* ET_DYN would need a load base plus relocation processing; a .o or a
         * core file has no entry contract at all. Say which, because "elf
         * rejected" with no reason is how a week disappears. */
        reject("image is not ET_EXEC (PIE/ET_DYN needs relocations)", eh->e_type);
        return -1;
    }
    if (eh->e_machine != EM_X86_64) {
        reject("image is not for x86-64", eh->e_machine);
        return -1;
    }
    if (eh->e_phentsize != sizeof(struct elf64_phdr)) {
        reject("program header entry size is not 56", eh->e_phentsize);
        return -1;
    }
    if (eh->e_phnum == 0 || eh->e_phnum > ELF_MAX_SEGMENTS) {
        reject("segment count out of range", eh->e_phnum);
        return -1;
    }

    /* The table has to be inside the file. Both products are checked for
     * wraparound, because a 64-bit e_phoff is exactly the field an edited
     * hex dump gets wrong. */
    uint64_t table_bytes = (uint64_t)eh->e_phnum * eh->e_phentsize;
    if (eh->e_phoff > file_size || table_bytes > file_size - eh->e_phoff) {
        reject("program header table runs past the file", eh->e_phoff);
        return -1;
    }
    return 0;
}

/* Validate one PT_LOAD before any mapping happens. */
static int check_segment(const struct elf64_phdr *ph, uint64_t file_size)
{
    if (ph->p_memsz < ph->p_filesz) {
        reject("p_memsz is smaller than p_filesz", ph->p_memsz);
        return -1;
    }

    /* The congruence rule, and the reason the leading pad can be read from
     * the file at all. */
    if (ph->p_align > 1 && (ph->p_align & (ph->p_align - 1)) != 0) {
        reject("p_align is not a power of two", ph->p_align);
        return -1;
    }
    uint64_t intra = ph->p_vaddr & ~PAGE_MASK;
    if (ph->p_align >= PAGE_SIZE &&
        ((ph->p_vaddr - ph->p_offset) & (ph->p_align - 1)) != 0) {
        reject("p_vaddr and p_offset disagree modulo p_align", ph->p_vaddr);
        return -1;
    }
    if (ph->p_offset < intra) {
        reject("segment starts before the file's page window", ph->p_offset);
        return -1;
    }

    uint64_t file_page = ph->p_offset - intra;
    uint64_t need = intra + ph->p_filesz;
    if (need > file_size - file_page) {
        reject("segment data is not entirely in the file", ph->p_offset);
        return -1;
    }

    /* The fence. A segment outside the user window would land on the kernel,
     * the stack we are about to build, or the NULL trap page. */
    if (ph->p_memsz == 0) {
        reject("PT_LOAD with an empty memory range", ph->p_vaddr);
        return -1;
    }
    if (ph->p_vaddr < USER_BASE) {
        reject("segment is below USER_BASE", ph->p_vaddr);
        return -1;
    }
    uint64_t last = ph->p_vaddr + ph->p_memsz - 1;
    if (last < ph->p_vaddr || last >= USER_ADDR_LIMIT) {
        reject("segment reaches outside the user window", ph->p_vaddr);
        return -1;
    }
    if (ph->p_vaddr < USER_STACK_TOP && last >= USER_STACK_BOTTOM) {
        reject("segment collides with the user stack", ph->p_vaddr);
        return -1;
    }
    return 0;
}

/* Copy the file bytes of one segment and zero its .bss tail. Fresh pages are
 * allocated and cleared; a page another segment already mapped is reused so
 * the shared boundary keeps the bytes both halves need. */
static int fill_segment(int fd, const struct elf64_phdr *ph,
                        uint64_t *bytes_loaded, uint64_t *bytes_zeroed)
{
    uint64_t intra     = ph->p_vaddr & ~PAGE_MASK;
    uint64_t seg_page  = seg_first_page(ph);
    uint64_t file_page = ph->p_offset - intra;
    uint64_t mem_end   = intra + ph->p_memsz;
    uint64_t data_end  = intra + ph->p_filesz;
    uint64_t count     = (mem_end + PAGE_SIZE - 1) / PAGE_SIZE;

    for (uint64_t i = 0; i < count; i++) {
        uint64_t va  = seg_page + i * PAGE_SIZE;
        uint64_t from = i * PAGE_SIZE;             /* page start, in segment terms */

        uint64_t phys = paging_translate(va);
        if (phys == 0) {
            phys = pmm_alloc_frame();
            if (phys == 0) {
                reject("out of physical frames while mapping the image", va);
                return -1;
            }
            paging_map_page(va, phys, PTE_WRITE | PTE_USER);
            memset((void *)(uintptr_t)va, 0, (size_t)PAGE_SIZE);
        }
        if (remember_page(va, phys, (ph->p_flags & PF_W) != 0) != 0) {
            reject("image needs more pages than the loader allows", va);
            return -1;
        }

        /* File bytes inside this page: [from, min(data_end, page end)). */
        if (from < data_end) {
            uint64_t n = data_end - from;
            if (n > PAGE_SIZE)
                n = PAGE_SIZE;
            if (vfs_pread(fd, (void *)(uintptr_t)va, n, file_page + from)
                    != (int64_t)n) {
                reject("short read on segment data", va);
                return -1;
            }
            *bytes_loaded += n;
        }

        /* The .bss tail of this page, if the segment has one. Explicit even
         * though a fresh page is already zero: on a shared page it is not. */
        if (data_end < mem_end) {
            uint64_t zstart = data_end > from ? data_end : from;
            uint64_t zend = mem_end < from + PAGE_SIZE ? mem_end : from + PAGE_SIZE;
            if (zstart < zend) {
                memset((void *)(uintptr_t)(va + (zstart - from)), 0,
                       (size_t)(zend - zstart));
                *bytes_zeroed += zend - zstart;
            }
        }
    }
    return 0;
}

static int entry_is_mapped(uint64_t entry)
{
    /* The entry must fall in a page we mapped — otherwise iretq takes a fetch
     * fault and the report says "general protection fault" where the truth is
     * "bad file". */
    for (unsigned i = 0; i < page_count; i++)
        if (pages[i].va == (entry & PAGE_MASK))
            return 1;
    return 0;
}

int elf_load(int fd, struct elf_report *rep)
{
    struct elf64_phdr segs[ELF_MAX_SEGMENTS];
    unsigned seg_count = 0;
    struct elf64_ehdr eh;

    memset(rep, 0, sizeof *rep);
    release_previous();

    int64_t size = vfs_size(fd);
    if (size < 0)
        return -1;
    if ((uint64_t)size < sizeof(eh)) {
        reject("file is smaller than an ELF64 header", (uint64_t)size);
        return -1;
    }
    if (vfs_pread(fd, &eh, sizeof eh, 0) != (int64_t)sizeof eh) {
        reject("cannot read the ELF header", 0);
        return -1;
    }
    if (check_header(&eh, (uint64_t)size) != 0)
        return -1;

    /* Read the program headers one at a time: 56 bytes each, and the table
     * never needs to live in the kernel at the same time as the image. */
    for (unsigned i = 0; i < eh.e_phnum; i++) {
        struct elf64_phdr ph;
        uint64_t at = eh.e_phoff + (uint64_t)i * eh.e_phentsize;
        if (vfs_pread(fd, &ph, sizeof ph, at) != (int64_t)sizeof ph) {
            reject("cannot read program header", i);
            return -1;
        }
        if (ph.p_type != PT_LOAD)
            continue;
        if (check_segment(&ph, (uint64_t)size) != 0)
            return -1;
        segs[seg_count++] = ph;
    }

    if (seg_count == 0) {
        reject("image has no PT_LOAD segment", 0);
        return -1;
    }

    for (unsigned i = 0; i < seg_count; i++) {
        if (fill_segment(fd, &segs[i], &rep->bytes_loaded, &rep->bytes_zeroed) != 0)
            return -1;
    }

    if (!entry_is_mapped(eh.e_entry)) {
        reject("e_entry is outside every mapped segment", eh.e_entry);
        return -1;
    }

    /* Pass 2: lower the flags the file asked for. Nothing writes to these
     * pages again, so the read-only code pages stay that way for the run. */
    for (unsigned i = 0; i < page_count; i++) {
        uint64_t flags = PTE_USER | (pages[i].writable ? PTE_WRITE : 0);
        uint64_t phys = paging_translate(pages[i].va);
        if (phys == 0) {
            reject("page vanished between mapping and protection", pages[i].va);
            return -1;
        }
        paging_map_page(pages[i].va, phys, flags);
        if (!pages[i].writable)
            rep->ro_pages++;
    }

    rep->entry    = eh.e_entry;
    rep->segments = seg_count;
    rep->pages    = page_count;

    /* pages[] is filled in segment order, and a file may list its segments in
     * any order at all, so the extent is a min/max, not first/last. */
    rep->low  = pages[0].va;
    rep->high = pages[0].va + PAGE_SIZE;
    for (unsigned i = 1; i < page_count; i++) {
        if (pages[i].va < rep->low)
            rep->low = pages[i].va;
        if (pages[i].va + PAGE_SIZE > rep->high)
            rep->high = pages[i].va + PAGE_SIZE;
    }
    return 0;
}
