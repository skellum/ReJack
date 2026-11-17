#!/usr/bin/env python3
"""Minimal read-only FAT16 extractor (with long filename support). Usage: fat16x.py IMG OUTDIR"""
import struct, sys, os
img, out = sys.argv[1], sys.argv[2]
d = open(img, 'rb').read()
bps, spc, rsv, nfat, nroot, _, _, spf = struct.unpack('<HBHBHHBH', d[11:24])
fat = d[rsv*bps:(rsv+spf)*bps]
rootoff = (rsv + nfat*spf) * bps
dataoff = rootoff + nroot*32
csz = bps*spc
def chain(c):
    while 2 <= c < 0xFFF8:
        yield c; c = struct.unpack('<H', fat[c*2:c*2+2])[0]
def readc(c, size=None):
    b = b''.join(d[dataoff+(x-2)*csz:dataoff+(x-1)*csz] for x in chain(c))
    return b if size is None else b[:size]
def entries(raw):
    lfn = []
    for i in range(0, len(raw), 32):
        e = raw[i:i+32]
        if e[0] == 0: break
        if e[0] == 0xE5: lfn = []; continue
        if e[11] == 0x0F:
            lfn.insert(0, (e[1:11]+e[14:26]+e[28:32]).decode('utf-16le', 'ignore').split('\0')[0]); continue
        name = ''.join(lfn) or (e[0:8].decode('latin1').rstrip() + ('.'+e[8:11].decode('latin1').rstrip() if e[8:11].strip() else ''))
        lfn = []
        if e[11] & 0x08: continue
        yield name, e[11], struct.unpack('<H', e[26:28])[0], struct.unpack('<I', e[28:32])[0]
def walk(raw, path):
    for name, attr, clus, size in entries(raw):
        if name in ('.', '..'): continue
        p = os.path.join(path, name)
        if attr & 0x10:
            os.makedirs(p, exist_ok=True); walk(readc(clus), p)
        else:
            open(p, 'wb').write(readc(clus, size) if clus else b'')
            print(f'{size:10d}  {os.path.relpath(p, out)}')
os.makedirs(out, exist_ok=True)
walk(d[rootoff:dataoff], out)
