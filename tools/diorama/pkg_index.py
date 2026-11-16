#!/usr/bin/env python3
"""List FTL ftl.dat (PKG\\n v1.6) entries: offset size name."""
import struct, sys
f = open(sys.argv[1], 'rb'); d = f.read(1 << 20)
magic, hsz, esz, n, psz = struct.unpack('>4sHHII', d[:16])
assert magic == b'PKG\n', magic
paths_at = hsz + esz * n
paths = d[paths_at:paths_at + psz]
out = []
for i in range(n):
    h, fo, off, size, usize = struct.unpack('>IIIII', d[hsz + i * esz: hsz + i * esz + 20])
    po = fo & 0xFFFFFF; flags = fo >> 24
    name = paths[po:paths.index(b'\0', po)].decode()
    out.append((off, size, usize, flags, name))
for o in sorted(out): print(*o)
