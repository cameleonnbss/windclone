#!/usr/bin/env python3
"""WindClone — build a real, DEFLATE-compressed wheel as a test specimen.

/bin/unzip can decode a deflated member since v0.1.35 (src/user/inflate.h), so
the wall a `.whl` still hits is the *name*: every entry of a wheel lives under a
directory (`demo/__init__.py`, `demo-1.0.dist-info/METADATA`), and unzip writes
flat names only — a program must not take its output path from an archive. That
refusal is the wall this specimen exists to knock down once extraction learns
subdirectories: a `.whl` you can actually install is a ZIP whose entries are both
compressed *and* nested, produced here by the reference implementation rather
than by anything in this repository.

The file this writes is *not* a stub and not a hand-made guess: it is written by
Python's own zipfile with ZIP_DEFLATED, which is the same LZ77+Huffman output
every real wheel on PyPI uses, produced by the reference implementation rather
than by anything in this repository. The test can therefore compare the guest's
extracted bytes against `zipfile`'s own reading of the same file, and a wrong
inflate shows up as a digest mismatch instead of as a plausible-looking page.

Keep it small on purpose. The kernel's store holds a file up to TMPFS_FILE_MAX
and unzip reads an archive up to UNZIP_MAX, so the specimen is a few hundred
bytes — that is a limit of this machine today, stated here rather than hidden,
and the numbers live in src/user/unzip.c.

SPDX-License-Identifier: MIT
"""
import zipfile
from pathlib import Path

OUT = Path(__file__).resolve().parent.parent.parent / "rootfs" / "src" / "demo-1.0-py3-none-any.whl"

METADATA = """Metadata-Version: 2.1
Name: demo
Version: 1.0
Summary: a deflated wheel WindClone can actually unpack
"""

WHEEL = """Wheel-Version: 1.0
Generator: make-sample-wheel.py
Root-Is-Purelib: true
Tag: py3-none-any
"""

RECORD = """demo-1.0.dist-info/METADATA,,
demo-1.0.dist-info/WHEEL,,
demo/__init__.py,,
"""

# The module is real Python the interpreter increment will run: no imports, no
# C extension, nothing that needs a stdlib WindClone does not have.
INIT = '''"""demo - the package WindClone unpacks in its tests."""


def greet(name):
    return "hello, " + name + "!"


def add(a, b):
    return a + b
'''


def main():
    OUT.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(OUT, "w", compression=zipfile.ZIP_DEFLATED) as z:
        z.writestr("demo-1.0.dist-info/METADATA", METADATA)
        z.writestr("demo-1.0.dist-info/WHEEL", WHEEL)
        z.writestr("demo/__init__.py", INIT)
        z.writestr("demo-1.0.dist-info/RECORD", RECORD)

    # Prove the specimen is what it claims: every member deflated, and readable
    # back through the reference implementation.
    with zipfile.ZipFile(OUT) as z:
        methods = {i.filename: i.compress_type for i in z.infolist()}
        assert set(methods.values()) == {zipfile.ZIP_DEFLATED}, methods
        assert z.read("demo/__init__.py") == INIT.encode()
        print(f"wrote {OUT.name}: {OUT.stat().st_size} bytes, "
              f"{len(methods)} deflated members")
        for i in z.infolist():
            print(f"  {i.filename}: {i.compress_size} -> {i.file_size} bytes")


if __name__ == "__main__":
    main()
