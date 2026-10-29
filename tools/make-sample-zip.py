#!/usr/bin/env python3
"""WindClone — build the ZIP specimen /bin/unzip unpacks, with every block type.

Why a generator and a committed artifact together: the test reads the archive
back with Python's own zipfile to derive every expectation (each member's bytes
and compression method), so the file on disk is the single source of truth — but
"on disk" has to mean "reproducible", not "something I once typed into a hex
editor". This script regenerates rootfs/src/sample.zip deterministically.

Why the archive is assembled here instead of through zipfile. zipfile can only
write what zlib chooses to emit, and zlib emits dynamic-Huffman blocks (BTYPE
10) plus, for data that will not compress, stored blocks. That leaves the
fixed-Huffman block (BTYPE 01) untested, and an untested branch in a decoder is
not a supported feature. So the container is written by hand — local file
headers, central directory, EOCD, exactly as RFC 1950 Appendix B / the ZIP spec
requires — and three of the members carry raw deflate streams this script builds
bit by bit:

  packed.dat  dynamic Huffman + LZ77 matches, produced by zlib itself, which is
              what a real archive from a real tool looks like.
  fixed.bl    a hand-written BTYPE=01 block: literals, then back-references that
              rebuild the rest of the file. This is the only way the canned
              tables in inflate.h get exercised.
  stored.bl   two hand-written BTYPE=00 blocks (LEN, NLEN, raw bytes) so the
              align-to-byte-boundary path and the BFINAL-between-blocks path
              are tested rather than assumed.

Every stream is checked against the reference implementation before it is
written: `zlib.decompress(stream, -15)` must return the member's intended bytes
exactly, and the finished archive is read back with `zipfile` and CRC-verified.
If this script produces a file, that file is valid by the reference decoder's
own judgement — so a guest digest that disagrees can only mean the guest is
wrong, never that the specimen was.

The members are small on purpose: /bin/unzip reads an archive up to UNZIP_MAX
(8 KiB) and the kernel's store caps a file at TMPFS_FILE_MAX (4 KiB). Those are
limits of this machine, stated here rather than hidden; the numbers live in
src/user/unzip.c.

SPDX-License-Identifier: MIT
"""
import os
import struct
import zipfile
import zlib

STAMP = (2026, 1, 1, 0, 0, 0)

# RFC 1951 3.2.5: length codes 257..285 and distance codes 0..29 as base plus
# extra bits. The two builders below look symbols up from these tables rather
# than hard-coding a code, so a typo here fails the zlib check instead of
# quietly producing an archive only this script can read.
LBASE = [3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51,
         59, 67, 83, 99, 115, 131, 163, 195, 227, 258]
LBITS = [0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4,
         4, 5, 5, 5, 5, 0]
DBASE = [1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385,
         513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577]
DBITS = [0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10,
         10, 11, 11, 12, 12, 13, 13]


def len_sym(n):
    """(symbol, extra value, extra bits) for a match of length n."""
    for i, base in enumerate(LBASE):
        if base <= n < base + (1 << LBITS[i]):
            return 257 + i, n - base, LBITS[i]
    raise ValueError("length %d is not encodable" % n)


def dist_sym(d):
    """(symbol, extra value, extra bits) for a back-reference of distance d."""
    for j, base in enumerate(DBASE):
        if base <= d < base + (1 << DBITS[j]):
            return j, d - base, DBITS[j]
    raise ValueError("distance %d is not encodable" % d)


class Bits(object):
    """A deflate bit stream. Bits fill each byte from the low end up.

    Two orders meet here and mixing them is the classic inflate bug: numbers
    (BFINAL, BTYPE, LEN, extra bits) go out low-bit-first, while Huffman codes
    are written high-bit-first. `num` and `code` say which is which; nothing
    else writes a bit.
    """

    def __init__(self):
        self.bits = []

    def num(self, value, n):
        for i in range(n):
            self.bits.append((value >> i) & 1)

    def code(self, value, n):
        for i in range(n - 1, -1, -1):
            self.bits.append((value >> i) & 1)

    def byte(self, b):
        self.num(b, 8)

    def align(self):
        while len(self.bits) % 8:
            self.bits.append(0)

    def out(self):
        self.align()
        data = bytearray()
        for i in range(0, len(self.bits), 8):
            v = 0
            for j in range(8):
                v |= self.bits[i + j] << j
            data.append(v)
        return bytes(data)


def lit_code_fixed(sym):
    """The canned (BTYPE=01) literal/length table, RFC 1951 3.2.6."""
    if sym <= 143:
        return 0x30 + sym, 8
    if sym <= 255:
        return 0x190 + sym - 144, 9
    if sym <= 279:
        return sym - 256, 7
    return 0xC0 + sym - 280, 8


def defl_fixed(data, unit):
    """One fixed-Huffman block: `unit` literals, then matches that repeat them.

    The head is copied byte for byte; the tail is rebuilt entirely from
    back-references, which is the part a decoder either does or does not: the
    overlap between a match and the bytes it refers to is where a naive
    memcpy-style copy corrupts the output.
    """
    w = Bits()
    w.num(1, 1)          # BFINAL - this is the stream's only block
    w.num(1, 2)          # BTYPE = 01, the canned tables
    for b in data[:unit]:
        v, n = lit_code_fixed(b)
        w.code(v, n)
    rest = len(data) - unit
    while rest > 0:
        n = min(unit, rest)
        sym, extra, ebits = len_sym(n)
        v, wbits = lit_code_fixed(sym)
        w.code(v, wbits)
        w.num(extra, ebits)
        dsym, dextra, debits = dist_sym(unit)
        w.code(dsym, 5)
        w.num(dextra, debits)
        rest -= n
    v, n = lit_code_fixed(256)       # end of block
    w.code(v, n)
    return w.out()


