/* WindClone — inflate: an RFC 1951 decoder for the bytes a real archive holds.
 *
 * Why this file exists. A .zip you can download is not a bag of stored files:
 * every entry Windows Explorer unpacks for you, and every wheel on PyPI, is
 * DEFLATE-compressed (method 8). Until now /bin/unzip could only read method 0
 * and had to refuse the interesting half of the format, so "extract an archive
 * the way Windows does" was true for a specimen we built ourselves and false for
 * anything else. This is the decoder that closes that gap: header-only, included
 * by the program that needs it, and no new syscall — the kernel stays out of it.
 *
 * How it is written, and why it is slow on purpose. There is no table-driven
 * "inflate_fast" here, no canonical-code pointer magic, and no malloc. Each code
 * bit is read one at a time and matched against the canonical ranges, which is
 * the algorithm as the spec describes it, about 15 lines, and impossible to get
 * subtly wrong in a way that survives a byte-for-byte digest test. A specimen of
 * a few hundred bytes decodes in microseconds on this machine; speed is a
 * different ticket than correctness, and pretending otherwise is how a decoder
 * ends up writing plausible garbage into the store.
 *
 * Two properties a decoder either states or lies about:
 *   - Bit order. Deflate packs bits from the low end of each byte, so a plain
 *     shift-out is used for lengths and extra bits; Huffman codes are the one
 *     exception, packed from the high end, which is why the symbol reader
 *     accumulates with `code = (code << 1) | bit` and why mixing the two is the
 *     classic inflate bug. Both orders are handled explicitly below.
 *   - Boundaries. Every read checks the input length, every write checks the
 *     output capacity, and a back-reference that reaches before the start of
 *     the output is an error rather than a read of uninitialised memory. On any
 *     of them the caller gets an error code and writes *nothing* — half a
 *     decompressed file in the store is worse than no file, the same rule
 *     /bin/copy applies to an oversized source.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef WINDCLONE_INFLATE_H
#define WINDCLONE_INFLATE_H

#define INFLATE_MAXBITS   15
#define INFLATE_CODES     288   /* literal/length alphabet: 0..287 */
#define INFLATE_DIST      32    /* distance alphabet: 0..31, 30/31 invalid */

/* Every failure is one code, and the caller prints which. "corrupt" and
 * "too big" must not share a number: one means the archive is broken and the
 * user should get another file, the other means this machine's buffer is the
 * limit and the entry should be reported with its real size. */
#define INFLATE_OK         0
#define INFLATE_ECORRUPT   1   /* bad block type, bad tree, unknown symbol */
#define INFLATE_EINPUT     2   /* the stream ran past the bytes we were given */
#define INFLATE_EOVER      3   /* more bytes out than the caller's capacity */
#define INFLATE_EDIST      4   /* a match reaching before the start of output */

struct inf_huff {
    unsigned short count[INFLATE_MAXBITS + 1];
    unsigned short symbol[INFLATE_CODES];
};

struct inf {
    const unsigned char *in;
    unsigned long inlen;
    unsigned long inpos;
    unsigned int  bitbuf;
    int nbits;
    int fail;              /* sticky: a read or write went past a boundary */
};

/* Length codes 257..285: base length and extra bits, straight from the spec. */
static const unsigned short inf_lbase[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
};
static const unsigned char inf_lbits[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
};
static const unsigned short inf_dbase[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193,
    12289, 16385, 24577
};
static const unsigned char inf_dbits[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};
/* The order code lengths are stored in for a dynamic block (RFC 1951 3.2.7). */
static const unsigned char inf_clorder[19] = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
};

/* --- bit input ---------------------------------------------------------
 * Low bit of the current byte first. Returns 0 and arms `fail` once the input
 * is exhausted, so a truncated stream decodes into a buffer nobody will write
 * instead of reading past it. */
static int inf_bit(struct inf *z)
{
    int b;

    if (z->nbits == 0) {
        if (z->inpos >= z->inlen) {
            z->fail = 1;
            return 0;
        }
        z->bitbuf = z->in[z->inpos++];
        z->nbits = 8;
    }
    b = (int)(z->bitbuf & 1U);
    z->bitbuf >>= 1;
    z->nbits--;
    return b;
}

/* n <= 25 bits, low-order bit first: how LEN, HDIST extras and HLIT itself
 * arrive. A 16-bit read here would be byte order plus bit order plus a bug. */
static unsigned long inf_bits(struct inf *z, int n)
{
    unsigned long v = 0;

    for (int i = 0; i < n; i++)
        v |= (unsigned long)inf_bit(z) << i;
    return v;
}

static void inf_align(struct inf *z)
{
    z->nbits = 0;
    z->bitbuf = 0;
}

/* --- Huffman tables ----------------------------------------------------
 * Canonical codes: for each length the codes are consecutive, so a table is a
 * count per length plus the symbols ordered by length then by value. The
 * decoder walks lengths, pulling one bit at a time, and the moment the code it
 * has falls inside a length's range it knows the symbol — no 32768-entry
 * lookup, which is the whole reason this fits in a freestanding program. */
