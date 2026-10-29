/* WindClone — /bin/imgview: decode a 24-bit bitmap and paint it in the window.
 *
 * A window here has always been made of glyphs: a program writes characters, the
 * shell assembles them into rows, the compositor draws the 5x7 font. That is a
 * terminal, and it is the right thing for `ls` and `cat`. It cannot show a
 * picture — a photograph, an icon, a screenshot — because a picture is not a
 * sequence of glyphs. v0.1.34 gave the surface a second channel (see RASTER_ESC
 * in shell.c): a program that has pixels writes them, and the kernel blits them.
 * This program is the first thing on the machine that produces real pixels and
 * uses that channel, and the headless test proves the pixels that reach the
 * window are the pixels the file held.
 *
 * It is a *program*, not a syscall, exactly like copy and unzip: the kernel
 * already has open() and write(). Decoding a bitmap is those two applied to a
 * format. No new syscall number, no new ABI surface.
 *
 * Why 24-bit BI_RGB specifically: it is the one bitmap shape a libc-less
 * freestanding program can decode without first reimplementing a second engine.
 * The pixels are three bytes each, sitting in the file, no run-length, no
 * deflate, no Huffman. That is not to say the decode is trivial — it still has to
 * get every real part of the format right: the little-endian header fields, the
 * four-byte row padding, the bottom-up row order (the first row in the file is
 * the *bottom* of the picture), and the BGR byte order that has to become the
 * RGB the framebuffer wants. Those four are precisely the bugs a bitmap decoder
 * actually has, and the test can catch each: a wrong stride shifts every row, a
 * wrong order flips the picture, a forgotten channel swap turns the red mid-row
 * blue.
 *
 * A format it cannot show is refused and not faked, the same rule /bin/unzip
 * holds for a deflated entry: a bitmap that is not 24-bit, or that is compressed,
 * is named and rejected with its real numbers rather than painted as garbage. A
 * viewer that showed a corrupt image would be worse than one that says what it
 * does not do.
 *
 * Why a digest over the decoded pixels. "imgview: read 4662 bytes" proves a
 * count of the file; it says nothing about the image. FNV-1a over the exact RGB
 * stream this program hands to the kernel — the same 32-bit hash copy, note and
 * unzip already print — lets the test require that the bytes that reached the
 * window are byte-for-byte the ones the bitmap held, decoded in the right order.
 * Not cryptographic, and nothing here claims it is.
 *
 * The words on screen are French, like every other window in this OS. The lines
 * it logs are English, because they are the lines CI greps.
 *
 * SPDX-License-Identifier: MIT
 */
#include "wcstr.h"

#define IMG_FILE_MAX   8192      /* the whole .bmp read into memory            */
#define IMG_RGB_MAX    (128 * 80 * 3)  /* a full window: the decoded pixels     */
#define IMG_PATH_MAX   64
#define IMG_MAX_W      128       /* the kernel's RASTER_MAX_W — kept in sync    */
#define IMG_MAX_H      80        /* the kernel's RASTER_MAX_H                   */

#define RASTER_ESC     0x1E      /* RS: the byte that opens a picture record    */

#define IMG_EXIT_OK         0
#define IMG_EXIT_USAGE      1   /* more than one word on the command line       */
#define IMG_EXIT_NOFILE     2   /* the bitmap could not be opened               */
#define IMG_EXIT_READ       3   /* the store refused a read of a file it lists  */
#define IMG_EXIT_NOBMP      4   /* not "BM" at byte 0: not a bitmap at all      */
#define IMG_EXIT_BIG        5   /* larger than IMG_FILE_MAX: refused whole      */
#define IMG_EXIT_UNSUPPORTED 6  /* right shape to try, wrong bpp or compression */
#define IMG_EXIT_TRUNCATED  7   /* the header promises pixels the file lacks    */

static char          file[IMG_FILE_MAX];
static unsigned long flen;
static unsigned char rgb[IMG_RGB_MAX];   /* decoded top-down RGB24, in order     */
static unsigned long rgbn;
static char          line[64];
static char          frame[256];
static char          path[IMG_PATH_MAX];

/* Little-endian reads, byte by byte: the BMP header is LE on every platform and
 * casting the buffer to a wider type would carry the host's endianness and
 * alignment into a format that has neither. Same reasoning as unzip's le16/le32. */
static unsigned le16(const char *p)
{
    return (unsigned)((unsigned char)p[0] |
                      ((unsigned)((unsigned char)p[1]) << 8));
}

static unsigned long le32(const char *p)
{
    return (unsigned long)((unsigned char)p[0] |
                           ((unsigned long)((unsigned char)p[1]) << 8) |
                           ((unsigned long)((unsigned char)p[2]) << 16) |
                           ((unsigned long)((unsigned char)p[3]) << 24));
}

/* --- log lines --------------------------------------------------------
 * One line, one write() so a tick cannot tear a line CI greps (see wcstr.h). The
 * appenders cap at 56 bytes; every line below is built to stay under that with
 * room for the pid. */
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

