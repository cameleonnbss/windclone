/* WindClone — /bin/sort: cmd's SORT, an ordering pass over the file layer.
 *
 * fc compares two files, find pulls the lines that carry a text; the verb that
 * arranges them is the one a person reaches for to read a list straight.
 * Windows' `SORT` is exactly that: it reads a file and prints its lines in
 * order. This is that verb, on the same open() path copy, fc and find already
 * proved, so it reaches both the read-only initrd and the writable store.
 *
 * Why the comparison is a plain byte order and not a locale collation: cmd's
 * default dictionary order folds case and consults the /L codepage, which is a
 * promise this libc-less kernel cannot keep honestly for every byte. A
 * byte-for-byte order (compare, and when one line is a prefix of the other the
 * shorter comes first) is total, deterministic, and — most importantly for a
 * test — reproducible: the harness sorts the same lines with Python's own
 * byte order and requires the program to agree, so the claim is arithmetic and
 * not a sentence.
 *
 * Why a line cap and not just a byte cap: SORT_MAX is how much text this run
 * can hold, but the reorder itself is O(n^2) and needs one index slot per line,
 * so SORT_LINES bounds the *count* independently. A file that fits in bytes but
 * has more lines than that (the specimen source is one) is refused with its
 * line count rather than half-sorted — a partial order would look like an answer.
 *
 * Why /r and nothing else: cmd's /R reverses, which is the only switch that is
 * a pure reading of the same sorted result. The column switches ( /+n ) count
 * fixed-width columns this flat, textual store does not promise, so they are
 * left out rather than half-implemented.
 *
 * The exit code is cmd's shape, like fc and find: 0 when the file was sorted
 * and printed, 2 for a wrong command line or a file that cannot be read, is too
 * big, or has too many lines.
 *
 * The words on screen are French, like every other window in this OS. The lines
 * it logs are English, because they are the lines CI greps.
 *
 * SPDX-License-Identifier: MIT
 */
#include "wcstr.h"

#define SORT_MAX        4096
#define SORT_PATH_MAX   40
#define SORT_CHUNK      512
#define SORT_LINES      32   /* index slots; also the reorder's own ceiling */

#define SORT_EXIT_OK      0
#define SORT_EXIT_USAGE   2   /* no file, or a refused path */
#define SORT_EXIT_NOFILE  2   /* a file could not be opened or read */
#define SORT_EXIT_BIG     2   /* a file is larger than this program can hold */
#define SORT_EXIT_MANY    2   /* more lines than there are index slots */

static char buf[SORT_MAX];
static char path[SORT_PATH_MAX];
static char line[64];
static char chunk[SORT_CHUNK];
static unsigned starts[SORT_LINES];
static unsigned lens[SORT_LINES];
static unsigned order[SORT_LINES];
static int reverse;

/* Absolute path, letters digits _ . / - only, no "..". */
static int valid_path(const char *s)
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
            return 0;
        if (n + 1 >= SORT_PATH_MAX)
            return 0;
        path[n] = c;
    }
    if (n == 1)
        return 0;
    path[n] = '\0';
    return 1;
}

static void say_len(unsigned n)
{
    line[n] = '\n';
    wc_write(STDOUT_FILENO, line, n + 1);
}

static void say(const char *s)
{
    say_len(wc_app_str(line, 0, s));
}

static void say_pid(unsigned n)
{
    n = wc_app_str(line, n, " pid ");
    n = wc_app_dec(line, n, (unsigned long)wc_getpid());
    say_len(n);
}

/* Is line index `ia` strictly before `ib` in byte order? Compare the common
 * length byte by byte; when one is a prefix of the other the shorter sorts
 * first. This is the whole ordering, and Python's sorted() over the same lines
 * uses the same rule, which is what makes the test's cross-check exact. */
static int less(unsigned ia, unsigned ib)
{
    unsigned a = starts[ia], la = lens[ia];
    unsigned b = starts[ib], lb = lens[ib];
    unsigned m = la < lb ? la : lb;
    unsigned k;

    for (k = 0; k < m; k++) {
        unsigned char ca = (unsigned char)buf[a + k];
        unsigned char cb = (unsigned char)buf[b + k];
        if (ca != cb)
            return ca < cb;
    }
    return la < lb;
}

/* Read `p` whole into buf. Returns the length, -1 unopenable/unreadable, -2
 * bigger than SORT_MAX. */
