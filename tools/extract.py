#!/usr/bin/env python3
"""Extract files from EngineX Filelist.00N archives, optionally from split parts.

usage: extract.py INDEX.json ARCHIVE_NUM OUTDIR PART [PART ...] [--slack] [--list]

Parts are treated as one contiguous archive in the order given (sort them by
name: Filelist.000.part_aa, _ab, ...). A file is extracted only if every byte
of it is covered by the parts supplied, so you can run this after each upload.
--slack also dumps the unused reserved space after each file (stale data left
by in-place rebuilds) when it contains anything other than zeros.
"""
import json, os, sys, collections


class Concat:
    def __init__(self, paths):
        self.parts = []
        pos = 0
        for p in paths:
            n = os.path.getsize(p)
            self.parts.append((pos, n, p))
            pos += n
        self.size = pos

    def covers(self, off, n):
        return off + n <= self.size

    def read(self, off, n):
        out = bytearray()
        for start, size, path in self.parts:
            if n == 0:
                break
            if off < start + size and off + n > start:
                a = max(off, start)
                b = min(off + n, start + size)
                with open(path, 'rb') as f:
                    f.seek(a - start)
                    out += f.read(b - a)
        return bytes(out)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    flags = {a for a in sys.argv[1:] if a.startswith('--')}
    index, arc, outdir, parts = args[0], int(args[1]), args[2], args[3:]
    E = json.load(open(index))['entries']
    es = sorted([e for e in E if e['locs'][0][1] == arc], key=lambda e: e['locs'][0][0])
    src = Concat(parts)
    done, pending, magics = [], [], collections.Counter()
    for i, e in enumerate(es):
        off, ln = e['locs'][0][0], e['length']
        rel = e['name'].split('_bin_xb\\', 1)[1].replace('\\', '/')
        if not src.covers(off, ln):
            pending.append(rel)
            continue
        dst = os.path.join(outdir, rel)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        if not (os.path.exists(dst) and os.path.getsize(dst) == ln):
            with open(dst, 'wb') as f:
                # stream in 8 MB chunks
                p = 0
                while p < ln:
                    k = min(8 << 20, ln - p)
                    f.write(src.read(off + p, k))
                    p += k
        magics[(rel.rsplit('.', 1)[1], src.read(off, 4))] += 1
        done.append(rel)
        if '--slack' in flags:
            end = off + ln
            nxt = es[i + 1]['locs'][0][0] if i + 1 < len(es) else src.size
            gap = nxt - ((end + 0x7FF) & ~0x7FF)
            if gap > 0x10000 and src.covers(end, nxt - end):
                blob = src.read(end, nxt - end)
                if blob.strip(b'\0'):
                    sd = os.path.join(outdir, '_slack')
                    os.makedirs(sd, exist_ok=True)
                    name = os.path.basename(rel) + '.slack_%x' % end
                    open(os.path.join(sd, name), 'wb').write(blob)
                    print('slack after %-28s %.2f MB non-zero -> _slack/%s' % (rel, len(blob) / 1e6, name))
    print('archive %d: %d/%d files extracted (%.1f MB of parts supplied, archive needs %.1f MB)' % (
        arc, len(done), len(es), src.size / 1e6,
        (es[-1]['locs'][0][0] + es[-1]['length']) / 1e6))
    print('header magics:', dict(magics))
    if pending and '--list' in flags:
        print('pending:', ' '.join(pending))
    elif pending:
        print('%d pending, first: %s' % (len(pending), pending[0]))


if __name__ == '__main__':
    main()