/* The command line is one optional path; the default is the committed specimen.
 * The path alphabet is the same one copy and unzip accept, so nothing here can be
 * coaxed into a name the store would not list back. */
static int take_path(int argc, char **argv)
{
    const char *src = (argc >= 2) ? argv[1] : "/src/logo.bmp";
    unsigned i = 0;

    for (; src[i] != '\0' && i < IMG_PATH_MAX - 1; i++) {
        char c = src[i];
        int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                 (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '/' ||
                 c == '-';
        if (!ok)
            return 0;
        path[i] = c;
    }
    path[i] = '\0';
    return i > 0 && src[i] == '\0';   /* refuse a path longer than the buffer */
}

/* Read the whole file into `file`. Returns 1 on success. A bitmap is read before
 * a byte is decoded so an input that is not a bitmap at all destroys nothing, and
 * so the row math can be bounded against a length already trusted. */
static int slurp(void)
{
    long fd = wc_open(path);

    if (fd < 0) {
        unsigned n = wc_app_str(line, 0, "imgview: cannot open ");
        n = wc_app_str(line, n, path);
        n = wc_app_str(line, n, ", error ");
        n = wc_app_signed(line, n, fd);
        say_pid(n);
        wc_exit(IMG_EXIT_NOFILE);
    }

    flen = 0;
    while (flen < IMG_FILE_MAX) {
        long got = wc_read(fd, file + flen, IMG_FILE_MAX - flen);
        if (got < 0) {
            unsigned n = wc_app_str(line, 0, "imgview: read failed, error ");
            n = wc_app_signed(line, n, got);
            say_pid(n);
            wc_close(fd);
            wc_exit(IMG_EXIT_READ);
        }
        if (got == 0)
            break;
        flen += (unsigned long)got;
    }
    wc_close(fd);

    if (flen >= 54 && le16(file) == 0x4D42) {
        /* Peeking the size field is enough to call an oversized file before the
         * loop stops early at IMG_FILE_MAX; a truncated read is caught below when
         * the pixel span is checked against flen. */
        unsigned long want = le32(file + 2);
        if (want > IMG_FILE_MAX) {
            unsigned n = wc_app_str(line, 0, "imgview: bitmap needs ");
            n = wc_app_dec(line, n, want);
            n = wc_app_str(line, n, " bytes, over ");
            n = wc_app_dec(line, n, IMG_FILE_MAX);
            say_pid(n);
            wc_exit(IMG_EXIT_BIG);
        }
    }

    {
        unsigned n = wc_app_str(line, 0, "imgview: read ");
        n = wc_app_dec(line, n, flen);
        n = wc_app_str(line, n, " bytes");
        say_pid(n);
    }
    return 1;
}

/* One hex digit, lowercase: the raster header the kernel parses accepts either
 * case (shell.c's hexval), and lowercase matches every digest this OS prints. */
static char hexdig(unsigned v)
{
    static const char d[] = "0123456789abcdef";
    return d[v & 0xF];
}

/* One write() moves at most a page: syscall.c caps `len` at USER_BUF_MAX and
 * returns the truncated count, so a program that asks 4608 bytes to go out gets
 * 4608 - 4096 = 512 of them lost. With a pixel record that is not a cosmetic
 * bug — the record never closes, and an open record eats every byte that follows
 * as pixels, so the picture's own caption and digest would vanish from the
 * window. The stream therefore goes out in chunks, and a chunk is a whole number
 * of RGB24 pixels so a write boundary never falls inside a pixel (the kernel
 * would survive it — rpix lives on the surface, not on the stack — but aligning
 * the split is what makes the two halves of this OS agree on what a frame is). */
#define IMG_WRITE_MAX 4096UL
#define IMG_CHUNK     (IMG_WRITE_MAX / 3UL * 3UL)   /* 4095: whole pixels */

/* Write it all, in chunks the kernel will accept, and follow the write's return
 * count instead of assuming it: a short write resumes where it stopped, and a
 * refused one ends the loop rather than spinning on a dead descriptor. */
static void emit(const char *p, unsigned long n)
{
    while (n > 0) {
        unsigned long part = (n > IMG_CHUNK) ? IMG_CHUNK : n;
        long got = wc_write(STDOUT_FILENO, p, part);

        if (got <= 0)
            break;
        p += (unsigned long)got;
        n -= (unsigned long)got;
    }
}

/* Emit the title frame first — a form feed to take the whole surface, and a
 * caption in French — then the picture record: RS 'R', four hex digits for the
 * width and height, and W*H RGB24 pixels after that. The text lands under the
 * bitmap because surf_draw blits the picture before it draws the rows. */
static void present(int w, int h)
{
    unsigned f = 0;
    char hdr[6];

    frame[f++] = '\f';
    frame[f++] = '\n';
    frame[f++] = 0x1C; frame[f++] = '1';       /* white caption ink */
    frame[f++] = 'I'; frame[f++] = 'M'; frame[f++] = 'A';
    frame[f++] = 'G'; frame[f++] = 'E';
    frame[f++] = ' ';
    for (const char *p = path; *p != '\0' && f < 200; p++)
        frame[f++] = (*p >= 'a' && *p <= 'z') ? (char)(*p - 32) : *p;
    frame[f++] = '\n';
    emit(frame, f);

    hdr[0] = (char)RASTER_ESC;
    hdr[1] = 'R';
    hdr[2] = hexdig((unsigned)w >> 4);
    hdr[3] = hexdig((unsigned)w);
    hdr[4] = hexdig((unsigned)h >> 4);
    hdr[5] = hexdig((unsigned)h);
    emit(hdr, sizeof hdr);
    emit((const char *)rgb, rgbn);
}

void _start(int argc, char **argv)
{
    unsigned long dataoff, stride, need;
    int w, h, topdown;
    unsigned long y, x;

    if (argc > 2) {
        unsigned n = wc_app_str(line, 0, "imgview: usage: imgview [fichier.bmp]");
        say_pid(n);
        wc_exit(IMG_EXIT_USAGE);
    }
    if (!take_path(argc, argv)) {
        unsigned n = wc_app_str(line, 0, "imgview: not a name the store can hold");
        say_pid(n);
        wc_exit(IMG_EXIT_USAGE);
    }

    slurp();

    if (flen < 54 || file[0] != 'B' || file[1] != 'M') {
        unsigned n = wc_app_str(line, 0, "imgview: not a bitmap, no BM header");
        say_pid(n);
        wc_exit(IMG_EXIT_NOBMP);
    }

    /* Width and height are signed. A negative height means the picture is stored
     * top-down already; a positive one means the file's first row is the bottom,
     * which is the case that trips most decoders. */
    w = (int)le32(file + 18);
    h = (int)le32(file + 22);
    if (h < 0) {
        topdown = 1;
        h = -h;
    } else {
        topdown = 0;
    }

    if (w < 1 || h < 1 || w > IMG_MAX_W || h > IMG_MAX_H) {
        unsigned n = wc_app_str(line, 0, "imgview: dimensions ");
        n = wc_app_signed(line, n, w);
        n = wc_app_str(line, n, "x");
        n = wc_app_dec(line, n, (unsigned long)h);
        n = wc_app_str(line, n, " won't fit a window");
        say_pid(n);
        wc_exit(IMG_EXIT_UNSUPPORTED);
    }

    {
        unsigned bpp = le16(file + 28);
        unsigned long comp = le32(file + 30);

        if (bpp != 24) {
            unsigned n = wc_app_str(line, 0, "imgview: only 24-bit, this is ");
            n = wc_app_dec(line, n, bpp);
            n = wc_app_str(line, n, " bits");
            say_pid(n);
            wc_exit(IMG_EXIT_UNSUPPORTED);
        }
        if (comp != 0) {
            unsigned n = wc_app_str(line, 0, "imgview: only uncompressed, method ");
            n = wc_app_dec(line, n, comp);
            say_pid(n);
            wc_exit(IMG_EXIT_UNSUPPORTED);
        }
    }

    dataoff = le32(file + 10);
    stride = ((unsigned long)w * 3 + 3) & ~3UL;   /* rows padded to a multiple of 4 */
    need = dataoff + stride * (unsigned long)h;
    if (dataoff < 54 || need > flen) {
        unsigned n = wc_app_str(line, 0, "imgview: pixels run past the file, need ");
        n = wc_app_dec(line, n, need);
        n = wc_app_str(line, n, " have ");
        n = wc_app_dec(line, n, flen);
        say_pid(n);
        wc_exit(IMG_EXIT_TRUNCATED);
    }

    /* Decode to top-down RGB, the order the framebuffer blits in. For a
     * bottom-up file, output row y reads file row (h-1-y); a BGR triplet on disk
     * leaves as R,G,B. */
    rgbn = 0;
    for (y = 0; y < (unsigned long)h; y++) {
        unsigned long fr = topdown ? y : ((unsigned long)h - 1 - y);
        unsigned long base = dataoff + fr * stride;

        for (x = 0; x < (unsigned long)w; x++) {
            unsigned char b = (unsigned char)file[base + x * 3];
            unsigned char g = (unsigned char)file[base + x * 3 + 1];
            unsigned char r = (unsigned char)file[base + x * 3 + 2];
            rgb[rgbn++] = r;
            rgb[rgbn++] = g;
            rgb[rgbn++] = b;
        }
    }

    present(w, h);

    {
        unsigned long digest = wc_fnv1a((const char *)rgb, rgbn);
        unsigned n = wc_app_str(line, 0, "imgview: image ");
        n = wc_app_dec(line, n, (unsigned long)w);
        n = wc_app_str(line, n, "x");
        n = wc_app_dec(line, n, (unsigned long)h);
        n = wc_app_str(line, n, " digest 0x");
        n = wc_app_hex(line, n, digest);
        say_pid(n);
    }
    wc_exit(IMG_EXIT_OK);
}
