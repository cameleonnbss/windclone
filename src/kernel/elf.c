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

/* The shell path keeps its image frames in one static list and reclaims them
 * itself, because there is a single address space and only one program can be
 * resident at a time. A process gets its own list through elf_load_pages(). */
static struct elf_page shell_pages[ELF_MAX_PAGES];
static unsigned shell_page_count;

/* Put the previous shell program's image back. There is one address space and
 * one list of pages, so "start a second program" means "the first one's frames
 * come back first": without this, the second image would be copied into pages
 * the first left read-only — a kernel write into a read-only leaf, which
 * faults inside the kernel rather than returning an error. Freeing them is
 * also what stops running ten programs from costing ten images.
 *
 * A failed load leaves its partial pages here too, which is the point: they
 * come back on the next attempt instead of leaking. */
static void release_shell_pages(void)
{
    for (unsigned i = 0; i < shell_page_count; i++) {
        paging_unmap_page(shell_pages[i].va);
        if (shell_pages[i].phys != 0)
            pmm_free_frame(shell_pages[i].phys);
    }
    shell_page_count = 0;
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

static int remember_page(struct elf_page *pages, unsigned *count, unsigned cap,
                         uint64_t va, uint64_t phys, int writable)
{
    for (unsigned i = 0; i < *count; i++) {
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
    if (*count == cap)
        return -1;
    pages[*count].va = va;
    pages[*count].phys = phys;
    pages[*count].writable = writable;
    (*count)++;
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
 * the shared boundary keeps the bytes both halves need.
 *
 * The copy goes through the higher-half direct map, not the page's user
 * virtual address: a process is built under its own table while the kernel
 * still runs on the boot CR3, so `va` may be absent from the live tables even
 * though `phys` is real. HHDM(phys) is always reachable and is the same RAM
 * the program will read at `va` once its own CR3 is loaded. */
static int fill_segment(int fd, const struct elf64_phdr *ph,
                        uint64_t *bytes_loaded, uint64_t *bytes_zeroed,
                        struct elf_page *pages, unsigned *count, unsigned cap)
{
    uint64_t intra     = ph->p_vaddr & ~PAGE_MASK;
    uint64_t seg_page  = seg_first_page(ph);
    uint64_t file_page = ph->p_offset - intra;
    uint64_t mem_end   = intra + ph->p_memsz;
    uint64_t data_end  = intra + ph->p_filesz;
    uint64_t npages     = (mem_end + PAGE_SIZE - 1) / PAGE_SIZE;

    for (uint64_t i = 0; i < npages; i++) {
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
            memset(pmm_phys_to_virt(phys), 0, (size_t)PAGE_SIZE);
        }
        uint8_t *kv = pmm_phys_to_virt(phys);
        if (remember_page(pages, count, cap, va, phys,
                          (ph->p_flags & PF_W) != 0) != 0) {
            reject("image needs more pages than the loader allows", va);
            return -1;
        }

        /* File bytes inside this page: [from, min(data_end, page end)). */
        if (from < data_end) {
            uint64_t n = data_end - from;
            if (n > PAGE_SIZE)
                n = PAGE_SIZE;
            if (vfs_pread(fd, kv + from, n, file_page + from) != (int64_t)n) {
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
                memset(kv + (zstart - from), 0, (size_t)(zend - zstart));
                *bytes_zeroed += zend - zstart;
            }
        }
    }
    return 0;
}

static int entry_is_mapped(const struct elf_page *pages, unsigned count,
                           uint64_t entry)
{
    /* The entry must fall in a page we mapped — otherwise iretq takes a fetch
     * fault and the report says "general protection fault" where the truth is
     * "bad file". */
    for (unsigned i = 0; i < count; i++)
        if (pages[i].va == (entry & PAGE_MASK))
            return 1;
    return 0;
}

/* The shared body. `pages`/`count`/`cap` is the caller's leaf list, and the
 * image lands in whatever table paging_set_working() last selected — the boot
 * table for the shell, a freshly cloned one for a process. */
static int elf_load_core(int fd, struct elf_report *rep,
                         struct elf_page *pages, unsigned *count, unsigned cap)
{
    struct elf64_phdr segs[ELF_MAX_SEGMENTS];
    unsigned seg_count = 0;
    struct elf64_ehdr eh;

    memset(rep, 0, sizeof *rep);
    *count = 0;

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
        /* ld lists an empty .bss as a PT_LOAD that asks for no memory and
         * carries no bytes — the first program with nothing writable
         * (/bin/echo) is what found it. There is no page to map and no byte
         * to copy, so it is skipped rather than loaded; refusing it would
         * have refused a perfectly good image. A segment that HAS file bytes
         * but no room for them is still the error it looks like, and
         * p_memsz < p_filesz above is exactly that check. */
        if (ph.p_memsz == 0 && ph.p_filesz == 0)
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
        if (fill_segment(fd, &segs[i], &rep->bytes_loaded, &rep->bytes_zeroed,
                         pages, count, cap) != 0)
            return -1;
    }

    if (!entry_is_mapped(pages, *count, eh.e_entry)) {
        reject("e_entry is outside every mapped segment", eh.e_entry);
        return -1;
    }

    /* Pass 2: lower the flags the file asked for. Nothing writes to these
     * pages again, so the read-only code pages stay that way for the run. */
    for (unsigned i = 0; i < *count; i++) {
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
    rep->pages    = *count;

    /* pages[] is filled in segment order, and a file may list its segments in
     * any order at all, so the extent is a min/max, not first/last. */
    rep->low  = pages[0].va;
    rep->high = pages[0].va + PAGE_SIZE;
    for (unsigned i = 1; i < *count; i++) {
        if (pages[i].va < rep->low)
            rep->low = pages[i].va;
        if (pages[i].va + PAGE_SIZE > rep->high)
            rep->high = pages[i].va + PAGE_SIZE;
    }
    return 0;
}

int elf_load(int fd, struct elf_report *rep)
{
    /* The shell has one address space: before a new image is copied in, the
     * previous one's read-only frames must come back, or the copy would be a
     * kernel write into a page the last program left unwritable. */
    release_shell_pages();
    return elf_load_core(fd, rep, shell_pages, &shell_page_count, ELF_MAX_PAGES);
}

int elf_load_pages(int fd, struct elf_report *rep,
                   struct elf_page *out, unsigned cap, unsigned *out_count)
{
    /* A process owns its frames: nothing here frees another image, because
     * each process has its own address space and its own leaf list. */
    return elf_load_core(fd, rep, out, out_count, cap);
}
