/* WindClone — /bin/certutil: cmd's hash tool, over the file layer.
 *
 * fc answers "are these two files the same?" and find answers "is this text in
 * here?"; the third question a person asks of a downloaded file is the one no
 * line-comparison can answer honestly — "is this the exact file the author
 * published?" Windows answers it with `certutil -hashfile`: the same 64-hex-digit
 * SHA-256 the release page prints, computed over the bytes on disk. This is that
 * verb, on the same open()/read() path copy, fc, find and sort already proved, so
 * it reaches both the read-only archive (`/bin/hello`) and the writable store
 * (`/tmp/x.txt`) with the same syscall the copier uses.
 *
 * Why SHA-256 and nothing else: it is the algorithm Windows 11's certutil prints
 * when you type `certutil -hashfile file` with no algorithm word, and it is what
 * every ISO and package digest on this project's own release page is published
 * as. MD5 and SHA-1 are broken and shipping them here would be an invitation to
 * trust the wrong thing; a name that is not "sha256" is refused outright rather
 * than silently hashed with a stronger algorithm the caller did not ask for.
 *
 * Why the digest is cross-checked and not asserted: a hash is only a number, and
 * a wrong SHA-256 implementation can print a plausible 64-digit string. So the
 * test does not read our digits and nod at them — it feeds the *same bytes*
 * through Python's own hashlib and requires the two 64-hex strings to be equal,
 * byte for byte. The claim is arithmetic (FIPS 180-4 against a reference), not a
 * sentence about a picture on the screen.
 *
 * Why it streams instead of slurping: fc and find must hold a whole file to walk
 * its lines, but a hash is a fold — a block at a time into a 32-byte chaining
 * value — so certutil never needs the file in memory and can digest a file of
 * any size the store can open. There is therefore no "too big" refusal to make;
 * the only refusals are a file that cannot be opened and a wrong command line.
 *
 * The exit code is cmd's shape: 0 when a hash was printed, 2 for a wrong command
 * line, an unreadable file, or an unsupported algorithm word.
 *
 * The words on screen are French, like every other window in this OS. The lines
 * it logs are English, because they are the lines CI greps.
 *
 * SPDX-License-Identifier: MIT
 */
#include "wcstr.h"

#define CU_PATH_MAX   40
#define CU_CHUNK      64      /* one SHA-256 block; read() fills it exactly */

#define CU_EXIT_OK       0
#define CU_EXIT_USAGE    2   /* no file, a refused path, or a bad algorithm */
#define CU_EXIT_NOFILE   2   /* a file could not be opened or read */

/* Absolute path, letters digits _ . / - only, no "..". Filled into `path` so the
 * label the program prints is the same bytes it actually opened. */
static char path[CU_PATH_MAX];
static char line[64];
static char dline[96];       /* "certutil: sha256 " + 64 hex + newline */

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
        if (n + 1 >= CU_PATH_MAX)
            return 0;
        path[n] = c;
    }
    if (n == 1)
        return 0;
    path[n] = '\0';
    return 1;
}

/* --- the hash --------------------------------------------------------- *
 * FIPS 180-4 SHA-256, streaming. Only 32-bit unsigned arithmetic and 64-bit
 * for the bit-length field, so it needs no libc and survives -mno-sse: the
 * compiler cannot vectorise a rotate on a machine with no vector registers.
 */
static unsigned h[8];
static unsigned char block[CU_CHUNK];
static unsigned blen;        /* bytes buffered in block[] */
static unsigned long long total;   /* whole bytes hashed so far */

static const unsigned K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

static unsigned ror(unsigned x, unsigned n)
{
    return (x >> n) | (x << (32 - n));
}

