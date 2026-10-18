#!/usr/bin/env python3
"""Find NVS partitions in a full flash dump and list string entries (values hidden unless --show)."""
import json
import struct
import sys

img = open(sys.argv[1], "rb").read()
show = "--show" in sys.argv
out_path = next((a.split("=", 1)[1] for a in sys.argv if a.startswith("--out=")), None)

parts = []
for i in range(0x8000, 0x8000 + 0xC00, 32):
    e = img[i:i + 32]
    if e[:2] != b"\xaa\x50":
        break
    typ, sub, off, size = e[2], e[3], *struct.unpack("<II", e[4:12])
    name = e[12:28].split(b"\0")[0].decode()
    parts.append((name, typ, sub, off, size))
    print(f"part {name:16} type={typ} sub={sub:#x} off={off:#x} size={size:#x}")

found = {}
for name, typ, sub, off, size in parts:
    if not (typ == 1 and sub == 2):
        continue
    ns = {}
    for page in range(off, off + size, 4096):
        hdr = img[page:page + 32]
        if struct.unpack("<I", hdr[:4])[0] == 0xFFFFFFFF:
            continue
        bitmap = img[page + 32:page + 64]
        i = 0
        while i < 126:
            state = (bitmap[i // 4] >> ((i % 4) * 2)) & 3
            ent = img[page + 64 + i * 32: page + 96 + i * 32]
            if state != 2:  # 2 = written
                i += 1
                continue
            nsi, etype, span = ent[0], ent[1], ent[2]
            key = ent[8:24].split(b"\0")[0].decode(errors="replace")
            if nsi == 0 and etype == 0x01:
                ns[ent[24]] = key
            elif etype == 0x21:  # string
                ln = struct.unpack("<H", ent[24:26])[0]
                data = img[page + 96 + i * 32: page + 96 + i * 32 + ln].rstrip(b"\0")
                val = data.decode(errors="replace")
                k = f"{name}:{ns.get(nsi, nsi)}:{key}"
                found[k] = val
                print(f"  {k:40} str len={len(val)}" + (f" = {val!r}" if show else ""))
            elif etype == 0x42:  # blob
                k = f"{name}:{ns.get(nsi, nsi)}:{key}"
                print(f"  {k:40} blob")
            i += max(span, 1)

if out_path:
    json.dump(found, open(out_path, "w"))
