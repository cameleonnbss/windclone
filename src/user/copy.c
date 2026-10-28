/* WindClone — /bin/copy: the program that moves bytes between the two halves
 * of the file layer.
 *
 * The initrd is a boot archive and /tmp is a RAM store, and until now the only
 * way a byte could cross from one to the other was a program that knew about
 * both. That is a small gap with large consequences: /bin/note can only edit
 * what the store holds, the store starts every boot empty, and so this OS had a
 * text editor with nothing real in it to edit. `copy /src/hello.c /tmp/hello.c`
 * closes the gap — and it is a *program*, not a syscall and not a shell builtin,
 * because the kernel already has open() and write() and needs no third way to
 * say the same thing.
 *
 * Why the whole file is read before a single byte is written: so that a source
 * which cannot be opened never destroys its destination. `copy missing.c
 * important.c` leaves important.c exactly as it was, which is what a person who
 * has just typed the wrong name needs and is not something a two-descriptor
 * byte pump gives you for free.
 *
 * Why one buffer the size of the store's own limit, and a refusal instead of a
 * truncation: COPY_MAX is TMPFS_FILE_MAX (src/kernel/tmpfs.h), the most bytes
 * that can exist in a file here. A copier that quietly wrote the first half of a
 * long source would leave a file that is *almost* the original — the worst
 * possible outcome, because it looks like it worked. So the read keeps going
 * past a full buffer to count what is left, and the answer is "too big" with the
 * real number in it.
 *
 * Why a digest. `copy: wrote 1819 bytes` proves a count, and a count can be
 * satisfied by 1819 wrong bytes. FNV-1a over the bytes read, printed again by
 * whoever reads the copy back, gives the headless test something better than a
 * length to compare and a human something better than a hope to read: the same
 * eight hex digits have to appear on both sides of an editor before anything
 * here is entitled to call itself a round trip. It is 32 bits and it is not
 * cryptographic; no line below claims it is.
 *
 * The words on screen are French, like every other window in this OS. The lines
 * it logs are English, because they are the lines CI greps.
 *
 * SPDX-License-Identifier: MIT
 */
#include "wcstr.h"

/* The store's own ceiling, mirrored here the way the tests mirror the kernel's
 * constants: if tmpfs ever grows, a copier that refuses a bigger file is a bug
 * the build is expected to notice and not one left for a user to find. */
#define COPY_MAX        4096
#define COPY_PATH_MAX   40
#define COPY_CHUNK      512

#define COPY_EXIT_OK      0
#define COPY_EXIT_USAGE   1   /* wrong number of words, or a path refused */
#define COPY_EXIT_NOFILE  2   /* the source is not there, or not readable */
#define COPY_EXIT_BIG     3   /* the source is larger than the store can hold */
#define COPY_EXIT_OPEN    4   /* the destination could not be opened to write */
#define COPY_EXIT_SHORT   5   /* the store took fewer bytes than it was handed */

static char srcbuf[COPY_MAX];
static char path[2][COPY_PATH_MAX];
static char line[64];
static char chunk[COPY_CHUNK];

/* A path, not a name: copy moves between the archive and the store, so it has
 * to take both as given. Absolute, letters digits _ . / - only, and no ".." —
 * the kernel resolves exact names and could not be fooled, but a copier that
 * refuses a traversal is a copier a person can point at two files and trust. */
static int valid_path(int slot, const char *s)
{
    unsigned n = 0;

    if (s[0] != '/')
        return 0;
    for (; s[n] != '\0'; n++) {
        char c = s[n];
        int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                 (c >= '0' && c <= '9') || c == '_' || c == '.' ||
                 c == '/' || c == '-';
        if (!ok)
            return 0;
        if (c == '.' && s[n + 1] == '.')
            return 0;                 /* the only shape that could mean "up" */
        if (n + 1 >= COPY_PATH_MAX)
            return 0;                 /* room left for the NUL, checked not hoped */
        path[slot][n] = c;
    }
    if (n == 1)
        return 0;                     /* "/" is a directory, not a file */
    path[slot][n] = '\0';
    return 1;
}

/* --- log lines --------------------------------------------------------
 * One line, one write() — see wcstr.h: a tick may land between two syscalls and
 * tear a line across another process's output, and CI greps whole lines. The
 * appenders cap at 56 bytes, so the pid can be the first thing a long pair of
 * paths cuts off; that is a line a test stops matching rather than a line that
 * quietly means something else. */
static void say_len(unsigned n)
{
    line[n] = '\n';
    wc_write(STDOUT_FILENO, line, n + 1);
}

static void say_pid(unsigned n)
{
    n = wc_app_str(line, n, " pid ");
    n = wc_app_dec(line, n, (unsigned long)wc_getpid());
    say_len(n);
}

static void say(const char *s)
{
    say_len(wc_app_str(line, 0, s));
}

/* Read the whole of `p` into srcbuf and report how many bytes that was. The
 * size is measured and never trusted: a file in the archive carries a ustar
 * length field, and this program does not read it, because an archive that lies
 * about a length is the exact thing a copier must not take on faith.
 *
 * Returns the length, -1 when the file could not be opened or read, and -2 when
 * it is bigger than the store could ever hold. */