static long slurp(const char *p)
{
    long fd = wc_open(p);
    unsigned long len = 0;

    if (fd < 0) {
        unsigned n = wc_app_str(line, 0, "sort: cannot open ");
        n = wc_app_str(line, n, p);
        n = wc_app_str(line, n, ", error ");
        n = wc_app_signed(line, n, fd);
        say_pid(n);
        return -1;
    }
    for (;;) {
        unsigned long room = SORT_MAX - len;
        long got;

        if (room == 0) {
            unsigned long extra = 0;
            long more = wc_read(fd, chunk, SORT_CHUNK);

            while (more > 0) {
                extra += (unsigned long)more;
                more = wc_read(fd, chunk, SORT_CHUNK);
            }
            wc_close(fd);
            if (more < 0) {
                unsigned n = wc_app_str(line, 0, "sort: read failed on ");
                n = wc_app_str(line, n, p);
                n = wc_app_str(line, n, ", error ");
                n = wc_app_signed(line, n, more);
                say_pid(n);
                return -1;
            }
            if (extra == 0)
                break;
            {
                unsigned n = wc_app_str(line, 0, "sort: too big, ");
                n = wc_app_dec(line, n, SORT_MAX + extra);
                n = wc_app_str(line, n, " bytes at ");
                n = wc_app_str(line, n, p);
                say_pid(n);
            }
            return -2;
        }
        if (room > SORT_CHUNK)
            room = SORT_CHUNK;
        got = wc_read(fd, chunk, room);
        if (got < 0) {
            unsigned n;

            wc_close(fd);
            n = wc_app_str(line, 0, "sort: read failed on ");
            n = wc_app_str(line, n, p);
            n = wc_app_str(line, n, ", error ");
            n = wc_app_signed(line, n, got);
            say_pid(n);
            return -1;
        }
        if (got == 0)
            break;
        for (long i = 0; i < got; i++)
            buf[len + (unsigned long)i] = chunk[i];
        len += (unsigned long)got;
    }
    wc_close(fd);
    return (long)len;
}

/* Split buf[0..len) into lines on '\n'. The newline is a separator, not part of
 * a line, and a single trailing newline does not make an empty last line — the
 * same reading the harness applies in Python. Returns the line count, or -1 if
 * it would overflow SORT_LINES (with the real count said, never a partial sort). */
static long split_lines(unsigned long len)
{
    unsigned i = 0, start = 0, n = 0;

    for (i = 0; i < len; i++) {
        if (buf[i] == '\n') {
            if (n >= SORT_LINES) {
                unsigned k = wc_app_str(line, 0, "sort: too many lines, ");
                k = wc_app_dec(line, k, n + 1);
                k = wc_app_str(line, k, " at ");
                k = wc_app_str(line, k, path);
                say_pid(k);
                return -1;
            }
            starts[n] = start;
            lens[n] = i - start;
            n++;
            start = i + 1;
        }
    }
    if (start < len) {                    /* final line without newline */
        if (n >= SORT_LINES) {
            unsigned k = wc_app_str(line, 0, "sort: too many lines, ");
            k = wc_app_dec(line, k, n + 1);
            k = wc_app_str(line, k, " at ");
            k = wc_app_str(line, k, path);
            say_pid(k);
            return -1;
        }
        starts[n] = start;
        lens[n] = (unsigned)len - start;
        n++;
    }
    return (long)n;
}

void _start(int argc, char **argv)
{
    int i;
    long flen, nl;
    unsigned k;
    unsigned printed;

    reverse = 0;
    i = 1;
    /* Leading switches: exactly '/' plus one letter. /r reverses; anything
     * else (or a longer token, which is a path) stops the switch parse. */
    while (i < argc && argv[i][0] == '/' && argv[i][1] != '\0'
           && argv[i][2] == '\0') {
        char f0 = argv[i][1];
        if (f0 == 'r' || f0 == 'R')
            reverse = 1;
        else
            break;
        i++;
    }
    if (i >= argc) {
        say("sort: usage sort [/r] fichier");
        wc_exit(SORT_EXIT_USAGE);
    }
    if (!valid_path(argv[i])) {
        say("sort: a path is absolute, letters digits _ . / - only, no ..");
        wc_exit(SORT_EXIT_USAGE);
    }

    {
        unsigned n = wc_app_str(line, 0, "sort: ");
        n = wc_app_str(line, n, path);
        if (reverse)
            n = wc_app_str(line, n, " (reverse)");
        say_len(n);
    }

    flen = slurp(path);
    if (flen < 0) {                        /* slurp already said why */
        wc_exit(flen == -2 ? SORT_EXIT_BIG : SORT_EXIT_NOFILE);
    }
    nl = split_lines((unsigned long)flen);
    if (nl < 0)
        wc_exit(SORT_EXIT_MANY);

    /* Insertion sort into `order`, ascending by less(); stable, and with
     * distinct lines the only possible answer. */
    for (k = 0; k < (unsigned)nl; k++)
        order[k] = k;
    for (k = 1; k < (unsigned)nl; k++) {
        unsigned key = order[k];
        long j = (long)k - 1;
        while (j >= 0 && less(key, order[j])) {
            order[j + 1] = order[j];
            j--;
        }
        order[j + 1] = key;
    }

    printed = 0;
    for (k = 0; k < (unsigned)nl; k++) {
        unsigned idx = reverse ? (unsigned)(nl - 1 - k) : k;
        unsigned li = order[idx];
        unsigned m = wc_app_str(line, 0, "sort: ");
        m = wc_app_bytes(line, m, buf + starts[li], lens[li]);
        say_len(m);
        printed++;
    }
    {
        unsigned n = wc_app_str(line, 0, "sort: ");
        n = wc_app_dec(line, n, printed);
        n = wc_app_str(line, n, " lines sorted");
        say_len(n);
    }
    wc_exit(SORT_EXIT_OK);
}
