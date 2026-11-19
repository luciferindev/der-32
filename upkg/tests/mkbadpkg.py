#!/usr/bin/env python3
"""
Forge deliberately malformed or malicious .upkg archives for upkg's
rejection tests.  `upkg create' refuses to produce these, so they are built
directly with tarfile.

usage: mkbadpkg.py KIND OUTFILE
"""
import hashlib
import io
import sys
import tarfile

KIND, OUT = sys.argv[1], sys.argv[2]

PKGINFO = "format 1\nname evil\nversion 1.0\nrelease 1\narch {arch}\n" \
          "description malicious test package\n"


def sha(b):
    return hashlib.sha256(b).hexdigest()


def add(tf, name, data=b"", typ=tarfile.REGTYPE, mode=0o644, link=""):
    ti = tarfile.TarInfo(name)
    ti.type = typ
    ti.mode = mode
    ti.uid = ti.gid = 0
    ti.mtime = 0
    if typ == tarfile.REGTYPE:
        ti.size = len(data)
        tf.addfile(ti, io.BytesIO(data))
    else:
        ti.linkname = link
        tf.addfile(ti)


def build(entries, manifest, arch="amd64"):
    with tarfile.open(OUT, "w:xz", format=tarfile.PAX_FORMAT) as tf:
        add(tf, "+PKGINFO", PKGINFO.format(arch=arch).encode())
        add(tf, "+MANIFEST", manifest.encode())
        for e in entries:
            add(tf, *e)


payload = b"#!/bin/sh\necho pwned\n"
s = sha(payload)
L = len(payload)
base_dirs = "d\t0755\tusr\nd\t0755\tusr/local\nd\t0755\tusr/local/bin\n"
dir_entries = [("usr", b"", tarfile.DIRTYPE, 0o755),
               ("usr/local", b"", tarfile.DIRTYPE, 0o755),
               ("usr/local/bin", b"", tarfile.DIRTYPE, 0o755)]

if KIND == "traversal":
    p = "usr/local/../../../../tmp/upkg-evil"
    build(dir_entries + [(p, payload)],
          base_dirs + f"f\t0755\t{L}\t{s}\t{p}\n")
elif KIND == "absolute":
    p = "/tmp/upkg-evil-abs"
    build([(p, payload)], f"f\t0755\t{L}\t{s}\t{p}\n")
elif KIND == "outside-prefix":
    build([("etc", b"", tarfile.DIRTYPE, 0o755), ("etc/evil.conf", payload)],
          f"d\t0755\tetc\nf\t0644\t{L}\t{s}\tetc/evil.conf\n")
elif KIND == "symlink-escape":
    build(dir_entries + [("usr/local/bin/passwd", b"", tarfile.SYMTYPE, 0o777,
                          "../../../etc/passwd")],
          base_dirs + "l\t../../../etc/passwd\tusr/local/bin/passwd\n")
elif KIND == "symlink-absolute":
    build(dir_entries + [("usr/local/bin/sh", b"", tarfile.SYMTYPE, 0o777,
                          "/bin/sh")],
          base_dirs + "l\t/bin/sh\tusr/local/bin/sh\n")
elif KIND == "bad-checksum":
    build(dir_entries + [("usr/local/bin/evil", payload, tarfile.REGTYPE, 0o755)],
          base_dirs + f"f\t0755\t{L}\t{'0' * 64}\tusr/local/bin/evil\n")
elif KIND == "setuid":
    build(dir_entries + [("usr/local/bin/evil", payload, tarfile.REGTYPE, 0o4755)],
          base_dirs + f"f\t4755\t{L}\t{s}\tusr/local/bin/evil\n")
elif KIND == "unlisted-member":
    build(dir_entries + [("usr/local/bin/evil", payload, tarfile.REGTYPE, 0o755),
                         ("usr/local/bin/hidden", payload, tarfile.REGTYPE, 0o755)],
          base_dirs + f"f\t0755\t{L}\t{s}\tusr/local/bin/evil\n")
elif KIND == "wrong-arch":
    build(dir_entries + [("usr/local/bin/evil", payload, tarfile.REGTYPE, 0o755)],
          base_dirs + f"f\t0755\t{L}\t{s}\tusr/local/bin/evil\n", arch="sparc64")
elif KIND == "bad-metadata":
    with tarfile.open(OUT, "w:xz", format=tarfile.PAX_FORMAT) as tf:
        add(tf, "+PKGINFO", b"format 1\nname ../evil\nversion 1.0\narch amd64\n")
        add(tf, "+MANIFEST", b"")
elif KIND == "not-a-package":
    with tarfile.open(OUT, "w:xz", format=tarfile.PAX_FORMAT) as tf:
        add(tf, "README", b"hello")
else:
    sys.exit("unknown kind " + KIND)
