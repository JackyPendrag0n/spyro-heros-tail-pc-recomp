#!/usr/bin/env python3
"""Make a test .xmv (Xbox movie) from any video, for checking the port's movie playback
without the game's own movies. Needs the ffmpeg command line tool (with the wmv2 encoder).

  mkxmv.py in.mp4 out.xmv [--pcm] [--fps 30] [--size 640x480] [--secs N]
  mkxmv.py --test out.xmv        (6 s colour bars + 440 Hz tone)

Video: WMV2 (XMV's word-swapped bitstream + XMV extradata). Audio: Xbox ADPCM (IMA, 36-byte
blocks per channel, tag 0x69) by default, or 16-bit PCM with --pcm. One packet per second."""
import struct, subprocess, sys, tempfile, os

IMA_STEP = [7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88,
            97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724,
            796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026,
            4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500,
            20350, 22385, 24623, 27086, 29794, 32767]
IMA_IDX = [-1, -1, -1, -1, 2, 4, 6, 8]


def ima_blocks(pcm, ch):
    """IMA ADPCM, WAV layout, 36 bytes per channel per block (1 + 64 samples)."""
    st = [[0, 0] for _ in range(ch)]
    out = bytearray()
    n = len(pcm) // ch
    spb = 65
    for b0 in range(0, n, spb):
        blk = [pcm[(b0 + i) * ch + c] if b0 + i < n else 0 for i in range(spb) for c in range(ch)]
        hdr = bytearray(); data = [[] for _ in range(ch)]
        for c in range(ch):
            st[c][0] = blk[c]
            hdr += struct.pack('<hBB', st[c][0], st[c][1], 0)
            for i in range(1, spb):
                s = blk[i * ch + c]; pred, idx = st[c]; step = IMA_STEP[idx]
                diff = s - pred; code = 0
                if diff < 0: code = 8; diff = -diff
                d = step >> 3
                if diff >= step: code |= 4; diff -= step; d += step
                step >>= 1
                if diff >= step: code |= 2; diff -= step; d += step
                step >>= 1
                if diff >= step: code |= 1; d += step
                pred = pred - d if code & 8 else pred + d
                pred = max(-32768, min(32767, pred))
                idx = max(0, min(88, idx + IMA_IDX[code & 7]))
                st[c] = [pred, idx]; data[c].append(code)
        out += hdr
        for k in range(0, 64, 8):
            for c in range(ch):
                nib = data[c][k:k + 8]
                out += bytes(nib[j] | nib[j + 1] << 4 for j in range(0, 8, 2))
    return bytes(out)


def avi_chunks(path):
    d = open(path, 'rb').read()
    vids, auds, extra = [], [], b''
    def walk(o, end):
        nonlocal extra
        strl = 0
        while o + 8 <= end:
            fcc = d[o:o + 4]; sz = struct.unpack_from('<I', d, o + 4)[0]
            if fcc == b'LIST': walk(o + 12, o + 8 + sz)
            elif fcc == b'strf' and not extra and sz > 40 and d[o + 8 + 16:o + 8 + 20] in (b'WMV2',):
                extra = d[o + 8 + 40:o + 8 + sz]
            elif fcc[2:] in (b'dc', b'db'): vids.append(d[o + 8:o + 8 + sz])
            o += 8 + sz + (sz & 1)
    walk(12, len(d))
    return vids, extra


def wmv2_to_xmv_extra(e):
    v = struct.unpack('>I', e[:4])[0]
    b = lambda s: (v >> s) & 1
    return b(15) | b(14) << 1 | b(13) << 2 | b(12) << 3 | b(11) << 4 | b(10) << 5 | ((v >> 7) & 7) << 6


