/* WindClone — /bin/browser: an address bar and the page under it.
 *
 * v0.1.30 could fetch one page, at boot, into one file, and show it. That is a
 * download, not a browser: nothing was *asked for* and the screen said nothing
 * about where its bytes came from. v0.1.31 adds both halves, and this program is
 * the visible one.
 *
 * What the bar shows is not this program's guess. `fetch /second` at the prompt
 * hands the path to net.c, tcp.c builds `GET /second HTTP/1.0` from it, and the
 * kernel keeps the address it asked for as `10.0.2.2:8080/second`; syscall 14
 * copies those words into the buffer below. This
 * program has no way to name a page — it opens /net/fetched like any other file —
 * so the text in the bar can only be the kernel's own record, read here through
 * the system call the browser release added. A bar that echoed what somebody typed would be
 * a picture of an address bar; this one reports a request that went on the wire.
 *
 * Why it renders the page rather than fetching it: a received frame exists only
 * once the emulator's network gets CPU, and that happens in the idle loop's `hlt`,
 * so the pump is the kernel's job and has been since v0.1.26. The split is the one
 * a real wget/browser pair has anyway, and it keeps this program as ordinary as
 * /bin/view: open, read, paint.
 *
 * The colour is the other half of "browser". The bytes in the file are markup, so
 * the tags are painted in one ink, an attribute name in another, a quoted value in
 * a third and the text between tags in the default green. That is a five-rule
 * lexer, not a DOM, and no line below claims otherwise; what makes it worth
 * testing is that the kernel counted the pens it moved (`wm: colour`) and that this
 * program counted the tags it recognised before it painted any of them.
 *
 * The words on screen are French, like every other window in this OS. The lines it
 * logs are English, because they are the lines CI greps.
 *
 * SPDX-License-Identifier: MIT
 */
#include "wcstr.h"

#define BR_MAX         2048         /* the whole fetched page, in RAM           */
#define BR_ROWS          11         /* body rows the window holds               */
#define BR_LINE          56         /* cells a row is capped to, no wrapping    */
#define BR_ADDR          96         /* long enough for the longest address      */

#define BR_EXIT_OK       0
#define BR_EXIT_NOFILE   2   /* /net/fetched is not there (nothing was fetched) */
#define BR_EXIT_READ     3   /* the store refused a read of a file it lists     */

static const char BR_PATH[] = "/net/fetched";

/* The inks, named for the markup role they carry. They are indices into
 * shell.c's color_ink[], the same eight any program may ask for. */
#define INK_WHITE     1
#define INK_TAG       2
#define INK_ATTR      3
#define INK_TEXTNODE  0      /* the default green: the text between the tags */

static char page[BR_MAX];
static unsigned plen;               /* bytes of page body */
static char line[64];
static char frame[4096];
static unsigned fn;
static unsigned row_cells;
static char addr[BR_ADDR];

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
 * line, exactly as it does for /bin/view and /bin/pkg (see shell.c surf_write).
 * The rows are counted, not free: an overlong page row is clipped to BR_LINE
 * cells so it cannot wrap into the status row, and the frame is written in one
 * syscall so a window changes in a single step. */
