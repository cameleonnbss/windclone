/* WindClone — load a program and run it in ring 3.
 *
 * The flat-binary path that used to live here is gone, and that is the
 * point: a flat blob proves the privilege boundary, but it cannot express
 * "these bytes are code and must not be written", "these bytes start at zero
 * and are not in the file", or "this is where the program begins". An ELF64
 * can, so the loader in elf.c does the loading and this file owns only the
 * address space around it — the stack, the fence for user pointers, and the
 * handover into ring 3.
 *
 * Two rules hold everything else together:
 *   - Every page a program touches must carry PTE_USER on the leaf *and* on
 *     every table above it. Miss an intermediate level and the CPU faults at
 *     CPL 3 although the leaf looks perfect (paging.c propagates the bit).
 *   - One CR3 is shared by kernel and program, so the user window in
 *     usermode.h is not a convenience — it is the only thing separating a
 *     hand-edited program header from the kernel's own text.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include <stddef.h>

#include "usermode.h"
#include "vfs.h"
#include "elf.h"
#include "paging.h"
#include "pmm.h"
#include "serial.h"
#include "string.h"

/* Not noreturn despite the iretq: SYS_EXIT unwinds the stack back to the
 * instruction after the call, so this really does return — once. */
extern void usermode_enter(uint64_t entry, uint64_t user_rsp,
                           uint64_t argc, uint64_t argv);
extern void user_exit_to_kernel(void);

static int exit_code_value;

/* Is [addr, addr+len) inside the region a program may own? Overflow-safe,
 * because `len` is user-controlled and addr+len wrapping to a small number is
 * exactly how a bounds check gets fooled. */
int user_range_ok(uint64_t addr, uint64_t len)
{
    if (addr < USER_BASE || addr >= USER_ADDR_LIMIT)
        return 0;
    if (len > USER_ADDR_LIMIT - addr)
        return 0;
    return 1;
}

/* One stack region, mapped with the USER bit so iretq can land on it. The
 * pages are writable and never read-only: a program that could not push would
 * fault on its first call.
 *
 * The frames are reused if the region is already mapped, because this runs
 * once per program and there is nothing to free them yet — a fresh alloc each
 * run would leak four frames per command typed at the shell, quietly forever.
 * Per-process address spaces are Phase 5's answer; this is the honest stopgap. */
static int map_stack(void)
{
    uint64_t pages = USER_STACK_SIZE / 4096;
    for (uint64_t i = 0; i < pages; i++) {
        uint64_t va = USER_STACK_TOP - (i + 1) * 4096;
        uint64_t phys = paging_translate(va);
        if (phys == 0) {
            phys = pmm_alloc_frame();
            if (phys == 0) {
                serial_write("user: out of frames while building the stack\n");
                return -1;
            }
        }
        paging_map_page(va, phys, PTE_WRITE | PTE_USER);
    }
    return 0;
}

/* Lay the command line out on the program's own stack, then hand back the
 * address _start is entered with in rsp. The shape is the one the AMD64 psABI
 * describes, because that is the shape a Linux binary reads (see wcabi.h):
 *
 *   (%rsp)   argc
 *            argv[0] .. argv[argc-1], NULL
 *            NULL                       (envp: no environment yet)
 *            0, 0                       (auxv terminated with AT_NULL)
 *   above    the argument strings themselves
 *
 * Strings go in first, top-down, and the word table lands below them - that
 * order is what makes the bounds exact: everything has to stay above
 * USER_STACK_BOTTOM, which is where the loaded image already is. */