static void transform(const unsigned char *p)
{
    unsigned w[64];
    unsigned a, b, c, d, e, f, g, hh;
    unsigned t;

    for (t = 0; t < 16; t++)
        w[t] = ((unsigned)p[t * 4] << 24) | ((unsigned)p[t * 4 + 1] << 16) |
               ((unsigned)p[t * 4 + 2] << 8) | (unsigned)p[t * 4 + 3];
    for (t = 16; t < 64; t++) {
        unsigned s0 = ror(w[t - 15], 7) ^ ror(w[t - 15], 18) ^ (w[t - 15] >> 3);
        unsigned s1 = ror(w[t - 2], 17) ^ ror(w[t - 2], 19) ^ (w[t - 2] >> 10);
        w[t] = w[t - 16] + s0 + w[t - 7] + s1;
    }
    a = h[0]; b = h[1]; c = h[2]; d = h[3];
    e = h[4]; f = h[5]; g = h[6]; hh = h[7];
    for (t = 0; t < 64; t++) {
        unsigned S1 = ror(e, 6) ^ ror(e, 11) ^ ror(e, 25);
        unsigned ch = (e & f) ^ (~e & g);
        unsigned temp1 = hh + S1 + ch + K[t] + w[t];
        unsigned S0 = ror(a, 2) ^ ror(a, 13) ^ ror(a, 22);
        unsigned maj = (a & b) ^ (a & c) ^ (b & c);
        unsigned temp2 = S0 + maj;
        hh = g; g = f; f = e; e = d + temp1;
        d = c; c = b; b = a; a = temp1 + temp2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d;
    h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

static void init_hash(void)
{
    h[0] = 0x6a09e667u; h[1] = 0xbb67ae85u; h[2] = 0x3c6ef372u;
    h[3] = 0xa54ff53au; h[4] = 0x510e527fu; h[5] = 0x9b05688cu;
    h[6] = 0x1f83d9abu; h[7] = 0x5be0cd19u;
    blen = 0;
    total = 0;
}

/* Feed `n` bytes, running a transform for every full 64-byte block assembled. */
static void update(const unsigned char *p, unsigned long n)
{
    total += n;
    for (; n > 0; n--, p++) {
        block[blen++] = *p;
        if (blen == CU_CHUNK) {
            transform(block);
            blen = 0;
        }
    }
}

/* Pad and run the final block(s) so h[] holds the message digest. */
static void finalize(void)
{
    unsigned long long bits = total * 8ULL;
    unsigned char pad[CU_CHUNK];
    unsigned i;

    pad[0] = 0x80;
    for (i = 1; i < CU_CHUNK; i++)
        pad[i] = 0;
    /* A 0x80 byte, then zeros to 56 mod 64, then the 8-byte big-endian length.
     * Emit enough pad bytes to reach the boundary, then a length-only block. */
    {
        unsigned need = (blen < 56) ? (56 - blen) : (120 - blen);
        update(pad, need);
    }
    pad[0] = (unsigned char)(bits >> 56);
    pad[1] = (unsigned char)(bits >> 48);
    pad[2] = (unsigned char)(bits >> 40);
    pad[3] = (unsigned char)(bits >> 32);
    pad[4] = (unsigned char)(bits >> 24);
    pad[5] = (unsigned char)(bits >> 16);
    pad[6] = (unsigned char)(bits >> 8);
    pad[7] = (unsigned char)(bits);
    update(pad, 8);
}

/* --- messaging -------------------------------------------------------- */

static void emit_line(void)
{
    wc_write(STDOUT_FILENO, line, wc_strlen(line));
    wc_write(STDOUT_FILENO, "\n", 1);
}

static void say_pid(void)
{
    unsigned n = wc_app_str(line, wc_strlen(line), " pid ");
    n = wc_app_dec(line, n, (unsigned long)wc_getpid());
    line[n] = '\0';
    emit_line();
}

/* The command line was wrong (no file, or an unrecognised switch): say so once
 * and stop. Reused by both the bad-flag and the missing-path branches. */
static void say_usage(void)
{
    unsigned n = wc_app_str(line, 0, "certutil: usage certutil -hashfile FICHIER");
    line[n] = '\0';
    emit_line();
}

/* Print the digest as "certutil: sha256 <64 hex>". Built in its own 96-byte
 * buffer and written in one syscall, because the 64 hex digits alone overflow
 * the 56-byte cap the shared line appenders enforce. */
static void print_digest(void)
{
    static const char xd[] = "0123456789abcdef";
    unsigned n = 0;
    const char *tag = "certutil: sha256 ";
    int word, nib;

    while (*tag)
        dline[n++] = *tag++;
    for (word = 0; word < 8; word++) {
        for (nib = 7; nib >= 0; nib--)
            dline[n++] = xd[(h[word] >> (nib * 4)) & 0xF];
    }
    dline[n++] = '\n';
    wc_write(STDOUT_FILENO, dline, n);
}

/* Case-insensitive compare of a token to a lowercase literal, used for the
 * flag and algorithm words (cmd's switches and algorithm names are both). */
static int ci_eq(const char *tok, const char *lit)
{
    for (;;) {
        char c = *tok;
        if (c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 'a');
        if (c != *lit)
            return 0;
        if (c == '\0')
            return 1;
        tok++;
        lit++;
    }
}

void _start(int argc, char **argv)
{
    long fd;
    int i = 1;

    /* cmd spells this `certutil -hashfile <file> [algo]`. The dash flag is
     * optional so `certutil <file>` works too; a leading "-" token we do not
     * recognise is a wrong command line, not a path. */
    if (i < argc && argv[i][0] == '-') {
        if (!ci_eq(argv[i] + 1, "hashfile")) {
            say_usage();
            wc_exit(CU_EXIT_USAGE);
        }
        i++;
    }
    if (i >= argc) {
        say_usage();
        wc_exit(CU_EXIT_USAGE);
    }
    if (!valid_path(argv[i])) {
        unsigned n = wc_app_str(line, 0, "certutil: a path is absolute, no ..");
        line[n] = '\0';
        emit_line();
        wc_exit(CU_EXIT_USAGE);
    }
    i++;
    /* Optional algorithm word: only sha256 exists here. */
    if (i < argc && !ci_eq(argv[i], "sha256")) {
        unsigned n = wc_app_str(line, 0, "certutil: algorithm not supported: ");
        n = wc_app_str(line, n, argv[i]);
        line[n] = '\0';
        emit_line();
        wc_exit(CU_EXIT_USAGE);
    }

    {
        unsigned n = wc_app_str(line, 0, "certutil: ");
        n = wc_app_str(line, n, path);
        line[n] = '\0';
        emit_line();
    }

    fd = wc_open(path);
    if (fd < 0) {
        unsigned n = wc_app_str(line, 0, "certutil: cannot open ");
        n = wc_app_str(line, n, path);
        n = wc_app_str(line, n, ", error ");
        n = wc_app_signed(line, n, fd);
        line[n] = '\0';
        say_pid();
        wc_exit(CU_EXIT_NOFILE);
    }

    init_hash();
    for (;;) {
        unsigned char chunk[CU_CHUNK];
        long got = wc_read(fd, chunk, CU_CHUNK);

        if (got < 0) {
            unsigned n;
            wc_close(fd);
            n = wc_app_str(line, 0, "certutil: read failed on ");
            n = wc_app_str(line, n, path);
            n = wc_app_str(line, n, ", error ");
            n = wc_app_signed(line, n, got);
            line[n] = '\0';
            say_pid();
            wc_exit(CU_EXIT_NOFILE);
        }
        if (got == 0)
            break;
        update(chunk, (unsigned long)got);
    }
    wc_close(fd);
    finalize();
    print_digest();
    wc_exit(CU_EXIT_OK);
}
