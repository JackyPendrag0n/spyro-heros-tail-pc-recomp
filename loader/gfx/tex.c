/* Xbox texture formats -> RGBA8. */
#include "gfx.h"
#include <stdlib.h>
#include <string.h>

/* Swizzled (Morton) layout: address bits interleave x and y (and z), starting
 * with x, for as long as each dimension still has bits. */
static void masks(u32 w, u32 h, u32 d, u32 *mx, u32 *my, u32 *mz)
{
    u32 bit = 1, x = 0, y = 0, z = 0;
    *mx = *my = *mz = 0;
    int done;
    do {
        done = 1;
        if ((1u << x) < w) { *mx |= bit; bit <<= 1; x++; done = 0; }
        if ((1u << y) < h) { *my |= bit; bit <<= 1; y++; done = 0; }
        if ((1u << z) < d) { *mz |= bit; bit <<= 1; z++; done = 0; }
    } while (!done);
}

static u32 spread(u32 v, u32 mask)
{
    u32 r = 0;
    for (u32 b = 1; mask; b <<= 1) {
        u32 low = mask & -mask;
        if (v & b) r |= low;
        mask &= mask - 1;
    }
    return r;
}

void tex_unswizzle(const u8 *src, u8 *dst, u32 w, u32 h, u32 d, u32 bpp)
{
    u32 mx, my, mz;
    masks(w, h, d, &mx, &my, &mz);
    u32 *ox = malloc(w * 4), *oy = malloc(h * 4);
    for (u32 x = 0; x < w; x++) ox[x] = spread(x, mx);
    for (u32 y = 0; y < h; y++) oy[y] = spread(y, my);
    for (u32 z = 0; z < d; z++) {
        u32 oz = spread(z, mz);
        for (u32 y = 0; y < h; y++)
            for (u32 x = 0; x < w; x++)
                memcpy(dst + ((z * h + y) * w + x) * bpp, src + (ox[x] | oy[y] | oz) * bpp, bpp);
    }
    free(ox); free(oy);
}

static inline u8 x5(u32 v) { return (u8)((v << 3) | (v >> 2)); }
static inline u8 x6(u32 v) { return (u8)((v << 2) | (v >> 4)); }
static inline u8 x4(u32 v) { return (u8)(v * 17); }