static int inf_build(struct inf_huff *h, const unsigned char *lens, int nsym)
{
    unsigned int slot[INFLATE_MAXBITS + 2];
    long left = 1;
    int i, len;

    for (len = 0; len <= INFLATE_MAXBITS; len++)
        h->count[len] = 0;
    for (i = 0; i < nsym; i++)
        h->count[lens[i]]++;
    h->count[0] = 0;                       /* symbols with no code are absent */

    /* Kraft check: two bits of length 1, four of length 2, and so on. Going
     * negative means the table claims more codes than exist, which is not a
     * stream we can decode honestly. Coming up short is an incomplete code:
     * legal in the spec, and harmless here because inf_symbol() reports the
     * unknown code instead of inventing one. */
    for (len = 1; len <= INFLATE_MAXBITS; len++) {
        left <<= 1;
        left -= (long)h->count[len];
        if (left < 0)
            return INFLATE_ECORRUPT;
    }

    slot[1] = 0;
    for (len = 2; len <= INFLATE_MAXBITS + 1; len++)
        slot[len] = slot[len - 1] + h->count[len - 1];
    for (i = 0; i < nsym; i++) {
        if (lens[i])
            h->symbol[slot[lens[i]]++] = (unsigned short)i;
    }
    if (slot[INFLATE_MAXBITS + 1] > INFLATE_CODES)
        return INFLATE_ECORRUPT;            /* cannot happen; the bound is kept */
    return INFLATE_OK;
}

static int inf_symbol(struct inf *z, const struct inf_huff *h)
{
    unsigned int code = 0, first = 0, index = 0;

    for (int len = 1; len <= INFLATE_MAXBITS; len++) {
        code = (code << 1) | (unsigned int)inf_bit(z);
        if (code >= first && code - first < (unsigned int)h->count[len])
            return (int)h->symbol[index + (code - first)];
        index += (unsigned int)h->count[len];
        first = (first + (unsigned int)h->count[len]) << 1;
    }
    z->fail = 1;
    return -1;
}

/* --- the two canned tables --------------------------------------------
 * Built from their code lengths rather than hard-coded as code values, so the
 * canonical machinery is the only place a code can be wrong. */
static void inf_fixed(struct inf_huff *lit, struct inf_huff *dist)
{
    unsigned char lens[INFLATE_CODES];
    unsigned char dlens[INFLATE_DIST];

    for (int i = 0; i < INFLATE_CODES; i++)
        lens[i] = (i < 144) ? 8 : (i < 256) ? 9 : (i < 280) ? 7 : 8;
    for (int i = 0; i < INFLATE_DIST; i++)
        dlens[i] = 5;
    inf_build(lit, lens, INFLATE_CODES);
    inf_build(dist, dlens, INFLATE_DIST);
}

/* Code-length code, then the literal and distance lengths it encodes, with the
 * three repeat opcodes the spec defines. The two alphabets arrive as one run of
 * hlit + hdist lengths, so `work` holds both and the distance table is simply
 * the tail of it — no second buffer, and no chance of mixing up where one ends. */
static int inf_dynamic(struct inf *z, struct inf_huff *lit,
                       struct inf_huff *dist, unsigned char *work)
{
    unsigned char clens[19];
    struct inf_huff cl;
    unsigned int hlit = (unsigned int)inf_bits(z, 5) + 257U;
    unsigned int hdist = (unsigned int)inf_bits(z, 5) + 1U;
    unsigned int hclen = (unsigned int)inf_bits(z, 4) + 4U;
    unsigned int i = 0;
    int ret;

    if (hlit > INFLATE_CODES || hdist > INFLATE_DIST) {
        z->fail = 1;
        return INFLATE_ECORRUPT;
    }
    for (i = 0; i < 19; i++)
        clens[i] = 0;
    for (i = 0; i < hclen; i++)
        clens[inf_clorder[i]] = (unsigned char)inf_bits(z, 3);
    ret = inf_build(&cl, clens, 19);
    if (ret != INFLATE_OK)
        return ret;

    i = 0;
    while (i < hlit + hdist) {
        int sym = inf_symbol(z, &cl);

        if (sym < 0)
            return INFLATE_ECORRUPT;
        if (sym < 16) {
            work[i++] = (unsigned char)sym;
        } else if (sym == 16) {                    /* repeat the previous value */
            unsigned int n;
            if (i == 0) {
                z->fail = 1;
                return INFLATE_ECORRUPT;
            }
            n = (unsigned int)inf_bits(z, 2) + 3U;
            if (i + n > hlit + hdist) {
                z->fail = 1;
                return INFLATE_ECORRUPT;
            }
            while (n--) {
                work[i] = work[i - 1];
                i++;
            }
        } else if (sym == 17 || sym == 18) {       /* run of zeroes */
            unsigned int n = (unsigned int)inf_bits(z, sym == 17 ? 3 : 7)
                             + (sym == 17 ? 3U : 11U);
            if (i + n > hlit + hdist) {
                z->fail = 1;
                return INFLATE_ECORRUPT;
            }
            while (n--)
                work[i++] = 0;
        } else {
            z->fail = 1;
            return INFLATE_ECORRUPT;
        }
    }
    if (z->fail)
        return INFLATE_EINPUT;

