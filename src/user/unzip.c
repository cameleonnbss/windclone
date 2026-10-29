/* WindClone — /bin/unzip: extract the entries of a ZIP archive into the store.
 *
 * Windows lets you double-click a .zip and get the files back out, and this OS
 * has, since v0.1.32, a shell that lists (dir), moves (ren) and deletes (del)
 * files in the writable store, plus a copier (copy) that carries bytes between
 * the boot archive and that store. The one file verb still missing was the
 * archive: a file that arrived as a single .zip could be listed but never
 * unpacked. This program closes that.
 *
 * It is a *program*, not a syscall, for the same reason copy is: the kernel
 * already has open() and write(), and unzipping is just those two applied to a
 * format. No new syscall number, no new ABI surface — the frozen int-0x80
 * contract in wcabi.h is untouched.
 *
 * What "stored" and "deflated" mean, and what this program does with each. A
 * ZIP local file header records, per entry, a two-byte compression method.
 * Method 0 is STORED — the bytes in the archive are already the file. Method 8
 * is DEFLATE, the format every real archive and every Python wheel uses, and it
 * needs an inflate engine. v0.1.33 had the walk and the store but not the
 * engine, so it extracted the stored half and refused the rest. This release
 * adds the engine: inflate.h decodes method 8 here, in userland, in the same
 * program, with no new syscall and nothing imported from zlib.
 *
 * The refusals that stay, and why they stay. A method that is neither 0 nor 8 is
 * named and skipped: bzip2 and LZMA entries are not this decoder's format and
 * guessing is how a corrupt file ends up called readme.txt. A deflated entry
 * whose decoded length disagrees with the size its own header claims is refused
 * with both numbers, because a stream that runs short or long is a damaged
 * archive — writing what came out anyway would swap "incomplete" for "done"
 * silently. And an entry that would overflow the store's per-file limit is
 * refused with its real size, exactly the way /bin/copy refuses an oversized
 * source. A copier that quietly produced a corrupt file is worse than one that
 * admits what it cannot do — the rule /bin/note applies to a truncated document
 * (v0.1.23) is the rule this program follows.
 *
 * Why the archive is read whole into a buffer before a byte is unpacked: so
 * that an input which is not a ZIP at all (bad signature) destroys nothing, and
 * so that walking the sequential local headers is a scan over memory I already
 * trust to be bounded by UNZIP_MAX. The header's own length fields are read but
 * never trusted past that bound: a truncated or oversized entry is refused with
 * the real numbers, exactly the way copy refuses a source too big for the
 * store.
 *
 * The name and the path, kept honest. An entry is extracted to <dest>/<name>,
 * and since v0.1.36 <name> may carry '/': a real archive — a jar, a wheel, a
 * Windows "extract here" folder — stores its members under directory names
 * (`demo/__init__.py`, `assets/icon.png`), and the store keys a file by its
 * whole path, so a nested member is one more key, not a filesystem. What is
 * still refused is the shape that is actually dangerous: an absolute name (a
 * leading '/'), a '..' component, or a '.' component, any of which would let an
 * archive climb out of the destination it was pointed at. A name made only of
 * the safe alphabet (letters digits _ . - and '/') and free of traversal is
 * taken as given. A directory entry — a name ending in '/', which is how a ZIP
 * records a folder — is skipped rather than written, because this store has no
 * folders and an empty file named after one would be a lie. dest goes through
 * the same path check copy uses, and the joined path is length-checked against
 * the store's own limit before it is opened.
 *
 * Why a digest per entry. `unzip: extracted ...` proves a count, and a count can
 * be satisfied by many wrong bytes. FNV-1a over the bytes unpacked — the same
 * 32-bit hash copy and note already print — gives the headless test a way to
 * require that what landed in the store is byte-for-byte what the archive held,
 * and gives a human the same eight hex digits on both sides. It is not
 * cryptographic and no line here says it is. For a deflated entry the bytes
 * hashed are the bytes that came out of inflate, not the compressed run: the
 * test then compares that number against fnv1a over what the archive's own
 * format says the file is, which is the only comparison that can tell a correct
 * decoder from one that produced 512 plausible-looking wrong bytes.
 *
 * Why the digest leads the entry line. Every appender caps a line at 56 bytes
 * (wcstr.h), and a nested name like `demo-1.0.dist-info/METADATA` is long enough
 * that if it came first it would push the eight hex digits — the whole proof —
 * off the end of the line CI greps. So the line is `unzip: extracted 0x<hex>
 * <n> b <name>`: the digest and the count are fixed-width and always survive,
 * and only the tail of a very long name can be cut. The path itself is proven
 * separately, by reading that member back out of the store at its full nested
 * name, so a truncated label never means a truncated file.
 *
 * The words on screen are French, like every other window in this OS. The lines
 * it logs are English, because they are the lines CI greps.
 *
 * SPDX-License-Identifier: MIT
 */
