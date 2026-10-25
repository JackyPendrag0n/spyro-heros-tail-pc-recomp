#!/usr/bin/env python3
"""EngineX (Eurocom) Filelist.bin parser for Spyro: A Hero's Tail (Xbox).

Format (version 7, little-endian), reverse-engineered from the retail files:

  0x00 u32  version            (7)
  0x04 u32  file size
  0x08 u32  entry count
  0x0C u16  ?                  (1)
  0x0E u16  highest archive index (2 -> Filelist.000..002)
  0x10 u32  name table offset, relative to 0x10
  0x14      entries, variable length:
              u32 length, u32 hash, u32 version, u32 flags, u32 nloc,
              nloc * { u32 offset, u32 archive_index }
  name table: count * u32 self-relative pointers (relative to the slot)
  names:      NUL-terminated, byte i of name k stored as
              (plain + 0xEA + k + i) & 0xFF
"""
import struct, sys, json


def decode_name(d, o, k):
    out = bytearray()
    i = 0
    while True:
        c = (d[o + i] - (0xEA + k + i)) & 0xFF
        if c == 0:
            return out.decode('latin1')
        out.append(c)
        i += 1


def parse_bin(path):
    d = open(path, 'rb').read()
    ver, size, n, unk, maxarc, nameoff = struct.unpack_from('<IIIHHI', d, 0)
    assert ver == 7 and size == len(d), (ver, size, len(d))
    entries = []
    o = 0x14
    for k in range(n):
        ln, h, v, fl, nloc = struct.unpack_from('<IIIII', d, o)
        o += 20
        locs = []
        for _ in range(nloc):
            off, arc = struct.unpack_from('<II', d, o)
            o += 8
            locs.append((off, arc))
        entries.append(dict(index=k, length=ln, hash=h, version=v, flags=fl, locs=locs))
    t = 0x10 + nameoff
    assert o == t, (hex(o), hex(t))
    for k in range(n):
        rel = struct.unpack_from('<I', d, t + 4 * k)[0]
        entries[k]['name'] = decode_name(d, t + 4 * k + rel, k)
    return dict(version=ver, unk=unk, max_archive=maxarc, entries=entries)


def parse_txt(path):
    rows = []
    for line in open(path, encoding='latin1'):
        if not line.strip():
            continue
        parts = [p.strip() for p in line.split(' : ')]
        name = parts[0]
        f = {}
        for p in parts[1:]:
            key, val = p.split(None, 1)
            f[key] = val.strip()
        off, arc = f['Loc'].split(':')
        rows.append(dict(name=name, length=int(f['Len']), version=int(f['Ver']),
                         hash=int(f['Hash'], 16), ts=int(f['Ts'], 16),
                         loc=(int(off, 16), int(arc))))
    return rows


if __name__ == '__main__':
    b = parse_bin(sys.argv[1])
    if len(sys.argv) > 2:
        rows = parse_txt(sys.argv[2])
        assert len(rows) == len(b['entries'])
        bad = 0
        for e, r in zip(b['entries'], rows):
            ok = (e['name'].lower() == r['name'].lower() and e['length'] == r['length']
                  and e['hash'] == r['hash'] and e['version'] == r['version']
                  and e['locs'][0] == r['loc'])
            if not ok:
                bad += 1
                print('MISMATCH', e, r)
            e['ts'] = r['ts']
        print(f'{len(rows)} entries, {bad} mismatches vs txt', file=sys.stderr)
    json.dump(b, sys.stdout, indent=1)
