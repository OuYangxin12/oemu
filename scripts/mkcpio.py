"""Write a newc cpio initramfs from files: mkcpio.py out.cpio name=path ...

Hand-rolled because the guest needs an archive and this box has no cpio(1).
Field order per the newc header: ino, mode, uid, gid, nlink, mtime, filesize,
devmajor, devminor, rdevmajor, rdevminor, namesize, chksum -- each 8 ASCII hex
digits. Names are NUL-terminated and padded so the data starts on a 4-byte
boundary; the archive ends with a TRAILER!!! record and 5120 NULs of slack.
"""
import sys

def record(name, payload, ino, mode=0o100755):
    fields = (ino, mode, 0, 0, 1, 0, len(payload), 0, 0, 0, 0, len(name) + 1, 0)
    hdr = b"070701" + b"".join(b"%08x" % f for f in fields)
    assert len(hdr) == 110, len(hdr)
    out = hdr + name + b"\0"
    out += b"\0" * ((4 - (len(out) % 4)) % 4)          # pad the name
    return out + payload + b"\0" * ((4 - (len(out) % 4)) % 4)

blob = b""
for i, spec in enumerate(sys.argv[2:], start=2):
    name, _, path = spec.partition("=")
    blob += record(name.encode(), open(path, "rb").read(), i)
blob += record(b"TRAILER!!!", b"", 0, 0o40000)
blob += b"\0" * ((512 - (len(blob) % 512)) % 512) + b"\0" * 5120
open(sys.argv[1], "wb").write(blob)
print(sys.argv[1], len(blob), "bytes")