static int build_stack(int argc, const char *const *argv, uint64_t *rsp_out)
{
    uint64_t ptrs[USER_ARG_MAX];
    uint64_t words = 1 + (uint64_t)argc + 1 + 1 + 2;
    uint64_t floor = USER_STACK_BOTTOM + 16;   /* a hole, never a landing spot */
    uint64_t sp = USER_STACK_TOP - 16;

    for (int i = argc - 1; i >= 0; i--) {
        uint64_t l = strlen(argv[i]) + 1;
        if (l > USER_ARGV_BYTES || sp - l < floor + words * 8)
            return -1;
        sp -= l;
        memcpy((void *)(uintptr_t)sp, argv[i], (size_t)l);
        ptrs[i] = sp;
    }

    /* The table needs 8-aligned addresses; rounding the string area down first
     * costs a few bytes and buys that, and the 16-alignment the ABI asks for
     * at entry is taken after the table is placed. */
    sp &= ~7ULL;
    if (sp < floor + words * 8)
        return -1;
    sp -= words * 8;
    sp &= ~0xFULL;
    if (sp < floor)
        return -1;

    uint64_t *p = (uint64_t *)(uintptr_t)sp;
    *p++ = (uint64_t)argc;
    for (int i = 0; i < argc; i++)
        *p++ = ptrs[i];
    *p++ = 0;                 /* argv[argc] */
    *p++ = 0;                 /* envp[0] */
    *p++ = 0;                 /* auxv: AT_NULL */
    *p++ = 0;

    *rsp_out = sp;
    return 0;
}

int usermode_run(const char *path, int *exit_code, int argc,
                 const char *const *argv)
{
    struct elf_report rep;
    const char *self[1];

    /* No command line means "you, called by your own name" - what exec(2)
     * would have put in argv[0] anyway, and what lets the boot self-check run
     * a program without pretending to have arguments. */
    if (argv == NULL || argc <= 0) {
        self[0] = path;
        argv = self;
        argc = 1;
    }
    if (argc > USER_ARG_MAX) {
        serial_write("user: too many arguments\n");
        return -1;
    }

    /* The program is opened like any other file, through the VFS. That is not
     * ceremony: it means the loader has one way to read bytes and no idea what
     * backs them, so a disk driver can replace the initrd later without
     * touching a line of either. */
    int64_t fd = vfs_open(path);
    if (fd < 0) {
        serial_write("user: cannot open ");
        serial_write(path);
        serial_write("\n");
        return -1;
    }

    if (elf_load((int)fd, &rep) != 0) {
        vfs_close((int)fd);
        serial_write("user: refused ");
        serial_write(path);
        serial_write("\n");
        return -1;
    }

    /* The stack is mapped after the image, so a segment that reached the
     * stack window would already have been refused by the loader rather than
     * being silently overwritten here. */
    if (map_stack() != 0) {
        vfs_close((int)fd);
        return -1;
    }

    uint64_t entry_rsp = USER_STACK_TOP - 16;
    if (build_stack(argc, argv, &entry_rsp) != 0) {
        vfs_close((int)fd);
        serial_write("user: command line does not fit on the stack\n");
        return -1;
    }

    serial_write("user: ");
    serial_write(path);
    serial_write(" elf64, entry ");
    serial_write_hex(rep.entry);
    serial_write(", ");
    serial_write_dec(rep.segments);
    serial_write(" segments, ");
    serial_write_dec(rep.pages);
    serial_write(" pages (");
    serial_write_dec(rep.ro_pages);
    serial_write(" read-only), ");
    serial_write_dec(rep.bytes_loaded);
    serial_write(" bytes loaded, ");
    serial_write_dec(rep.bytes_zeroed);
    serial_write(" zeroed, argv ");
    serial_write_dec((uint64_t)argc);
    serial_write(", entering ring 3\n");

    exit_code_value = -1;

    usermode_enter(rep.entry, entry_rsp, (uint64_t)argc,
                   (uint64_t)(uintptr_t)entry_rsp + 8);

    /* Reached only through SYS_EXIT -> user_exit_to_kernel(). */
    vfs_close((int)fd);
    if (exit_code != NULL)
        *exit_code = exit_code_value;
    return 0;
}

void usermode_exit(int code)
{
    exit_code_value = code;

    serial_write("user: exit(");
    serial_write_dec((uint64_t)(code < 0 ? 0u : (unsigned)code));
    serial_write(") - back to ring 0\n");

    user_exit_to_kernel();   /* unwinds to the usermode_enter() caller */
    for (;;)
        __asm__ volatile("cli; hlt");
}
