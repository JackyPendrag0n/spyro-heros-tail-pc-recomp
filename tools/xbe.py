#!/usr/bin/env python3
"""Minimal XBE reader + reference finder for reverse-engineering work."""
import struct
import capstone

XOR_EP_RETAIL = 0xA8FC57AB
XOR_KT_RETAIL = 0x5B6D40B6


class XBE:
    def __init__(self, path):
        self.d = d = open(path, 'rb').read()
        self.base = struct.unpack_from('<I', d, 0x104)[0]
        n = struct.unpack_from('<I', d, 0x11C)[0]
        sh = struct.unpack_from('<I', d, 0x120)[0] - self.base
        self.sections = []
        for i in range(n):
            fl, va, vs, raw, rs, na = struct.unpack_from('<IIIIII', d, sh + i * 0x38)
            name = d[na - self.base:].split(b'\0')[0].decode()
            self.sections.append(dict(name=name, va=va, vsize=vs, raw=raw, rsize=rs, flags=fl))
        self.entry = struct.unpack_from('<I', d, 0x128)[0] ^ XOR_EP_RETAIL
        self.kthunk = struct.unpack_from('<I', d, 0x158)[0] ^ XOR_KT_RETAIL
        self.md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        self.md.detail = False

    def sec(self, name):
        return next(s for s in self.sections if s['name'] == name)

    def off(self, va):
        for s in self.sections:
            if s['va'] <= va < s['va'] + s['rsize']:
                return s['raw'] + va - s['va']
        if self.base <= va < self.base + 0x1000:
            return va - self.base
        return None

    def va_of_off(self, off):
        for s in self.sections:
            if s['raw'] <= off < s['raw'] + s['rsize']:
                return s['va'] + off - s['raw']
        return None

    def read(self, va, n):
        o = self.off(va)
        return self.d[o:o + n]

    def u32(self, va):
        return struct.unpack('<I', self.read(va, 4))[0]

    def cstr(self, va, maxlen=512):
        return self.read(va, maxlen).split(b'\0')[0].decode('latin1')

    def find_string(self, s):
        """All VAs where the exact NUL-terminated string s starts."""
        b = s.encode('latin1') + b'\0'
        out, i = [], 0
        while True:
            i = self.d.find(b, i)
            if i < 0:
                return out
            if i == 0 or True:
                va = self.va_of_off(i)
                if va is not None:
                    out.append(va)
            i += 1

    def code_refs(self, target):
        """Offsets in .text whose 4-byte immediate equals target."""
        t = self.sec('.text')
        blob = self.d[t['raw']:t['raw'] + t['rsize']]
        pat = struct.pack('<I', target)
        out, i = [], 0
        while True:
            i = blob.find(pat, i)
            if i < 0:
                return out
            out.append(t['va'] + i)
            i += 1

    def disasm(self, va, n=40):
        code = self.read(va, n * 8)
        res = []
        for ins in self.md.disasm(code, va):
            res.append(ins)
            if len(res) >= n:
                break
        return res

    def func_start(self, va, maxback=0x2000):
        """Heuristic: walk back to a preceding 'push ebp; mov ebp,esp' or int3/ret padding."""
        o = self.off(va)
        d = self.d
        for k in range(o, o - maxback, -1):
            if d[k - 1] in (0xCC, 0xC3, 0x90) and d[k:k + 3] in (b'\x55\x8b\xec',):
                return self.va_of_off(k)
            if d[k - 1] in (0xCC,) and d[k] not in (0xCC,):
                return self.va_of_off(k)
        return None


def rel_refs(x, target, secs=('.text', 'D3D', 'D3DX', 'XGRPH', 'DSOUND', 'XMV', 'XPP')):
    """Sites of E8/E9 rel32 call/jmp whose destination == target (all code sections)."""
    out = []
    for s in x.sections:
        if s['name'] not in secs:
            continue
        blob = x.d[s['raw']:s['raw'] + s['rsize']]
        for i in range(len(blob) - 5):
            if blob[i] in (0xE8, 0xE9):
                va = s['va'] + i
                if (va + 5 + struct.unpack_from('<i', blob, i + 1)[0]) & 0xffffffff == target:
                    out.append(va)
    return out


_ct_cache = {}
def call_targets(x):
    """Sorted list of E8 rel32 call targets inside .text (function entry candidates)."""
    if id(x) in _ct_cache: return _ct_cache[id(x)]
    s = x.sec('.text'); blob = x.d[s['raw']:s['raw'] + s['rsize']]
    lo, hi = s['va'], s['va'] + s['rsize']
    t = set()
    for i in range(len(blob) - 5):
        if blob[i] == 0xE8:
            dst = (s['va'] + i + 5 + struct.unpack_from('<i', blob, i + 1)[0]) & 0xffffffff
            if lo <= dst < hi: t.add(dst)
    r = sorted(t); _ct_cache[id(x)] = r
    return r

def func_of(x, va):
    import bisect
    ct = call_targets(x)
    i = bisect.bisect_right(ct, va) - 1
    return ct[i] if i >= 0 else None

def dis(x, start, end):
    out = []
    for c in x.md.disasm(x.read(start, end - start), start):
        out.append(f'{c.address:x}: {c.mnemonic} {c.op_str}')
    return '\n'.join(out)
