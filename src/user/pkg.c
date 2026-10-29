/* WindClone — /bin/pkg: the fetched page, shown in a window.
 *
 * v0.1.29 proved the kernel could open a TCP connection, send an HTTP GET and
 * read a status line. v0.1.30 went one rung further: the response body is now
 * handed to the writable store as /net/fetched, so the page the machine pulled
 * off the network is a *file* — readable by any program through the same open()
 * the editor and the copier already use. This program is what makes that file
 * mean something to a person: it opens /net/fetched, paints its bytes into its
 * own window, and prints the digest the kernel computed on the way in.
 *
 * Why it reads rather than fetches. A package manager's two halves are pulling
 * bytes and showing them, and the pull is a kernel job driven from the idle loop
 * (a program cannot pump SLIRP: only the `hlt` pump gives the emulator CPU). So
 * the fetch happens at boot and lands in the store; /bin/pkg is the second half,
 * the reader, and it is exactly as ordinary as /bin/view. That split is honest —
 * nothing here pretends a ring-3 program opened the socket — and it is the shape
 * a real `wget`/browser pairing has anyway.
 *
 * Why a digest and not a screenshot. `pkg: rendered 43 bytes` proves a count,
 * and a count is satisfied by 43 wrong bytes. FNV-1a over what was read back,
 * printed here and recomputed by the headless test from the page it served, is
 * the one number that says the whole path — TCP to the store to the VFS to this
 * window — moved the *same* bytes. It is 32 bits and not cryptographic; no line
 * below claims otherwise.
 *
 * The words on screen are French, like every other window in this OS. The lines
 * it logs are English, because they are the lines CI greps.
 *
 * SPDX-License-Identifier: MIT
 */
#include "wcstr.h"

#define PKG_MAX        2048         /* the whole fetched page, in RAM        */
#define PKG_ROWS         12         /* body rows the window holds            */
#define PKG_LINE         56         /* cells a row is capped to, no wrapping */

#define PKG_EXIT_OK      0
#define PKG_EXIT_NOFILE  2   /* /net/fetched is not there (nothing was fetched) */
#define PKG_EXIT_READ    3   /* the store refused a read of a file it lists     */

static const char PKG_PATH[] = "/net/fetched";

static char page[PKG_MAX];
static unsigned plen;               /* bytes of page body */
static char line[64];
static char frame[3072];
static unsigned fn;
static unsigned row_cells;

/* --- log lines --------------------------------------------------------
 * One line, one write(): a tick cannot tear a line a test greps. See wcstr.h. */
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

/* --- the frame --------------------------------------------------------
 * A form feed turns the surface into a screen rather than another scrollback
 * line, exactly as it does for /bin/view (see shell.c surf_write). The rows are
 * counted, not free: an overlong page row is clipped to PKG_LINE cells so it
 * cannot wrap into the status row, and the frame is written in one syscall so a
 * window changes in a single step. */
static void f_addc(char c)
{
    if (row_cells >= PKG_LINE || fn + 1 >= (unsigned)sizeof frame)
        return;
    frame[fn++] = c;
    row_cells++;
}

static void f_add(const char *s)
{
    while (*s)
        f_addc(*s++);
}

static void f_addn(unsigned long v)
{
    char d[24];
    unsigned n = 0;

    if (v == 0)
        d[n++] = '0';
    while (v > 0 && n < sizeof d) {
        d[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (n > 0)
        f_addc(d[--n]);
}

static void f_addhex(unsigned long v)
{
    static const char digits[] = "0123456789abcdef";
    for (int i = 7; i >= 0; i--)
        f_addc(digits[(v >> (i * 4)) & 0xF]);
}

static void f_begin(void) { row_cells = 0; }

static void f_end(void)
{
    if (fn + 1 < (unsigned)sizeof frame)
        frame[fn++] = '\n';
}

/* A byte of the page as a glyph. The font has no picture for control bytes, so
 * tab and everything outside printable ASCII becomes the blank it would be. The
 * file's bytes are never touched; this is only what a human can read. */
static char show_byte(char c)
{
    if (c == '\t')
        return ' ';
    if (c >= 32 && c < 127)
        return c;
    return ' ';
}

static void draw(unsigned long digest)
{
    unsigned i = 0, painted;

    fn = 0;
    f_begin();
    f_addc('\f');                    /* a screen, not a line */
    f_end();

    f_begin();
    f_add("PAGE WEB - ");
    f_add(PKG_PATH);
    f_end();

    f_begin();
    f_end();

    /* Paint up to PKG_ROWS body rows, splitting the page on newlines. A single
     * line of HTML lands on one row and the rest of the window stays blank, so
     * the frame is kept exactly its height and the status row never moves. */
    painted = 0;
    while (painted < PKG_ROWS && i < plen) {
        f_begin();
        while (i < plen && page[i] != '\n' && row_cells < PKG_LINE) {
            f_addc(show_byte(page[i]));
            i++;
        }
        /* Consume the rest of an overlong row so the next row is the next line
         * of the page, not the middle of this one. */
        while (i < plen && page[i] != '\n')
            i++;
        if (i < plen && page[i] == '\n')
            i++;
        f_end();
        painted++;
    }
    while (painted < PKG_ROWS) {
        f_begin();
        f_end();
        painted++;
    }

    f_begin();
    f_add("RECUPERE  ");
    f_addn(plen);
    f_add(" OCTETS  FNVS 0x");
    f_addhex(digest);
    f_end();

    f_begin();
    f_add("PAGE INSTALLEE DEPUIS HTTP - VIEWER: pkg");
    f_end();

    wc_write(STDOUT_FILENO, frame, fn);
}

void _start(int argc, char **argv)
{
    long fd;
    unsigned long digest;

    (void)argc;
    (void)argv;

    fd = wc_open(PKG_PATH);
    if (fd < 0) {
        unsigned n = wc_app_str(line, 0, "pkg: cannot open /net/fetched, error ");
        n = wc_app_signed(line, n, fd);
        n = wc_app_str(line, n, " - nothing was fetched this boot");
        say_pid(n);
        wc_exit(PKG_EXIT_NOFILE);
    }

    while (plen < PKG_MAX) {
        long got = wc_read(fd, page + plen, PKG_MAX - plen);
        if (got < 0) {
            unsigned n = wc_app_str(line, 0, "pkg: read failed on /net/fetched, error ");
            n = wc_app_signed(line, n, got);
            say_pid(n);
            wc_close(fd);
            wc_exit(PKG_EXIT_READ);
        }
        if (got == 0)
            break;
        plen += (unsigned)got;
    }
    wc_close(fd);

    {
        unsigned n = wc_app_str(line, 0, "pkg: read ");
        n = wc_app_dec(line, n, plen);
        n = wc_app_str(line, n, " bytes from /net/fetched");
        say_pid(n);
    }

    /* Taken over the bytes as they arrived in this window, before drawing, so
     * the number is a fact about what the store held and not about the frame. */
    digest = wc_fnv1a(page, (unsigned long)plen);

    draw(digest);

    {
        unsigned n = wc_app_str(line, 0, "pkg: rendered fetched page, ");
        n = wc_app_dec(line, n, plen);
        n = wc_app_str(line, n, " bytes, digest 0x");
        n = wc_app_hex(line, n, digest);
        say_len(n);
    }
    wc_exit(PKG_EXIT_OK);
}
