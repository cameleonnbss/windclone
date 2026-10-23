/* WindClone — load a flat program from the initrd and run it in ring 3.
 *
 * Why a flat binary before an ELF: the interesting failure modes here are
 * about *privilege*, not file formats. Keeping the image a single blob
 * (nasm -f bin, org 0x400000) means that when the machine faults, the
 * cause is the GDT/TSS/page tables, not a misparsed program header. The
 * ELF64 loader drops in on top of exactly this function next.
 *
 * Every page a program touches must carry PTE_USER — leaf *and* every
 * table above it. Miss the intermediate ones and the CPU faults at CPL 3
 * even though the leaf looks right (paging.c propagates the bit for us).
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include <stddef.h>

#include "usermode.h"
#include "initrd.h"
#include "paging.h"
#include "pmm.h"
#include "serial.h"
#include "string.h"

/* Not noreturn despite the iretq: SYS_EXIT unwinds the stack back to the
 * instruction after the call, so this really does return — once. */
extern void usermode_enter(uint64_t entry, uint64_t user_rsp);
extern void user_exit_to_kernel(void);

static int exit_code_value;

static uint64_t page_up(uint64_t v)
{
    return (v + 0xFFFULL) & ~0xFFFULL;
}

/* Map `bytes` of the image at USER_BASE, one frame per 4 KiB. */
static int load_image(const struct initrd_file *file)
{
    if (file->size == 0 || file->size > USER_MAX_IMAGE) {
        serial_write("user: image size out of range - ");
        serial_write_dec(file->size);
        serial_write(" bytes\n");
        return -1;
    }

    uint64_t pages = page_up(file->size) / 4096;
    const uint8_t *src = file->data;

    for (uint64_t i = 0; i < pages; i++) {
        uint64_t phys = pmm_alloc_frame();
        if (phys == 0) {
            serial_write("user: out of frames while loading the image\n");
            return -1;
        }
        paging_map_page(USER_BASE + i * 4096, phys, PTE_WRITE | PTE_USER);

        /* The kernel's own mapping is live, so we can write the image in
         * through the same virtual address the program will execute. */
        uint8_t *dst = (uint8_t *)(uintptr_t)(USER_BASE + i * 4096);
        memset(dst, 0, 4096);
        uint64_t offset = i * 4096;
        uint64_t chunk = (file->size - offset > 4096) ? 4096 : file->size - offset;
        memcpy(dst, src + offset, (size_t)chunk);
    }
    return 0;
}

/* One stack region, mapped with the USER bit so iretq can land on it. */
static int map_stack(void)
{
    uint64_t pages = USER_STACK_SIZE / 4096;
    for (uint64_t i = 0; i < pages; i++) {
        uint64_t phys = pmm_alloc_frame();
        if (phys == 0) {
            serial_write("user: out of frames while building the stack\n");
            return -1;
        }
        paging_map_page(USER_STACK_TOP - (i + 1) * 4096, phys, PTE_WRITE | PTE_USER);
    }
    return 0;
}

int usermode_run(const char *path, int *exit_code)
{
    const struct initrd_file *file = initrd_find(path);
    if (file == NULL) {
        serial_write("user: no such program: ");
        serial_write(path);
        serial_write("\n");
        return -1;
    }

    if (load_image(file) != 0 || map_stack() != 0)
        return -1;

    serial_write("user: ");
    serial_write(path);
    serial_write(" loaded at ");
    serial_write_hex(USER_BASE);
    serial_write(" (");
    serial_write_dec(file->size);
    serial_write(" bytes), entering ring 3\n");

    exit_code_value = -1;

    /* 16-byte aligned top, minus one word so a `call` in user code pushes
     * into mapped memory instead of the guard-less hole above it. */
    uint64_t rsp = USER_STACK_TOP - 16;

    usermode_enter(USER_BASE, rsp);

    /* Reached only through SYS_EXIT -> user_exit_to_kernel(). */
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