/* One texel of an uncompressed format -> RGBA. */
static void texel(u32 f, const u8 *p, const u32 *pal, u8 *o)
{
    u32 v;
    switch (f) {
    case 0x00: case 0x13: o[0] = o[1] = o[2] = p[0]; o[3] = 255; break;               /* L8 */
    case 0x01: case 0x1B: o[0] = o[1] = o[2] = o[3] = p[0]; break;                    /* AL8 */
    case 0x19: case 0x1F: o[0] = o[1] = o[2] = 255; o[3] = p[0]; break;               /* A8 */
    case 0x1A: case 0x20: o[0] = o[1] = o[2] = p[0]; o[3] = p[1]; break;              /* A8L8 */
    case 0x0B: v = pal ? pal[p[0]] : (p[0] * 0x010101u | 0xFF000000u);                 /* P8 (palette ARGB) */
        o[0] = v >> 16; o[1] = v >> 8; o[2] = v; o[3] = v >> 24; break;
    case 0x02: case 0x10: v = p[0] | p[1] << 8;                                        /* A1R5G5B5 */
        o[0] = x5(v >> 10 & 31); o[1] = x5(v >> 5 & 31); o[2] = x5(v & 31); o[3] = (v & 0x8000) ? 255 : 0; break;
    case 0x03: case 0x1C: v = p[0] | p[1] << 8;                                        /* X1R5G5B5 */
        o[0] = x5(v >> 10 & 31); o[1] = x5(v >> 5 & 31); o[2] = x5(v & 31); o[3] = 255; break;
    case 0x04: case 0x1D: v = p[0] | p[1] << 8;                                        /* A4R4G4B4 */
        o[0] = x4(v >> 8 & 15); o[1] = x4(v >> 4 & 15); o[2] = x4(v & 15); o[3] = x4(v >> 12); break;
    case 0x05: case 0x11: v = p[0] | p[1] << 8;                                        /* R5G6B5 */
        o[0] = x5(v >> 11); o[1] = x6(v >> 5 & 63); o[2] = x5(v & 31); o[3] = 255; break;
    case 0x27: case 0x37: v = p[0] | p[1] << 8;                                        /* R6G5B5 */
        o[0] = x6(v >> 10); o[1] = x5(v >> 5 & 31); o[2] = x5(v & 31); o[3] = 255; break;
    case 0x38: case 0x3D: v = p[0] | p[1] << 8;                                        /* R5G5B5A1 */
        o[0] = x5(v >> 11); o[1] = x5(v >> 6 & 31); o[2] = x5(v >> 1 & 31); o[3] = (v & 1) ? 255 : 0; break;
    case 0x39: case 0x3E: v = p[0] | p[1] << 8;                                        /* R4G4B4A4 */
        o[0] = x4(v >> 12); o[1] = x4(v >> 8 & 15); o[2] = x4(v >> 4 & 15); o[3] = x4(v & 15); break;
    case 0x06: case 0x12: o[0] = p[2]; o[1] = p[1]; o[2] = p[0]; o[3] = p[3]; break;  /* A8R8G8B8 */
    case 0x07: case 0x1E: o[0] = p[2]; o[1] = p[1]; o[2] = p[0]; o[3] = 255; break;   /* X8R8G8B8 */
    case 0x3A: case 0x3F: o[0] = p[0]; o[1] = p[1]; o[2] = p[2]; o[3] = p[3]; break;  /* A8B8G8R8 */
    case 0x3B: case 0x40: o[0] = p[1]; o[1] = p[2]; o[2] = p[3]; o[3] = p[0]; break;  /* B8G8R8A8 */
    case 0x3C: case 0x41: o[0] = p[3]; o[1] = p[2]; o[2] = p[1]; o[3] = p[0]; break;  /* R8G8B8A8 */
    case 0x28: case 0x17: o[0] = 0; o[1] = p[1]; o[2] = p[0]; o[3] = 255; break;      /* G8B8 / V8U8 */
    case 0x29: case 0x16: o[0] = p[1]; o[1] = 0; o[2] = p[0]; o[3] = 255; break;      /* R8B8 */
    case 0x32: case 0x35: o[0] = o[1] = o[2] = p[1]; o[3] = 255; break;               /* L16 */
    case 0x33: case 0x36: o[0] = p[1]; o[1] = p[3]; o[2] = 0; o[3] = 255; break;      /* V16U16 */
    case 0x2C: case 0x30: case 0x2D: case 0x31: o[0] = o[1] = o[2] = p[1]; o[3] = 255; break;  /* depth 16 */
    case 0x2A: case 0x2E: case 0x2B: case 0x2F: o[0] = o[1] = o[2] = p[3]; o[3] = 255; break;  /* depth 24 */
    default: {                                                                       /* unknown: magenta */
        static u32 seen[16]; static int ns;
        int k; for (k = 0; k < ns && seen[k] != f; k++) ;
        if (k == ns && ns < 16) { seen[ns++] = f; klog("tex: unsupported texture format %#x\n", f); }
        o[0] = 255; o[1] = 0; o[2] = 255; o[3] = 255; break; }
    }
}

static void rgb565(u16 v, u8 *o) { o[0] = x5(v >> 11); o[1] = x6(v >> 5 & 63); o[2] = x5(v & 31); }

