/* WindClone — /bin/motd: the first program that reads a file.
 *
 * /bin/hello proves the privilege boundary. This proves the two things the
 * ELF loader and the VFS were built for, and it checks them from *inside* the
 * program, where a loader bug shows up as a wrong number rather than a fault:
 *
 *   - .data came out of the file (marker 0x5a5a) and .bss did not exist in it
 *     at all, so a non-zero counter at the start would mean p_memsz -
 *     p_filesz was not zeroed;
 *   - open()/read()/close() work from ring 3, one page at a time, through
 *     descriptors the program cannot forge and buffers the kernel has to
 *     validate before touching.
 *
 * Freestanding: no libc, no startup files, and deliberately no call that
 * binutils would have to satisfy from libgcc — the link is -nostdlib, so a
 * memcpy() reference here would fail the build instead of hiding.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>

#include "wcuser.h"

/* Initialized, so it lives in a PT_LOAD that carries these bytes.
 *
 * volatile is not about interrupts here — it is the test. A plain static that
 * nothing ever writes would be folded by gcc into the constant at its use
 * site, the variable would vanish, .data would be empty, and the program would
 * "prove" the loader placed initialised data correctly while reading nothing
 * from memory at all. Forcing the load means the number printed below really
 * came out of the page the kernel filled. */
static volatile unsigned long data_marker = 0x5A5AUL;

/* Uninitialized: addresses only. The loader owes us zeros. */
static unsigned long bytes_read;
static char chunk[128];
static char digits[24];

static const char banner[] =
    "[motd] elf64 program: open()/read()/close() through the vfs\n";
static const char seg_a[]  = "[motd] segments: .data marker=0x";
static const char seg_b[]  = " .bss counter=";
static const char hexdig[] = "0123456789abcdef";
static const char nofile[] = "[motd] open(/etc/motd) failed\n";
static const char tail[]   = "[motd] closed. bytes read: ";
static const char after[]  = "[motd] read after close returned -";

/* Write `value` in decimal. Built backwards into digits[] and copied out
 * reversed, because there is no memmove to borrow and 20 bytes is not worth
 * writing one for. */
static void put_dec(unsigned long value)
{
    if (value == 0) {
        wc_write(STDOUT_FILENO, "0", 1);
        return;
    }

    unsigned i = 0;
    while (value > 0) {
        digits[i++] = (char)('0' + (value % 10));
        value /= 10;
    }
    for (unsigned k = 0; k < i; k++)
        chunk[k] = digits[i - 1 - k];
    wc_write(STDOUT_FILENO, chunk, i);
}

static void put_hex16(unsigned long value)
{
    for (int shift = 12; shift >= 0; shift -= 4)
        wc_write(STDOUT_FILENO, &hexdig[(value >> shift) & 0xFUL], 1);
}

void _start(void)
{
    wc_puts(banner);

    /* Report the image's own state before touching either field, so the
     * numbers below are what the loader delivered and not what this program
     * happened to compute. */
    wc_puts(seg_a);
    put_hex16(data_marker);
    wc_puts(seg_b);
    put_dec(bytes_read);
    wc_write(STDOUT_FILENO, "\n", 1);

    long fd = wc_open("/etc/motd");
    if (fd < 0) {
        wc_puts(nofile);
        wc_exit(1);
    }

    for (;;) {
        long n = wc_read(fd, chunk, sizeof chunk);
        if (n <= 0)                 /* 0 = clean end of file, <0 = an error */
            break;
        bytes_read += (unsigned long)n;
        wc_write(STDOUT_FILENO, chunk, (unsigned long)n);
    }
    wc_close(fd);

    wc_puts(tail);
    put_dec(bytes_read);
    wc_write(STDOUT_FILENO, "\n", 1);

    /* A closed descriptor has to stop answering. If the slot table were
     * sloppy this either succeeds with someone else's bytes or blocks; -9
     * (EBADF) is the only honest answer, and printing it is how a smoke test
     * can grep for that. */
    long again = wc_read(fd, chunk, sizeof chunk);
    wc_puts(after);
    put_dec((unsigned long)(0 - again));
    wc_write(STDOUT_FILENO, "\n", 1);

    wc_exit(0);
}
