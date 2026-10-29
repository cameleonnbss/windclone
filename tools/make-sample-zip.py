#!/usr/bin/env python3
"""WindClone — build the ZIP specimen /bin/unzip unpacks in the fileops-zip test.

Why a generator and a committed artifact together: the test reads the archive
back with Python's own zipfile to derive every expectation (which members are
STORED and so must come out byte-for-byte, which are DEFLATED and so must be
refused), so the file on disk is the single source of truth — but "on disk" has
to mean "reproducible", not "something I once typed into a hex editor". This
script regenerates rootfs/src/sample.zip deterministically.

Two STORED members (readme.txt, notes.md) carry LF endings on purpose: a ZIP
entry is raw bytes, not text, and the same two bytes must appear in the archive,
in the store after unzip, and in the digest the test recomputes. One DEFLATED
member (packed.dat) exists so the refusal path is proven: this userland has no
inflate, and a deflated entry must be named and skipped, never written out as a
corrupt file that claims to be the original.

The members are written with explicit sizes into the local headers (Python's
zipfile does this for a seekable output file rather than a trailing data
descriptor), which is exactly the shape /bin/unzip's sequential local-header
walk reads. SPDX-License-Identifier: MIT
"""
import zipfile

# ZipInfo.compress_type: ZIP_STORED=0, ZIP_DEFLATED=8. Fixed timestamps keep the
# archive byte-stable across runs so a rebuild does not move the digest by
# accident.
STAMP = (2026, 1, 1, 0, 0, 0)

MEMBERS = [
    # (name, bytes, compress_type)
    ("readme.txt", b"Hello from WindClone. This entry is stored, not compressed.\n",
     zipfile.ZIP_STORED),
    ("notes.md",
     b"# notes\n\nA second stored member, to prove two files come out of "
     b"one archive.\n",
     zipfile.ZIP_STORED),
    # Repetitive so deflate really shrinks it; the point is the method, not the
    # size, but a member that did not compress would invite a STORED fallback.
    ("packed.dat", (b"packme packme packme packme packme packme\n" * 16),
     zipfile.ZIP_DEFLATED),
]


def build(path):
    with zipfile.ZipFile(path, "w") as zf:
        for name, data, ctype in MEMBERS:
            info = zipfile.ZipInfo(name, date_time=STAMP)
            info.compress_type = ctype
            info.external_attr = 0o644 << 16
            zf.writestr(info, data)


if __name__ == "__main__":
    import os
    out = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                       "rootfs", "src", "sample.zip")
    build(out)
    print("wrote %s (%d bytes)" % (out, os.path.getsize(out)))