static void f_addc(char c)
{
    if (row_cells >= BR_LINE || fn + 1 >= (unsigned)sizeof frame)
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

/* Move the pen. FS followed by a digit is consumed by the kernel's surface
 * parser and costs no cell on the screen, so a highlighted page is the same
 * bytes as a plain one plus two per colour change. */
static void f_ink(int ink)
{
    if (fn + 2 >= (unsigned)sizeof frame)
        return;
    frame[fn++] = 0x1C;
    frame[fn++] = (char)('0' + (ink & 7));
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

/* The kernel's record of what it asked for, taken over syscall 14: net_addr
 * copies tcp.c's own words into this program's buffer, page-checked at the
 * border. A ring-3 program cannot call a kernel function, and it has no way to
 * name a page — so the text in the bar can only be the kernel's answer, read
 * from the record of a request that actually went on the wire. NULL before the
 * first fetch, which the bar shows as an explicit refusal rather than an empty
 * field: an address bar that prints nothing looks like a page that failed to
 * load, and here nothing was ever requested. */
static void take_addr(void)
{
    if (wc_net_addr(addr, sizeof addr) != 0)
        addr[0] = '\0';
}

/* Paint one row of markup and return the index just past it, counting the tags it
 * recognised on the way. The rules are four and deliberately not a parser: inside
 * `<...>` the first word is the element, a later word is an attribute, a run
 * between quotes is its value, and text outside the brackets is text. A tag that
 * never closes runs to the end of the row in the tag ink, which is what a reader
 * wants from a broken document and not what a compiler does with one. */
static unsigned draw_row(unsigned i, unsigned *tags)
{
    int inside = 0, name = 0, quoted = 0;
    unsigned seen_tags = 0;

    f_begin();
    while (i < plen && page[i] != '\n' && row_cells < BR_LINE) {
        char c = page[i];

        if (c == '<') {
            inside = 1;
            name = 1;
            f_ink(INK_TAG);
            f_addc('<');
        } else if (inside && c == '>') {
            inside = 0;
            name = 0;
            *tags += 1;
            seen_tags += 1;
            f_addc('>');
            f_ink(INK_TEXTNODE);
        } else if (c == '"') {
            quoted = !quoted;
            f_ink(quoted ? INK_ATTR : (inside ? INK_WHITE : INK_TEXTNODE));
            f_addc(c);
        } else if (quoted) {
            f_addc(show_byte(c));
        } else if (inside && (c == ' ' || c == '\t')) {
            name = 0;                 /* the next run is an attribute, not the name */
            f_ink(INK_TAG);
            f_addc(c);
        } else if (inside && (c == '=' || c == '/')) {
            name = 1;                 /* ... and one starts again right after it */
            f_ink(INK_TAG);
            f_addc(c);
        } else if (inside && name) {
            f_ink(seen_tags == 0 ? INK_WHITE : INK_ATTR);
            f_addc(show_byte(c));
        } else {
            f_ink(INK_TEXTNODE);
            f_addc(show_byte(c));
        }
        i++;
    }
    /* Consume the rest of an overlong row so the next row is the next line of the
     * page and not the middle of this one. */
    while (i < plen && page[i] != '\n')
        i++;
    if (i < plen && page[i] == '\n')
        i++;
    f_end();
    return i;
}

static void draw(unsigned long digest, unsigned *tags)
{
    unsigned i = 0, painted;

    fn = 0;
    f_begin();
    f_addc('\f');                    /* a screen, not a line */
    f_end();

    f_begin();
    f_ink(INK_WHITE);
    f_add("NAVIGATEUR - /NET/FETCHED");
    f_end();

    f_begin();
    f_end();

    /* The address bar, in the kernel's own words. */
    f_begin();
    f_ink(INK_TAG);
    f_add("ADRESSE ");
    f_ink(INK_WHITE);
    if (addr[0] != '\0')
        f_add(addr);
    else
        f_add("AUCUNE REQUETE EMISE");
    f_ink(INK_TEXTNODE);
    f_end();

    f_begin();
    f_add("--------------------");
    f_end();

    painted = 0;
    while (painted < BR_ROWS && i < plen) {
        i = draw_row(i, tags);
        painted++;
    }
    while (painted < BR_ROWS) {
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
    f_add("PAGE INSTALLEE PAR LE NOYAU - BARRE LUE VIA APPEL 14");
    f_end();

    wc_write(STDOUT_FILENO, frame, fn);
}

void _start(int argc, char **argv)
{
    long fd;
    unsigned tags = 0;
    unsigned long digest;

    (void)argc;
    (void)argv;

    take_addr();

    fd = wc_open(BR_PATH);
    if (fd < 0) {
        unsigned n = wc_app_str(line, 0, "browser: cannot open /net/fetched, error ");
        n = wc_app_signed(line, n, fd);
        n = wc_app_str(line, n, " - nothing was fetched this boot");
        say_pid(n);
        wc_exit(BR_EXIT_NOFILE);
    }

    while (plen < BR_MAX) {
        long got = wc_read(fd, page + plen, BR_MAX - plen);
        if (got < 0) {
            unsigned n = wc_app_str(line, 0, "browser: read failed on /net/fetched, error ");
            n = wc_app_signed(line, n, got);
            say_pid(n);
            wc_close(fd);
            wc_exit(BR_EXIT_READ);
        }
        if (got == 0)
            break;
        plen += (unsigned)got;
    }
    wc_close(fd);

    {
        unsigned n = wc_app_str(line, 0, "browser: read ");
        n = wc_app_dec(line, n, plen);
        n = wc_app_str(line, n, " bytes from /net/fetched");
        say_pid(n);
    }

    /* Taken over the bytes as they arrived, before drawing, so the number is a
     * fact about what the store held and not about the frame. */
    digest = wc_fnv1a(page, (unsigned long)plen);

    draw(digest, &tags);

    {
        /* "rendered N bytes, T tags, digest 0x........" — kept deliberately
         * short: every appender caps at 56 bytes (wcstr.h), and a line whose
         * digest was silently sliced in half is a test that can never pass. */
        unsigned n = wc_app_str(line, 0, "browser: rendered ");
        n = wc_app_dec(line, n, plen);
        n = wc_app_str(line, n, " bytes, ");
        n = wc_app_dec(line, n, tags);
        n = wc_app_str(line, n, " tags, digest 0x");
        n = wc_app_hex(line, n, digest);
        say_len(n);
    }
    if (addr[0] != '\0') {
        unsigned n = wc_app_str(line, 0, "browser: address bar shows ");
        n = wc_app_str(line, n, addr);
        say_len(n);
    }
    wc_exit(BR_EXIT_OK);
}