def defl_stored(chunks):
    """Consecutive BTYPE=00 blocks; the last one carries BFINAL."""
    w = Bits()
    for k, c in enumerate(chunks):
        w.num(1 if k == len(chunks) - 1 else 0, 1)
        w.num(0, 2)                  # BTYPE = 00, stored
        w.align()                    # a stored block starts on a byte boundary
        w.byte(len(c) & 0xFF)
        w.byte((len(c) >> 8) & 0xFF)
        w.byte((~len(c)) & 0xFF)
        w.byte(((~len(c)) >> 8) & 0xFF)
        for b in c:
            w.byte(b)
    return w.out()


def raw_deflate(data, level=6):
    """The stream zipfile itself would store for this member: wbits=-15."""
    co = zlib.compressobj(level, zlib.DEFLATED, -15)
    return co.compress(data) + co.flush()


def dos_time(date_time):
    y, mo, d, h, mi, s = date_time
    return ((h << 11) | (mi << 5) | (s // 2)) & 0xFFFF, \
           (((y - 1980) << 9) | (mo << 5) | d) & 0xFFFF


def build(path, members):
    """members: (name, raw deflate/stream bytes, original bytes, method)."""
    tmod, dmod = dos_time(STAMP)
    local, central, off = bytearray(), bytearray(), 0
    for name, blob, data, method in members:
        nb = name.encode()
        crc = zlib.crc32(data) & 0xFFFFFFFF
        local += struct.pack("<IHHHHHIIIHH", 0x04034b50, 20, 0, method,
                             tmod, dmod, crc, len(blob), len(data), len(nb), 0)
        local += nb
        local += blob
        central += struct.pack("<IHHHHHHIIIHHHHHII", 0x02014b50, 20, 20, 0,
                               method, tmod, dmod, crc, len(blob), len(data),
                               len(nb), 0, 0, 0, 0, 0o644 << 16, off)
        central += nb
        off += 30 + len(nb) + len(blob)
    end = len(local)
    local += central
    local += struct.pack("<IHHHHIIH", 0x06054b50, 0, 0, len(members),
                         len(members), len(central), end, 0)
    with open(path, "wb") as f:
        f.write(bytes(local))
    return len(local)


# --- the members ---------------------------------------------------------
README = b"Hello from WindClone. This entry is stored, not compressed.\n"
NOTES = (b"# notes\n\nA second stored member, to prove two files come out of "
         b"one archive.\n")
PACKED = b"packme packme packme packme packme packme\n" * 16
# 12-byte unit repeated four times: three of the four repeats must arrive as
# back-references, so nothing but a working match copy can rebuild them.
FIXED = b"windclone-12" * 4
STORED_A = bytes((i * 7 + 11) & 0xFF for i in range(90))
STORED_B = bytes((255 - i * 3) & 0xFF for i in range(64))
# v0.1.36: the member that used to be refused for carrying a '/' now unpacks, so
# the specimen writes it deflated - a compressed nested name, which is exactly
# what a wheel's demo/__init__.py is. Kept under the digest-line budget so the
# hex survives the 56-byte cap; the path is proven by a store read-back anyway.
INNER = (b"\"\"\"A nested member. A real wheel puts its modules under a package\n"
         b"directory, and since v0.1.36 /bin/unzip honours that name.\"\"\"\n"
         b"__version__ = \"1.0\"\n")
# The dangerous shape that stays refused: a name that climbs above the
# destination. This is not a folder record and not a legal member - it is the
# zip-slip, and the extractor must name it and skip it, never write it.
SLIP = b"this byte stream must never reach the store\n"


def streams():
    """(name, blob, original, method) with every blob validated by zlib."""
    packed = raw_deflate(PACKED)
    fixed = defl_fixed(FIXED, 12)
    stored = defl_stored([STORED_A, STORED_B])
    inner = raw_deflate(INNER)
    # The reference implementation is the judge: a hand-built stream that zlib
    # cannot read back byte for byte is a bug in this script, not a test case.
    assert zlib.decompress(packed, -15) == PACKED
    assert zlib.decompress(fixed, -15) == FIXED, zlib.decompress(fixed, -15)
    assert zlib.decompress(stored, -15) == STORED_A + STORED_B
    assert zlib.decompress(inner, -15) == INNER
    return [
        ("readme.txt", README, README, 0),
        ("notes.md", NOTES, NOTES, 0),
        ("packed.dat", packed, PACKED, 8),
        ("fixed.bl", fixed, FIXED, 8),
        ("stored.bl", stored, STORED_A + STORED_B, 8),
        ("demo/", b"", b"", 0),          # a folder record: skipped, not written
        ("demo/__init__.py", inner, INNER, 8),   # nested + deflated: unpacks
        ("../evil.txt", SLIP, SLIP, 0),  # zip-slip: refused, never written
    ]



def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out = os.path.join(root, "rootfs", "src", "sample.zip")
    size = build(out, streams())

    # Read the finished archive back the way the test does, and insist that the
    # container itself is valid: right methods, right bytes, CRCs intact.
    with zipfile.ZipFile(out) as zf:
        bad = zf.testzip()
        assert bad is None, "CRC failure in %s" % bad
        got = {i.filename: (i.compress_type, zf.read(i.filename))
               for i in zf.infolist()}
        for name, blob, data, method in streams():
            assert got[name] == (method, data), name
    print("wrote %s (%d bytes, %d members)" % (out, size, len(got)))
    for name, blob, data, method in streams():
        print("  %-14s method %d  %5d -> %5d bytes" % (name, method,
                                                       len(blob), len(data)))


if __name__ == "__main__":
    main()
