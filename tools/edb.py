#!/usr/bin/env python3
"""EngineX GEOM (.edb, version 240 = Spyro AHT Xbox) header/list reader.
Layout from eurochef (github.com/eurotools/eurochef) plus our own RE."""
import struct, sys

LISTS = ['section', 'refptr', 'entity', 'anim', 'animskin', 'animscript', 'map', 'animmode',
         'animset', 'particle', 'swoosh', 'spreadsheet', 'font']

class EDB:
    def __init__(self, path):
        self.d = d = open(path, 'rb').read()
        assert d[:4] == b'MOEG'
        self.hash, self.version = struct.unpack_from('<II', d, 4)
        self.lists = {}
        o = 0x54
        for name in LISTS:
            cnt, hcnt, rel = struct.unpack_from('<hhi', d, o)
            self.lists[name] = (cnt, o + 4 + rel)
            o += 8
        o += 8                                # spyro hack (u64)
        cnt, hcnt, rel = struct.unpack_from('<hhi', d, o)
        self.lists['texture'] = (cnt, o + 4 + rel)

    def u32(self, o): return struct.unpack_from('<I', self.d, o)[0]

    def anims(self):
        cnt, base = self.lists['anim']
        out = []
        for i in range(cnt):
            o = base + i * 0x20
            h, sec, dbg, addr, ptr, mdi, ptr2, size, skin = struct.unpack_from('<IHHIIIIII', self.d, o)
            out.append(dict(i=i, hash=h, section=sec, address=addr, motiondata=mdi, size=size, skin=skin, off=o))
        return out

if __name__ == '__main__':
    e = EDB(sys.argv[1])
    print('hash %08x version %d' % (e.hash, e.version))
    for k, (c, b) in e.lists.items(): print('  %-12s %4d @ %#x' % (k, c, b))
    for a in e.anims()[:60]:
        print('  anim %2d %08x sec %d addr %#x motion %#x size %#x skin %d' % (a['i'], a['hash'], a['section'], a['address'], a['motiondata'], a['size'], a['skin']))