#include "wcstr.h"
#include "inflate.h"       /* the method-8 decoder, userland-side, no zlib */

/* Largest archive this program will read into memory. The store caps a file at
 * TMPFS_FILE_MAX (src/kernel/tmpfs.h, 4096); an archive is a bag of files plus
 * headers, so a little headroom over two files' worth is the honest ceiling for
 * a specimen, and anything bigger is refused with its length rather than
 * half-unpacked. */
#define UNZIP_MAX      8192
#define UNZIP_PATH_MAX 64
#define UNZIP_NAME_MAX 40          /* a relative entry path, dirs included, this program will build */
#define UNZIP_CHUNK    512
#define UNZIP_FILE_MAX 4096        /* TMPFS_FILE_MAX: the store's per-file ceiling */

/* What classify_entry() decided about an archive member's name. A wheel or a
 * jar mixes all three in one file: ordinary members, the folder records that
 * carry them, and — if someone built it carelessly or on purpose — a name that
 * would climb out of the destination. Only the first is written. */
#define ENTRY_OK    0      /* a safe relative path, copied into nm        */
#define ENTRY_DIR   1      /* ends in '/': a folder record, skipped       */
#define ENTRY_BAD   2      /* absolute, or a '.' / '..' component: refuse */

#define UNZIP_EXIT_OK       0
#define UNZIP_EXIT_USAGE    1   /* wrong number of words, or a refused path */
#define UNZIP_EXIT_NOFILE   2   /* the archive could not be opened or read */
#define UNZIP_EXIT_NOZIP    3   /* not a local-file-header signature at byte 0 */
#define UNZIP_EXIT_BIG      4   /* the archive is larger than UNZIP_MAX */

static char zipbuf[UNZIP_MAX];
static char chunk[UNZIP_CHUNK];
static char line[64];
static char dest[UNZIP_PATH_MAX];
static char outpath[UNZIP_PATH_MAX];
static char nm[UNZIP_NAME_MAX + 1];
/* The deflated entry's decoded bytes, one per-file ceiling at a time. Static for
 * the same reason zipbuf is: this userland has no heap on the path a program
 * takes to write a file, and a 4 KiB local would be 4 KiB of a 16 KiB stack. */
static unsigned char outbuf[UNZIP_FILE_MAX];

/* Little-endian reads: the ZIP format is byte-for-byte LE on every platform,
 * and casting the buffer to a wider type would carry the host's endianness and
 * alignment rules into a format that has neither. Byte assembly is what the
 * spec describes, so it is what this does. */
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
 * One line, one write() — see wcstr.h: a tick may land between two syscalls and
 * tear a line across another process's output, and CI greps whole lines. The
 * appenders cap at 56 bytes, so a very long name can be the first thing cut
 * off; that is a line a test stops matching rather than a line that quietly
 * means something else. */
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

/* The destination directory: an absolute path, the same alphabet copy accepts
 * (letters digits _ . / -), no "..". This is the border the flat-name rule
 * below leans on: nothing an entry names can climb out of here. */
static int valid_dest(const char *s)
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
        if (n + 1 >= UNZIP_PATH_MAX)
            return 0;
        dest[n] = c;
    }
    if (n == 1)
        return 0;                     /* "/" is a root, point somewhere under it */
    dest[n] = '\0';
    return 1;
}

/* An entry name, taken from the header (not NUL-terminated there), classified
 * and — for the safe case — copied into nm[]. A '/' is now a path separator, not
 * a refusal, so a wheel's `demo/__init__.py` unpacks; but the shapes that let an
 * archive escape the destination are still refused: an absolute name (a leading
 * '/'), a '.' or '..' component, an empty component from a doubled slash, or any
 * character outside letters digits _ . -. A name that ends in '/' is a folder
 * record (ENTRY_DIR): the store has no directories, so it is skipped rather than
 * written. Anything safe lands in nm[] (whole relative path, separators kept)
 * and the caller builds <dest>/<nm>. Returns ENTRY_OK/ENTRY_DIR/ENTRY_BAD. */