def main():
    a = sys.argv[1:]
    pcm16 = '--pcm' in a
    def opt(k, dflt):
        return a[a.index(k) + 1] if k in a else dflt
    fps = int(opt('--fps', '30')); size = opt('--size', '640x480'); secs = opt('--secs', None)
    pos = [x for i, x in enumerate(a) if not x.startswith('--') and (i == 0 or a[i - 1] not in ('--fps', '--size', '--secs'))]
    rate, ch = 44100, 2
    tmp = tempfile.mkdtemp()
    avi, raw = os.path.join(tmp, 'v.avi'), os.path.join(tmp, 'a.raw')
    if '--test' in a:
        src = ['-f', 'lavfi', '-i', f'testsrc=size={size}:rate={fps}:duration={secs or 6}',
               '-f', 'lavfi', '-i', f'sine=frequency=440:sample_rate={rate}:duration={secs or 6}']
        out = pos[0]
    else:
        src = ['-i', pos[0]] + (['-t', secs] if secs else [])
        out = pos[1]
    q = ['-hide_banner', '-loglevel', 'error', '-y']
    subprocess.check_call(['ffmpeg'] + q + src + ['-map', '0:v:0', '-s', size, '-r', str(fps), '-c:v', 'wmv2', '-b:v', '3M', '-g', str(fps), avi])
    subprocess.check_call(['ffmpeg'] + q + src + ['-map', '1:a:0' if '--test' in a else '0:a:0', '-ac', str(ch), '-ar', str(rate), '-f', 's16le', raw])
    frames, extra = avi_chunks(avi)
    pcm = struct.unpack('<%dh' % (os.path.getsize(raw) // 2), open(raw, 'rb').read())
    w, h = map(int, size.split('x'))
    spf = rate / fps
    packets = []
    for p0 in range(0, len(frames), fps):
        fr = frames[p0:p0 + fps]
        s0, s1 = int(p0 * spf), int((p0 + len(fr)) * spf)
        chunk = pcm[s0 * ch:s1 * ch]
        if pcm16:
            blk = 36 * ch // 4                                   # whole 36*ch-byte blocks
            n = (len(chunk) // (blk * 1)) * blk
            adata = struct.pack('<%dh' % n, *chunk[:n])
        else:
            adata = ima_blocks(list(chunk), ch)
        vdata = bytearray(struct.pack('<I', wmv2_to_xmv_extra(extra)) if p0 == 0 else b'')
        for i, f in enumerate(fr):
            f = f + b'\0' * (-len(f) % 4)
            if len(f) < 4: f = b'\0' * 4
            ts = 0 if p0 + i == 0 else round(1000 / fps)
            vdata += struct.pack('<I', (len(f) - 4) // 4 | ts << 17)
            vdata += b''.join(struct.pack('>I', struct.unpack_from('<I', f, k)[0]) for k in range(0, len(f), 4))
        vfield = (len(vdata) + 4) | len(fr) << 23 | (0x80000000 if p0 == 0 else 0)
        body = struct.pack('<II', vfield, 0) + struct.pack('<I', len(adata)) + bytes(vdata) + adata
        packets.append(body)
    hdr_len = 36 + 12
    sizes = [4 + len(b) for b in packets]
    o = bytearray()
    o += struct.pack('<IIII', sizes[0], hdr_len + sizes[0], max(sizes), 0) [:12] + b'xobX'
    o[0:4] = struct.pack('<I', sizes[0])
    o += struct.pack('<IIII', 4, w, h, int(len(frames) * 1000 / fps))
    o += struct.pack('<HH', 1, 0)
    o += struct.pack('<HHIHH', 1 if pcm16 else 0x69, ch, rate, 16 if pcm16 else 4, 0)
    for i, b in enumerate(packets):
        o += struct.pack('<I', sizes[i + 1] if i + 1 < len(sizes) else 0) + b
    open(out, 'wb').write(o)
    print(f'{out}: {w}x{h} {len(frames)} frames @ {fps}, {len(packets)} packets, {"PCM" if pcm16 else "ADPCM"} {rate} Hz')


main()
