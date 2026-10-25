/* WindClone — /bin/ls: enumerate what the kernel actually has.
 *
 * The shell used to answer `ls` by calling initrd_count()/initrd_at() itself,
 * which meant "ls" was a feature of the shell and not of the OS: no other
 * program could list anything, and a bug in the listing was a bug in the
 * kernel. This is the honest shape — ring 3 asks for entry N until the kernel
 * says -WC_ENOENT, and the same SYS_GETDIRENT answer is what a future file
 * browser will draw.
 *
 * There is no directory tree. The archive is a flat list of paths, so `ls
 * /etc` would need a readdir per directory and a real inode layer; until then
 * one line per file is the truth and a fake tree would be a lie.
 *
 * SPDX-License-Identifier: MIT
 */
#include "wcstr.h"

static struct wc_dirent ent;      /* one syscall's worth, in .bss */
static const char newline = '\n';

void _start(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    unsigned long count = 0, bytes = 0;

    for (unsigned long i = 0; wc_getdirent(i, &ent) == 0; i++) {
        unsigned long n = wc_strnlen(ent.name, sizeof ent.name);
        if (n == 0)
            continue;                    /* nothing to show, do not count it */

        wc_write(STDOUT_FILENO, ent.name, n);
        /* Pad to a column so the sizes line up: the kernel gives no width
         * hint, so the program decides how its own table should read. */
        for (unsigned long p = n; p < 24; p++)
            wc_write(STDOUT_FILENO, " ", 1);
        wc_put_dec(STDOUT_FILENO, ent.size);
        wc_write(STDOUT_FILENO, &newline, 1);

        count++;
        bytes += (unsigned long)ent.size;
    }

    wc_puts("[ls] ");
    wc_put_dec(STDOUT_FILENO, count);
    wc_puts(" entries, ");
    wc_put_dec(STDOUT_FILENO, bytes);
    wc_puts(" bytes\n");

    wc_exit(0);
}