static int classify_entry(const char *p, unsigned len)
{
    unsigned i;

    if (len == 0 || len > UNZIP_NAME_MAX)
        return ENTRY_BAD;
    if (p[0] == '/')
        return ENTRY_BAD;                 /* absolute: would ignore dest */

    for (i = 0; i < len; i++)
        nm[i] = p[i];
    nm[len] = '\0';
    if (p[len - 1] == '/')
        return ENTRY_DIR;                 /* a folder record, nothing to write */

    i = 0;
    while (i < len) {
        unsigned j = i;
        unsigned clen;

        while (j < len && p[j] != '/')
            j++;
        clen = j - i;
        if (clen == 0)
            return ENTRY_BAD;             /* "//" or a stray separator */
        if (clen == 1 && p[i] == '.')
            return ENTRY_BAD;             /* "." adds nothing and hides intent */
        if (clen == 2 && p[i] == '.' && p[i + 1] == '.')
            return ENTRY_BAD;             /* ".." climbs out of dest */
        for (unsigned k = i; k < j; k++) {
            char c = p[k];
            int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                     (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-';
            if (!ok)
                return ENTRY_BAD;
        }
        i = j + 1;                        /* step over the separator */
    }
    return ENTRY_OK;
}

/* dest + '/' + nm, into outpath. dest was length-checked and nm is at most
 * UNZIP_NAME_MAX, so the sum either fits under UNZIP_PATH_MAX or is refused
 * here rather than overflowing a store name later. */
static int build_outpath(void)
{
    unsigned n = 0;
    unsigned dl = wc_strlen(dest);

    if (dl + 1 + wc_strlen(nm) + 1 > UNZIP_PATH_MAX)
        return 0;
    for (unsigned i = 0; i < dl; i++)
        outpath[n++] = dest[i];
    outpath[n++] = '/';
    for (unsigned i = 0; nm[i] != '\0'; i++)
        outpath[n++] = nm[i];
    outpath[n] = '\0';
    return 1;
}

/* Read the whole archive into zipbuf, bounded and measured, never trusting the
 * size a header claims. Returns the length, -1 on an open/read failure, -2 when
 * it runs past UNZIP_MAX. Mirrors copy's slurp so the two programs answer a big
 * or broken source with the same shape. */
static long slurp(const char *p)
{
    long fd = wc_open(p);
    unsigned long len = 0;

    if (fd < 0) {
        unsigned n = wc_app_str(line, 0, "unzip: cannot open ");
        n = wc_app_str(line, n, p);
        n = wc_app_str(line, n, ", error ");
        n = wc_app_signed(line, n, fd);
        say_pid(n);
        return -1;
    }
    for (;;) {
        unsigned long room = UNZIP_MAX - len;
        long got;

        if (room == 0) {
            unsigned long extra = 0;
            long more = wc_read(fd, chunk, UNZIP_CHUNK);

            while (more > 0) {
                extra += (unsigned long)more;
                more = wc_read(fd, chunk, UNZIP_CHUNK);
            }
            wc_close(fd);
            {
                unsigned n = wc_app_str(line, 0, "unzip: too big, ");
                n = wc_app_dec(line, n, UNZIP_MAX + extra);
                n = wc_app_str(line, n, " bytes at ");
                n = wc_app_str(line, n, p);
                say_pid(n);
            }
            return -2;
        }
        if (room > UNZIP_CHUNK)
            room = UNZIP_CHUNK;
        got = wc_read(fd, chunk, room);
        if (got < 0) {
            unsigned n;

            wc_close(fd);
            n = wc_app_str(line, 0, "unzip: read failed on ");
            n = wc_app_str(line, n, p);
            n = wc_app_str(line, n, ", error ");
            n = wc_app_signed(line, n, got);
            say_pid(n);
            return -1;
        }
        if (got == 0)
            break;
        for (long i = 0; i < got; i++)
            zipbuf[len + (unsigned long)i] = chunk[i];
        len += (unsigned long)got;
    }
    wc_close(fd);

    {
        unsigned n = wc_app_str(line, 0, "unzip: read ");
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
    unsigned off = 0;
    unsigned files = 0, refused = 0;
    unsigned long bytes = 0;

    if (argc < 2) {
        say("unzip: usage unzip <archive> [destination]");
        wc_exit(UNZIP_EXIT_USAGE);
    }
    if (argc >= 3) {
        if (!valid_dest(argv[2])) {
            say("unzip: a destination is absolute, letters digits _ . / - only, no ..");
            wc_exit(UNZIP_EXIT_USAGE);
        }
    } else {
        /* No destination named: the store's own front door, the same place a
         * copy without a full path would be pointed. Explicit, not guessed
         * from a cwd this OS does not have. */
        dest[0] = '/'; dest[1] = 't'; dest[2] = 'm'; dest[3] = 'p'; dest[4] = '\0';
    }

    len = slurp(argv[1]);
    if (len == -2)
        wc_exit(UNZIP_EXIT_BIG);
    if (len == -1)
        wc_exit(UNZIP_EXIT_NOFILE);

    if (len < 4 || zipbuf[0] != 'P' || zipbuf[1] != 'K' ||
        (unsigned char)zipbuf[2] != 3 || (unsigned char)zipbuf[3] != 4) {
        unsigned n = wc_app_str(line, 0, "unzip: not an archive, signature 0x");
        unsigned sig = len >= 2 ? (((unsigned)(unsigned char)zipbuf[0]) << 8) |
                                 (unsigned)(unsigned char)zipbuf[1] : 0;
        n = wc_app_hex(line, n, sig);
        say_pid(n);
        wc_exit(UNZIP_EXIT_NOZIP);
    }

    /* Sequential walk of local file headers. A real ZIP also carries a central
     * directory at the end; it is the archive's index, not its data, and this
     * program is the part that reads the data, so the walk stops at the first
     * four bytes that are not "PK\x03\x04". */
    while (off + 30 <= (unsigned long)len &&
           zipbuf[off] == 'P' && zipbuf[off + 1] == 'K' &&
           (unsigned char)zipbuf[off + 2] == 3 && (unsigned char)zipbuf[off + 3] == 4) {
        unsigned flags  = le16(zipbuf + off + 6);
        unsigned method = le16(zipbuf + off + 8);
        unsigned long comp   = le32(zipbuf + off + 18);
        unsigned long uncomp = le32(zipbuf + off + 22);
        unsigned nlen = le16(zipbuf + off + 26);
        unsigned elen = le16(zipbuf + off + 28);
        unsigned long dataoff = (unsigned long)off + 30 + nlen + elen;
        const char *namep = zipbuf + off + 30;
        const char *src;
        unsigned long outlen;
        unsigned n;

        if (dataoff + comp > (unsigned long)len) {
            n = wc_app_str(line, 0, "unzip: truncated archive, entry wants ");
            n = wc_app_dec(line, n, comp);
            n = wc_app_str(line, n, " bytes past the end");
            say_pid(n);
            break;
        }

        {
            int cls = classify_entry(namep, nlen);

            if (cls == ENTRY_DIR) {
                /* A folder record: the store has no directories, so there is
                 * nothing to write and nothing refused — say so and move on. */
                n = wc_app_str(line, 0, "unzip: skipped folder ");
                n = wc_app_str(line, n, nm);
                say_pid(n);
                off = (unsigned)(dataoff + comp);
                continue;
            }
            if (cls == ENTRY_BAD) {
                n = wc_app_str(line, 0, "unzip: refused entry ");
                n = wc_app_bytes(line, n, namep, nlen);
                n = wc_app_str(line, n, " - path escapes the destination");
                say_pid(n);
                refused++;
                off = (unsigned)(dataoff + comp);
                continue;
            }
        }

        if (flags & 0x08) {
            /* Sizes live in a trailing data descriptor, not the header, so the
             * walk cannot trust comp to step past this entry. Refused whole,
             * and the walk stops rather than resyncing on hope. */
            n = wc_app_str(line, 0, "unzip: refused ");
            n = wc_app_str(line, n, nm);
            n = wc_app_str(line, n, " - sizes not in the header");
            say_pid(n);
            break;
        }
        if (method != 0 && method != 8) {
            n = wc_app_str(line, 0, "unzip: refused ");
            n = wc_app_str(line, n, nm);
            n = wc_app_str(line, n, " - method ");
            n = wc_app_dec(line, n, method);
            say_pid(n);
            refused++;
            off = (unsigned)(dataoff + comp);
            continue;
        }
        if (uncomp > UNZIP_FILE_MAX) {
            n = wc_app_str(line, 0, "unzip: refused ");
            n = wc_app_str(line, n, nm);
            n = wc_app_str(line, n, " - ");
            n = wc_app_dec(line, n, uncomp);
            n = wc_app_str(line, n, " bytes over the store's limit");
            say_pid(n);
            refused++;
            off = (unsigned)(dataoff + comp);
            continue;
        }
        if (method == 0 && uncomp != comp) {
            n = wc_app_str(line, 0, "unzip: refused ");
            n = wc_app_str(line, n, nm);
            n = wc_app_str(line, n, " - stored entry sizes disagree");
            say_pid(n);
            refused++;
            off = (unsigned)(dataoff + comp);
            continue;
        }

        /* The bytes to store, and how many. Stored: the archive's own run.
         * Deflated: inflate it first, into the program's own buffer, and only
         * hand the store a file once the whole stream decoded and came out the
         * length the header promised. */
        src = zipbuf + dataoff;
        outlen = comp;
        if (method == 8) {
            unsigned long got = 0;
            int err = inflate_bytes((const unsigned char *)(zipbuf + dataoff),
                                    comp, outbuf, UNZIP_FILE_MAX, &got);

            if (err != INFLATE_OK) {
                n = wc_app_str(line, 0, "unzip: inflate failed on ");
                n = wc_app_str(line, n, nm);
                n = wc_app_str(line, n, " code ");
                n = wc_app_dec(line, n, (unsigned long)err);
                say_pid(n);
                refused++;
                off = (unsigned)(dataoff + comp);
                continue;
            }
            if (got != uncomp) {
                n = wc_app_str(line, 0, "unzip: refused ");
                n = wc_app_str(line, n, nm);
                n = wc_app_str(line, n, " - got ");
                n = wc_app_dec(line, n, got);
                n = wc_app_str(line, n, " of ");
                n = wc_app_dec(line, n, uncomp);
                n = wc_app_str(line, n, " bytes");
                say_pid(n);
                refused++;
                off = (unsigned)(dataoff + comp);
                continue;
            }
            src = (const char *)outbuf;
            outlen = got;
        }

        if (!build_outpath()) {
            n = wc_app_str(line, 0, "unzip: refused ");
            n = wc_app_str(line, n, nm);
            n = wc_app_str(line, n, " - path too long");
            say_pid(n);
            refused++;
            off = (unsigned)(dataoff + comp);
            continue;
        }

        {
            long fd = wc_open_flags(outpath, WC_O_WRONLY | WC_O_CREAT | WC_O_TRUNC);
            long wrote;

            if (fd < 0) {
                n = wc_app_str(line, 0, "unzip: cannot write ");
                n = wc_app_str(line, n, outpath);
                n = wc_app_str(line, n, ", error ");
                n = wc_app_signed(line, n, fd);
                say_pid(n);
                off = (unsigned)(dataoff + comp);
                continue;
            }
            wrote = wc_write(fd, src, outlen);
            wc_close(fd);

            if (wrote != (long)outlen) {
                n = wc_app_str(line, 0, "unzip: store took ");
                n = wc_app_dec(line, n, (unsigned long)(wrote < 0 ? 0 : wrote));
                n = wc_app_str(line, n, " of ");
                n = wc_app_dec(line, n, outlen);
                n = wc_app_str(line, n, " bytes for ");
                n = wc_app_str(line, n, nm);
                say_pid(n);
                off = (unsigned)(dataoff + comp);
                continue;
            }
        }

        {
            unsigned long digest = wc_fnv1a(src, outlen);
            n = wc_app_str(line, 0, "unzip: extracted 0x");
            n = wc_app_hex(line, n, digest);
            n = wc_app_str(line, n, " ");
            n = wc_app_dec(line, n, outlen);
            n = wc_app_str(line, n, " b ");
            n = wc_app_str(line, n, nm);
            say_pid(n);
        }
        files++;
        bytes += outlen;
        off = (unsigned)(dataoff + comp);
    }

    {
        unsigned n = wc_app_str(line, 0, "unzip: ");
        n = wc_app_dec(line, n, files);
        n = wc_app_str(line, n, " files, ");
        n = wc_app_dec(line, n, bytes);
        n = wc_app_str(line, n, " bytes into ");
        n = wc_app_str(line, n, dest);
        if (refused) {
            n = wc_app_str(line, n, ", refused ");
            n = wc_app_dec(line, n, refused);
        }
        say_pid(n);
    }
    wc_exit(UNZIP_EXIT_OK);
}
