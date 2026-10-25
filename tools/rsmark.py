# rsmark.py FILE FROM TO [--apply] [--skip name,name] : find variable definitions (file scope, and
# function-local statics) in lines FROM..TO and mark them RS (per-player Red state).
import re, sys
f, a, b = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
apply = '--apply' in sys.argv
skip = set()
if '--skip' in sys.argv: skip = set(sys.argv[sys.argv.index('--skip') + 1].split(','))
L = open(f).read().split('\n')
TYPES = r'(?:unsigned |signed )?(?:int|u32|u8|u16|s16|s32|float|double|char|short|long|bone|quat|bclip|upd_fn|struct\s+\w+\s*(?:\{[^}]*\})?|struct\s*\{[^}]*\})'
# a declaration statement starting at line start or after ; { } within the line
pat = re.compile(r'(?:(?<=[;{}])\s*|^\s*)(static\s+)?((?:volatile\s+)?' + TYPES + r')(\s+\*?\s*)([A-Za-z_]\w*)\s*(\[[^\]]*\])*\s*([=;,])')
depth = 0
out = []
for i in range(len(L)):
    line = L[i]; n = i + 1
    st = line.strip()
    if a <= n <= b and not st.startswith(('#', '//', '/*', '*', 'extern', 'typedef', 'return')):
        new = line; off = 0
        for m in pat.finditer(line):
            static = m.group(1)
            pre = line[:m.start()]; d_here = depth + pre.count('{') - pre.count('}')
            if not static and not (d_here == 0 and line[0] not in ' \t'): continue
            if 'const' in line[max(0, m.start(2) - 12):m.start(2)]: continue
            name = m.group(4)
            if name in skip: continue
            if 'RS' in line[max(0, m.start(2) - 4):m.start(2)]: continue
            pos = m.start(2) + off
            new = new[:pos] + 'RS ' + new[pos:]; off += 3
            out.append((n, name, st[:110]))
        L[i] = new
    # brace depth (rough: ignores braces in strings/comments)
    s = re.sub(r'"([^"\\]|\\.)*"', '', line); s = re.sub(r"'([^'\\]|\\.)*'", '', s); s = re.sub(r'/\*.*?\*/', '', s); s = s.split('//')[0]
    depth += s.count('{') - s.count('}')
for n, name, st in out: print(n, name, '|', st)
if apply: open(f, 'w').write('\n'.join(L))