static void dxt_block(u32 f, const u8 *b, u8 out[16][4])
{
    const u8 *cb = f == 0x0C ? b : b + 8;
    u16 c0 = cb[0] | cb[1] << 8, c1 = cb[2] | cb[3] << 8;
    u8 pal[4][4];
    rgb565(c0, pal[0]); rgb565(c1, pal[1]);
    pal[0][3] = pal[1][3] = 255;
    if (f != 0x0C || c0 > c1) {
        for (int k = 0; k < 3; k++) { pal[2][k] = (2 * pal[0][k] + pal[1][k]) / 3; pal[3][k] = (pal[0][k] + 2 * pal[1][k]) / 3; }
        pal[2][3] = pal[3][3] = 255;
    } else {
        for (int k = 0; k < 3; k++) { pal[2][k] = (pal[0][k] + pal[1][k]) / 2; pal[3][k] = 0; }
        pal[2][3] = 255; pal[3][3] = 0;
    }
    u32 idx = cb[4] | cb[5] << 8 | cb[6] << 16 | (u32)cb[7] << 24;
    for (int i = 0; i < 16; i++) memcpy(out[i], pal[(idx >> (2 * i)) & 3], 4);
    if (f == 0x0E) {                                      /* DXT3: explicit 4-bit alpha */
        for (int i = 0; i < 16; i++) out[i][3] = x4((b[i / 2] >> ((i & 1) * 4)) & 15);
    } else if (f == 0x0F) {                               /* DXT5: interpolated alpha */
        u8 a[8]; a[0] = b[0]; a[1] = b[1];
        if (a[0] > a[1]) for (int k = 1; k < 7; k++) a[k + 1] = ((7 - k) * a[0] + k * a[1]) / 7;
        else { for (int k = 1; k < 5; k++) a[k + 1] = ((5 - k) * a[0] + k * a[1]) / 5; a[6] = 0; a[7] = 255; }
        u64 bits = 0;
        for (int k = 0; k < 6; k++) bits |= (u64)b[2 + k] << (8 * k);
        for (int i = 0; i < 16; i++) out[i][3] = a[(bits >> (3 * i)) & 7];
    }
}

/* src points at this level's data; pitch is used for linear formats only. */
int tex_decode(u32 f, const u8 *src, u32 w, u32 h, u32 pitch, const u32 *palette, u8 *dst)
{
    if (tex_is_dxt(f)) {
        u32 bs = f == 0x0C ? 8 : 16, bw = (w + 3) / 4, bh = (h + 3) / 4;
        u8 blk[16][4];
        for (u32 by = 0; by < bh; by++)
            for (u32 bx = 0; bx < bw; bx++) {
                dxt_block(f, src + (by * bw + bx) * bs, blk);
                for (int i = 0; i < 16; i++) {
                    u32 x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
                    if (x < w && y < h) memcpy(dst + (y * w + x) * 4, blk[i], 4);
                }
            }
        return 0;
    }
    if (f == 0x24 || f == 0x25) {                         /* YUY2 / UYVY: BT.601 video range */
        int yo = f == 0x24 ? 0 : 1, uo = f == 0x24 ? 1 : 0;
        for (u32 y = 0; y < h; y++)
            for (u32 x = 0; x < w; x++) {
                const u8 *p = src + y * pitch + (x & ~1u) * 2;
                int Y = p[yo + 2 * (x & 1)] - 16, U = p[uo] - 128, V = p[uo + 2] - 128;
                int r = (298 * Y + 409 * V + 128) >> 8, g = (298 * Y - 100 * U - 208 * V + 128) >> 8, b = (298 * Y + 516 * U + 128) >> 8;
                u8 *o = dst + (y * w + x) * 4;
                o[0] = r < 0 ? 0 : r > 255 ? 255 : r; o[1] = g < 0 ? 0 : g > 255 ? 255 : g; o[2] = b < 0 ? 0 : b > 255 ? 255 : b; o[3] = 255;
            }
        return 0;
    }
    u32 bpp = tex_bpp(f) / 8;
    if (!bpp) {
        static u32 seen[16]; static int ns;
        int k; for (k = 0; k < ns && seen[k] != f; k++) ;
        if (k == ns && ns < 16) { seen[ns++] = f; klog("tex: unknown texture format %#x (no size)\n", f); }
        memset(dst, 0, (size_t)w * h * 4);
        return -1;
    }
    const u8 *lin = src;
    u8 *tmp = NULL;
    if (!tex_is_linear(f)) {
        tmp = malloc(w * h * bpp);
        tex_unswizzle(src, tmp, w, h, 1, bpp);
        lin = tmp;
        pitch = w * bpp;
    }
    for (u32 y = 0; y < h; y++)
        for (u32 x = 0; x < w; x++)
            texel(f, lin + y * pitch + x * bpp, palette, dst + (y * w + x) * 4);
    free(tmp);
    return 0;
}