static long slurp(const char *p)
{
    long fd = wc_open(p);
    unsigned long len = 0;

    if (fd < 0) {
        unsigned n = wc_app_str(line, 0, "copy: cannot open ");
        n = wc_app_str(line, n, p);
        n = wc_app_str(line, n, ", error ");
        n = wc_app_signed(line, n, fd);
        say_pid(n);
        return -1;
    }
    for (;;) {
        unsigned long room = COPY_MAX - len;
        long got;

        if (room == 0) {
            /* The buffer is exactly full. Reading on decides between "the file
             * was COPY_MAX bytes" — which fits, and is the reason this branch
             * exists — and something bigger, which is refused with the count
             * that made the decision rather than a guess at it. */
            unsigned long extra = 0;
            long more = wc_read(fd, chunk, COPY_CHUNK);

            while (more > 0) {
                extra += (unsigned long)more;
                more = wc_read(fd, chunk, COPY_CHUNK);
            }
            wc_close(fd);
            if (more < 0) {
                unsigned n = wc_app_str(line, 0, "copy: read failed on ");
                n = wc_app_str(line, n, p);
                n = wc_app_str(line, n, ", error ");
                n = wc_app_signed(line, n, more);
                say_pid(n);
                return -1;
            }
            if (extra == 0)
                break;                    /* the file was exactly COPY_MAX */
            {
                unsigned n = wc_app_str(line, 0, "copy: too big, ");
                n = wc_app_dec(line, n, COPY_MAX + extra);
                n = wc_app_str(line, n, " bytes at ");
                n = wc_app_str(line, n, p);
                say_pid(n);
                n = wc_app_str(line, 0, "copy: the store holds ");
                n = wc_app_dec(line, n, COPY_MAX);
                n = wc_app_str(line, n, " bytes per file");
                say_len(n);
            }
            return -2;
        }
        if (room > COPY_CHUNK)
            room = COPY_CHUNK;
        got = wc_read(fd, chunk, room);
        if (got < 0) {
            unsigned n;

            wc_close(fd);
            n = wc_app_str(line, 0, "copy: read failed on ");
            n = wc_app_str(line, n, p);
            n = wc_app_str(line, n, ", error ");
            n = wc_app_signed(line, n, got);
            say_pid(n);
            return -1;
        }
        if (got == 0)
            break;                        /* clean end of file */
        for (long i = 0; i < got; i++)
            srcbuf[len + (unsigned long)i] = chunk[i];
        len += (unsigned long)got;
    }
    wc_close(fd);

    {
        unsigned n = wc_app_str(line, 0, "copy: read ");
        n = wc_app_dec(line, n, len);
        n = wc_app_str(line, n, " bytes from ");
        n = wc_app_str(line, n, p);
        say_pid(n);
    }
    return (long)len;
}

void _start(int argc, char **argv)
{
    long len;
    unsigned long digest;

    if (argc < 3) {
        say("copy: usage copy <source> <destination>");
        wc_exit(COPY_EXIT_USAGE);
    }
    if (!valid_path(0, argv[1]) || !valid_path(1, argv[2])) {
        say("copy: a path is absolute, letters digits _ . / - only, no ..");
        wc_exit(COPY_EXIT_USAGE);
    }

    len = slurp(path[0]);
    if (len == -2)
        wc_exit(COPY_EXIT_BIG);
    if (len == -1)
        wc_exit(COPY_EXIT_NOFILE);

    /* Taken over what was read and before anything is handed to the store, so
     * the number describes the source and not the hope about it. wcstr.h owns
     * the algorithm, because /bin/note has to answer with the same number. */
    digest = wc_fnv1a(srcbuf, (unsigned long)len);
    {
        unsigned n = wc_app_str(line, 0, "copy: digest 0x");
        n = wc_app_hex(line, n, digest);
        say_pid(n);
    }

    {
        long fd = wc_open_flags(path[1], WC_O_WRONLY | WC_O_CREAT | WC_O_TRUNC);
        long wrote;

        if (fd < 0) {
            unsigned n = wc_app_str(line, 0, "copy: cannot write ");
            n = wc_app_str(line, n, path[1]);
            n = wc_app_str(line, n, ", error ");
            n = wc_app_signed(line, n, fd);
            say_pid(n);
            wc_exit(COPY_EXIT_OPEN);
        }
        wrote = wc_write(fd, srcbuf, (unsigned long)len);
        wc_close(fd);

        if (wrote != len) {
            unsigned n = wc_app_str(line, 0, "copy: store took ");
            n = wc_app_dec(line, n, (unsigned long)(wrote < 0 ? 0 : wrote));
            n = wc_app_str(line, n, " of ");
            n = wc_app_dec(line, n, (unsigned long)len);
            n = wc_app_str(line, n, " bytes at ");
            say_pid(n);
            wc_exit(COPY_EXIT_SHORT);
        }
    }

    {
        unsigned n = wc_app_str(line, 0, "copy: wrote ");
        n = wc_app_dec(line, n, (unsigned long)len);
        n = wc_app_str(line, n, " bytes to ");
        n = wc_app_str(line, n, path[1]);
        say_pid(n);
    }
    wc_exit(COPY_EXIT_OK);
}