    ret = inf_build(lit, work, (int)hlit);
    if (ret != INFLATE_OK)
        return ret;
    return inf_build(dist, work + hlit, (int)hdist);
}

/* One compressed block into `out`, which already holds `have` bytes. */
static int inf_block(struct inf *z, struct inf_huff *lit, struct inf_huff *dist,
                     unsigned char *out, unsigned long cap, unsigned long *havep)
{
    unsigned long have = *havep;

    for (;;) {
        int sym = inf_symbol(z, lit);

        if (sym < 0)
            return INFLATE_ECORRUPT;
        if (sym < 256) {
            if (have >= cap)
                return INFLATE_EOVER;
            out[have++] = (unsigned char)sym;
            continue;
        }
        if (sym == 256) {                           /* end of block */
            *havep = have;
            return z->fail ? INFLATE_EINPUT : INFLATE_OK;
        }
        if (sym > 285)
            return INFLATE_ECORRUPT;

        {
            unsigned int li = (unsigned int)sym - 257U;
            unsigned long len = inf_lbase[li] + inf_bits(z, inf_lbits[li]);
            int dsym = inf_symbol(z, dist);
            unsigned long dist_v;
            unsigned long src;

            if (dsym < 0 || dsym > 29)
                return INFLATE_ECORRUPT;
            dist_v = inf_dbase[dsym] + inf_bits(z, inf_dbits[dsym]);
            if (dist_v > have)
                return INFLATE_EDIST;               /* match before the output */
            src = have - dist_v;
            while (len--) {
                if (have >= cap)
                    return INFLATE_EOVER;
                out[have++] = out[src++];           /* overlapping copy, by design */
            }
        }
    }
}

/* A whole deflate stream: BFINAL/BTYPE, stored runs, then compressed blocks. */
static int inf_stream(struct inf *z, unsigned char *out, unsigned long cap,
                      unsigned long *produced)
{
    struct inf_huff lit, dist;
    unsigned char work[INFLATE_CODES + INFLATE_DIST];
    unsigned long have = 0;
    int final = 0;

    while (!final) {
        int type;
        int ret;

        final = (int)inf_bits(z, 1);
        type = (int)inf_bits(z, 2);
        if (z->fail)
            return INFLATE_EINPUT;

        if (type == 0) {                        /* stored: LEN, NLEN, raw bytes */
            unsigned long n;

            inf_align(z);
            if (z->inpos + 4 > z->inlen)
                return INFLATE_EINPUT;
            n = (unsigned long)z->in[z->inpos] |
                ((unsigned long)z->in[z->inpos + 1] << 8);
            {
                unsigned long nlen = (unsigned long)z->in[z->inpos + 2] |
                                     ((unsigned long)z->in[z->inpos + 3] << 8);
                if (nlen != ((~n) & 0xFFFFUL))
                    return INFLATE_ECORRUPT;
            }
            z->inpos += 4;
            if (n > z->inlen - z->inpos)
                return INFLATE_EINPUT;
            if (have + n > cap)
                return INFLATE_EOVER;
            for (unsigned long i = 0; i < n; i++)
                out[have + i] = z->in[z->inpos + i];
            z->inpos += n;
            have += n;
            continue;
        }

        if (type == 1) {
            inf_fixed(&lit, &dist);
        } else if (type == 2) {
            ret = inf_dynamic(z, &lit, &dist, work);
            if (ret != INFLATE_OK)
                return ret;
        } else {
            return INFLATE_ECORRUPT;            /* BTYPE 11 is reserved */
        }

        ret = inf_block(z, &lit, &dist, out, cap, &have);
        if (ret != INFLATE_OK)
            return ret;
    }
    if (z->fail)
        return INFLATE_EINPUT;
    *produced = have;
    return INFLATE_OK;
}

/* Entry point. `inlen` is the compressed byte count the archive header claims —
 * trusting a longer run than the buffer holds is how a decoder reads its own
 * stack. Returns INFLATE_OK with *produced set, or an error and no promise. */
static int inflate_bytes(const unsigned char *in, unsigned long inlen,
                         unsigned char *out, unsigned long cap,
                         unsigned long *produced)
{
    struct inf z;

    z.in = in;
    z.inlen = inlen;
    z.inpos = 0;
    z.bitbuf = 0;
    z.nbits = 0;
    z.fail = 0;
    *produced = 0;
    if (inlen == 0)
        return INFLATE_EINPUT;
    return inf_stream(&z, out, cap, produced);
}

#endif
