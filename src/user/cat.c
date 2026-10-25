/* WindClone — /bin/cat: copy a file's bytes to stdout.
 *
 * The built-in this replaces printed through kprint(), which meant a file's
 * contents were really the kernel's console text: no length limit, no error
 * path, and nothing else could ever reuse it. Here the bytes travel the only
 * route a program has — open(), read() into a buffer the kernel has to
 * validate, write(1) — so this file also proves the VFS works for a caller
 * that knows nothing about it beyond the ABI in wcabi.h.
 *
 * One decision worth stating: read() returning 0 is end of file, and a
 * negative return is an error. Treating both as "stop, exit 0" would let a
 * truncated file pass as a complete one, so the two are handled apart.
 *
 * SPDX-License-Identifier: MIT
 */
#include "wcstr.h"

#define CHUNK 512                 /* 512 read()s of 4 KiB = a 2 MiB program */

static char buf[CHUNK];

/* Returns 0 on a clean copy, 1 on any failure. */
static int cat_one(const char *path)
{
    long fd = wc_open(path);
    if (fd < 0) {
        wc_puts("[cat] cannot open ");
        wc_write(STDERR_FILENO, path, wc_strlen(path));
        wc_puts("\n");
        return 1;
    }

    for (;;) {
        long n = wc_read(fd, buf, sizeof buf);
        if (n == 0)
            break;                /* clean end of file */
        if (n < 0) {
            wc_close(fd);
            wc_puts("[cat] read failed on ");
            wc_write(STDERR_FILENO, path, wc_strlen(path));
            wc_puts("\n");
            return 1;
        }
        wc_write(STDOUT_FILENO, buf, (unsigned long)n);
    }

    wc_close(fd);
    return 0;
}

void _start(int argc, char **argv)
{
    int code = 0;

    if (argc <= 1) {
        /* No arguments means standard input for real cat. Here the keyboard
         * belongs to the shell and read(0) answers -WC_ENOSYS, so saying so
         * is the whole of what this program can honestly do. */
        wc_puts("[cat] no input files; the keyboard is not wired to stdin\n");
        wc_exit(1);
    }

    for (int i = 1; i < argc; i++) {
        if (wc_streq(argv[i], "-")) {
            wc_puts("[cat] - (stdin) is not available yet\n");
            continue;
        }
        if (cat_one(argv[i]) != 0)
            code = 1;
    }

    wc_exit(code);
}
