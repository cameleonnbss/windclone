#!/usr/bin/env python3
"""WindClone — build the 24-bit BMP specimen /bin/imgview decodes in the `img` test.

The same rule as make-sample-zip.py: the file on disk is the single source of
truth, and the test reads it back to derive every expectation, but it has to be
reproducible. This regenerates rootfs/src/logo.bmp byte-for-byte on every run.

Why 24-bit BI_RGB: it is the one bitmap a libc-less ring-3 program can decode
without a second engine — the pixels are three bytes each, in the file, no
run-length and no deflate to reimplement first. The reader still has to get the
real parts of the format right: the little-endian header fields, the four-byte
row padding, the bottom-up row order, and the BGR byte order that becomes RGB on
the way to the window. Those are the bugs a bitmap decoder actually has.

The pattern is a gradient with a couple of marked pixels, so a row that came out
in the wrong order, or a stride that ignored the padding, changes the digest the
test recomputes — a silent transpose or a swapped red/blue channel cannot sneak
through a flat image. SPDX-License-Identifier: MIT
"""
import struct

W, H = 48, 32


def pixel(x, y):
    """The one definition of the picture, shared by the writer here and the
    reader's expectation in the test. Returns (r, g, b), each 0..255."""
    if (x, y) in ((0, 0), (W - 1, 0), (0, H - 1), (W - 1, H - 1)):
        return (255, 255, 255)        # the four corners mark the axes
    if y == H // 2:
        return (255, 0, 0)            # a red mid-row, so a bottom-up flip is visible
    return ((x * 5) & 0xFF, (y * 7) & 0xFF, (x + y) & 0xFF)


def build(path):
    row_bytes = W * 3
    stride = (row_bytes + 3) & ~3     # BMP rows are padded to a multiple of 4
    pad = stride - row_bytes
    pixel_data = bytearray()
    # Stored bottom-up: the last row of the picture is the first row in the file.
    for y in range(H - 1, -1, -1):
        for x in range(W):
            r, g, b = pixel(x, y)
            pixel_data += bytes((b, g, r))   # BMP is BGR
        pixel_data += b"\x00" * pad

    file_size = 14 + 40 + len(pixel_data)
    with open(path, "wb") as f:
        f.write(b"BM")
        f.write(struct.pack("<I", file_size))   # total size
        f.write(struct.pack("<HH", 0, 0))       # reserved
        f.write(struct.pack("<I", 14 + 40))     # offset to pixel data
        f.write(struct.pack("<I", 40))          # BITMAPINFOHEADER size
        f.write(struct.pack("<ii", W, H))       # width, signed height (bottom-up)
        f.write(struct.pack("<HH", 1, 24))      # planes, bits per pixel
        f.write(struct.pack("<I", 0))           # BI_RGB (no compression)
        f.write(struct.pack("<I", len(pixel_data)))
        f.write(struct.pack("<ii", 0, 0))       # resolution, unused
        f.write(struct.pack("<II", 0, 0))       # palette counts, none for 24-bit
        f.write(pixel_data)


if __name__ == "__main__":
    import os
    out = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                       "rootfs", "src", "logo.bmp")
    build(out)
    print("wrote %s (%d bytes)" % (out, os.path.getsize(out)))
