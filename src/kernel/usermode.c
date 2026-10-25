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
extern void usermode_enter(uint64_t entry, uint64_t user_rsp);
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

int usermode_run(const char *path, int *exit_code)
{
    struct elf_report rep;

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
    serial_write(" zeroed, entering ring 3\n");

    exit_code_value = -1;

    /* 16-byte aligned top, minus one word so a `call` in user code pushes
     * into mapped memory instead of the guard-less hole above it. */
    uint64_t rsp = USER_STACK_TOP - 16;

    usermode_enter(rep.entry, rsp);

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
