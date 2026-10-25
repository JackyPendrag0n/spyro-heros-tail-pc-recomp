/* Xbox Direct3D 8 high-level emulation.
 *
 * Principles:
 *  - Xbox resource structs stay byte-identical, so pure XDK helpers (D3DX,
 *    XGRAPHICS, GetDesc, GetLevelCount...) keep running natively.
 *  - The game writes deferred render/texture states straight into D3D's
 *    tables; those tables are the source of truth and are read at draw time.
 *  - Only functions that touch the GPU, the device, or resource memory
 *    are replaced. */
#define _GNU_SOURCE
#include "gfx.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>

struct gfx_state G;
u32 arena_alloc(u32 size, u32 align, int do_commit, int top_down);
void arena_free(u32 addr);

static u8 *g_fake_device;
static u32 g_dev_ptr_addr;          /* &D3D_g_pDevice */

u32 *guestp(u32 a)
{
    /* D3D stores either virtual addresses or (on hardware) physical ones; both
     * map to the same place here because the arena is below 0x80000000. */
    return (u32 *)(uintptr_t)(a & 0x7FFFFFFFu);
}

static int g_d3d_trace;
#define DTRACE(...) do { if (g_d3d_trace) klog(__VA_ARGS__); } while (0)

/* ================= formats ================= */
int tex_is_dxt(u32 f) { return f == 0x0C || f == 0x0E || f == 0x0F; }
int tex_is_linear(u32 f)
{
    switch (f) {
    case 0x10: case 0x11: case 0x12: case 0x13: case 0x16: case 0x17: case 0x1B: case 0x1C:
    case 0x1D: case 0x1E: case 0x1F: case 0x20: case 0x2E: case 0x2F: case 0x30: case 0x31:
    case 0x35: case 0x36: case 0x37: case 0x3D: case 0x3E: case 0x3F: case 0x40: case 0x41:
    case 0x24: case 0x25:                                   /* YUY2 / UYVY (movie frames) */
        return 1;
    }
    return 0;
}
u32 tex_bpp(u32 f)
{
    switch (f) {
    case 0x00: case 0x01: case 0x0B: case 0x19: case 0x13: case 0x1B: case 0x1F: return 8;
    case 0x02: case 0x03: case 0x04: case 0x05: case 0x1A: case 0x27: case 0x28: case 0x29:
    case 0x2C: case 0x2D: case 0x32: case 0x38: case 0x39: case 0x10: case 0x11: case 0x16:
    case 0x17: case 0x1C: case 0x1D: case 0x20: case 0x30: case 0x31: case 0x35: case 0x37:
    case 0x3D: case 0x3E: case 0x24: case 0x25: return 16;
    case 0x06: case 0x07: case 0x2A: case 0x2B: case 0x33: case 0x3A: case 0x3B: case 0x3C:
    case 0x12: case 0x1E: case 0x2E: case 0x2F: case 0x36: case 0x3F: case 0x40: case 0x41: return 32;
    case 0x0C: return 4;
    case 0x0E: case 0x0F: return 8;
    }
    return 0;
}

/* Dimensions of a pixel container (level 0). */
void tex_dims(const XPixelContainer *t, u32 *w, u32 *h, u32 *d, int *levels, int *linear, int *pitch)
{
    u32 f = t->Format, fmt = XFMT_FMT(f);
    *linear = tex_is_linear(fmt);
    if (t->Size) {
        *w = (t->Size & 0xFFF) + 1;
        *h = ((t->Size >> 12) & 0xFFF) + 1;
        *pitch = (((t->Size >> 24) & 0xFF) + 1) * 64;
        *d = 1;
        *levels = 1;
    } else {
        *w = 1u << XFMT_USIZE(f);
        *h = 1u << XFMT_VSIZE(f);
        *d = XFMT_DIM(f) == 3 ? 1u << XFMT_PSIZE(f) : 1;
        *levels = XFMT_MIPS(f) ? XFMT_MIPS(f) : 1;
        u32 bpp = tex_bpp(fmt);
        *pitch = tex_is_dxt(fmt) ? (*w / 4) * (fmt == 0x0C ? 8 : 16) : *w * bpp / 8;
    }
}

static u32 level_bytes(u32 fmt, u32 w, u32 h, u32 d)
{
    if (tex_is_dxt(fmt)) {
        u32 bw = (w + 3) / 4, bh = (h + 3) / 4;
        return bw * bh * (fmt == 0x0C ? 8 : 16) * d;
    }
    return w * h * d * tex_bpp(fmt) / 8;
}

/* Byte offset of a mip level within a swizzled/compressed texture (one face). */
u32 tex_level_offset(u32 f, u32 size, int level, u32 *wo, u32 *ho, u32 *pitch)
{
    (void)size;
    u32 fmt = XFMT_FMT(f);
    u32 w = 1u << XFMT_USIZE(f), h = 1u << XFMT_VSIZE(f);
    u32 d = XFMT_DIM(f) == 3 ? 1u << XFMT_PSIZE(f) : 1;
    u32 off = 0;
    for (int l = 0; l < level; l++) {
        off += level_bytes(fmt, w, h, d);
        if (w > 1) w >>= 1;
        if (h > 1) h >>= 1;
        if (d > 1) d >>= 1;
        if (tex_is_dxt(fmt)) { if (w < 4) w = 4; if (h < 4) h = 4; }
    }
    if (wo) *wo = w;
    if (ho) *ho = h;
    if (pitch) *pitch = tex_is_dxt(fmt) ? (w / 4) * (fmt == 0x0C ? 8 : 16) : w * tex_bpp(fmt) / 8;
    return off;
}

static u32 face_bytes(u32 f)
{
    int lv = XFMT_MIPS(f) ? XFMT_MIPS(f) : 1;
    u32 off = tex_level_offset(f, 0, lv, NULL, NULL, NULL);
    return (off + 127) & ~127u;     /* cube faces are 128-byte aligned */
}

static u32 ilog2(u32 v) { u32 r = 0; while ((1u << r) < v) r++; return r; }

/* ================= resource helpers ================= */
static void *gcalloc(u32 n) { u32 a = arena_alloc(n, 0x1000, 1, 0); if (a) memset((void *)(uintptr_t)a, 0, n); return (void *)(uintptr_t)a; }

static XSurface *new_surface(u32 w, u32 h, u32 xfmt, int alloc_data)
{
    XSurface *s = calloc(1, sizeof *s);
    s->Common = 1 | XCOMMON_TYPE_SURFACE | XCOMMON_D3DCREATED;
    s->Format = (xfmt << 8) | (2 << 4) | (1 << 16) | 1;   /* 2D, 1 level, DMA A */
    u32 bpp = tex_bpp(xfmt) / 8;
    u32 pitch = ((w * bpp + 63) / 64) * 64;
    s->Size = (w - 1) | ((h - 1) << 12) | ((pitch / 64 - 1) << 24);
    if (alloc_data) s->Data = (u32)(uintptr_t)gcalloc(pitch * h);
    return s;
}

static int surf_is_backbuffer(XSurface *s) { return s && (s == G.backbuffer); }

/* ================= device ================= */
static void set_default_states(void)
{
    u32 *rs = G.rs;
    rs[XD3DRS_ZFUNC] = 0x203;             /* LESSEQUAL */
    rs[XD3DRS_ALPHAFUNC] = 0x207;         /* ALWAYS */
    rs[XD3DRS_ALPHABLENDENABLE] = 0;
    rs[XD3DRS_ALPHATESTENABLE] = 0;
    rs[XD3DRS_SRCBLEND] = 1;              /* ONE */
    rs[XD3DRS_DESTBLEND] = 0;             /* ZERO */
    rs[XD3DRS_ZWRITEENABLE] = 1;
    rs[XD3DRS_SHADEMODE] = 0x1D01;        /* GOURAUD */
    rs[XD3DRS_COLORWRITEENABLE] = 0x01010101;
    rs[XD3DRS_STENCILZFAIL] = 0x1E00; rs[XD3DRS_STENCILPASS] = 0x1E00; rs[XD3DRS_STENCILFAIL] = 0x1E00; /* KEEP */
    rs[XD3DRS_STENCILFUNC] = 0x207;
    rs[XD3DRS_STENCILMASK] = 0xFF; rs[XD3DRS_STENCILWRITEMASK] = 0xFF;
    rs[XD3DRS_BLENDOP] = 0x8006;          /* ADD */
    rs[XD3DRS_FILLMODE] = 0x1B02; rs[XD3DRS_BACKFILLMODE] = 0x1B02;  /* SOLID */
    rs[XD3DRS_ZENABLE] = 1;
    rs[XD3DRS_CULLMODE] = 0x901;          /* CCW */
    rs[XD3DRS_FRONTFACE] = 0x900;         /* CW */
    rs[XD3DRS_MULTISAMPLEMASK] = 0xFFFFFFFF;
    rs[XD3DRS_LIGHTING] = 1;
    rs[XD3DRS_FOGSTART] = 0; { float one = 1.0f; memcpy(&rs[XD3DRS_FOGEND], &one, 4); memcpy(&rs[XD3DRS_FOGDENSITY], &one, 4); }
    for (int s = 0; s < 4; s++) {
        u32 *t = G.tss[s];
        t[XTSS_ADDRESSU] = t[XTSS_ADDRESSV] = t[XTSS_ADDRESSW] = 1;
        t[XTSS_MAGFILTER] = t[XTSS_MINFILTER] = 1;
        t[XTSS_MIPFILTER] = 0;
        t[XTSS_MAXANISOTROPY] = 1;
        t[XTSS_COLOROP] = s == 0 ? 4 : 1;       /* MODULATE / DISABLE */
        t[XTSS_ALPHAOP] = s == 0 ? 2 : 1;       /* SELECTARG1 / DISABLE */
        t[XTSS_COLORARG1] = t[XTSS_ALPHAARG1] = 2;   /* TEXTURE */
        t[XTSS_COLORARG2] = t[XTSS_ALPHAARG2] = 1;   /* CURRENT */
        t[XTSS_TEXCOORDINDEX] = s;
    }
}

static u32 STDCALL h_Direct3D_CreateDevice(u32 Adapter, u32 DeviceType, void *hWnd, u32 Behavior,
                                           u32 *pp, u32 *ppDevice)
{
    /* X_D3DPRESENT_PARAMETERS: BackBufferWidth, BackBufferHeight, BackBufferFormat, BackBufferCount,
     * MultiSampleType, SwapEffect, hDeviceWindow, Windowed, EnableAutoDepthStencil, AutoDepthStencilFormat, Flags, ... */
    u32 w = pp[0] ? pp[0] : 640, h = pp[1] ? pp[1] : 480;
    klog("Direct3D_CreateDevice: %ux%u fmt %#x bufs %u msaa %#x autodepth %u (%#x) flags %#x refresh %u interval %#x\n",
         w, h, pp[2], pp[3], pp[4], pp[8], pp[9], pp[10], pp[11], pp[12]);
    G.bb_width = w; G.bb_height = h;
    /* real guest memory, so textures the game points at the back buffer resolve to it */
    /* the back buffer is linear: map the requested format to its LIN_ variant (A8R8G8B8 keeps
     * its alpha, which titles sample, e.g. as a glow mask) */
    u32 bbf = pp[2];
    switch (bbf) {
    case 0x06: bbf = 0x12; break;   /* A8R8G8B8 -> LIN_A8R8G8B8 */
    case 0x07: bbf = 0x1E; break;   /* X8R8G8B8 -> LIN_X8R8G8B8 */
    case 0x05: bbf = 0x11; break;   /* R5G6B5 -> LIN_R5G6B5 */
    case 0x03: bbf = 0x1C; break;   /* X1R5G5B5 -> LIN_X1R5G5B5 */
    case 0x02: bbf = 0x10; break;   /* A1R5G5B5 -> LIN_A1R5G5B5 */
    case 0x12: case 0x1E: case 0x11: case 0x1C: case 0x10: break;
    default: bbf = 0x1E;
    }
    G.backbuffer = new_surface(w, h, bbf, 1);
    G.backbuffer->Common += 0x10;                        /* never freed */
    G.depthbuffer = NULL;
    if (pp[8]) {
        u32 zf = pp[9] ? pp[9] : 0x2A;
        G.depthbuffer = new_surface(w, h, zf, 1);
        G.depthbuffer->Common += 0x10;
    }
    G.rt = G.backbuffer;
    G.zs = G.depthbuffer;
    set_default_states();
    if (gl_init(w, h)) { klog("graphics backend failed to initialise\n"); }
    gl_set_render_target(G.rt, G.zs);
    /* Fields of the XDK 5849 CDevice that native helpers (GetDesc, GetDisplayMode) read. */
    *(u32 *)(g_fake_device + 0x1a14) = (u32)(uintptr_t)G.backbuffer;    /* back buffer surface */
    *(u32 *)(g_fake_device + 0x1a18) = (u32)(uintptr_t)G.backbuffer;    /* front buffer surface */
    *(u32 *)(g_fake_device + 0x196c) = pp[4];                           /* multisample type */
    if (ppDevice) *ppDevice = (u32)(uintptr_t)g_fake_device;
    wr32(g_dev_ptr_addr, (u32)(uintptr_t)g_fake_device);
    return 0;
}

static u32 STDCALL h_D3DDevice_Reset(u32 *pp)
{
    klog("D3DDevice_Reset(%ux%u)\n", pp[0], pp[1]);
    return 0;
}

static u32 STDCALL h_D3DDevice_Release(void) { return 1; }

static u32 STDCALL h_D3D_SetPushBufferSize(u32 a, u32 b) { return 0; }

static u32 STDCALL h_D3DDevice_GetDeviceCaps_unused(void *p) { return 0; }

static void addref(void *r) { if (r) ((XResource *)r)->Common++; }

static u32 STDCALL h_D3DDevice_GetBackBuffer2(s32 which)
{
    addref(G.backbuffer);
    return (u32)(uintptr_t)G.backbuffer;
}
static u32 STDCALL h_D3DDevice_GetRenderTarget2(void) { addref(G.rt); return (u32)(uintptr_t)G.rt; }
static u32 STDCALL h_D3DDevice_GetDepthStencilSurface2(void) { addref(G.zs); return (u32)(uintptr_t)G.zs; }

static u32 STDCALL h_D3DDevice_SetRenderTarget(XSurface *rt, XSurface *zs)
{
    DTRACE("SetRenderTarget(%p, %p)\n", (void *)rt, (void *)zs);
    if (rt) G.rt = rt;
    G.zs = zs;
    gl_set_render_target(G.rt, G.zs);
    return 0;
}

static u32 STDCALL h_D3DDevice_Clear(u32 count, const s32 *rects, u32 flags, u32 color, u32 zbits, u32 stencil)
{
    float z; memcpy(&z, &zbits, 4);
    DTRACE("Clear(flags %#x color %08x z %f)\n", flags, color, z);
    gl_clear(count, rects, flags, color, z, stencil);
    return 0;
}

typedef void CDECLX (*vblank_cb_t)(u32 *);
static volatile u32 g_vblank_count;

static u32 STDCALL h_D3DDevice_Swap(u32 flags)
{
    { extern void debugmenu_game_tick(void); debugmenu_game_tick(); }
    gl_present();
    G.frame++;
    if (G.frame < 5 || G.frame % 600 == 0) klog("frame %u presented (vblank %u)\n", G.frame, g_vblank_count);
    return G.frame;
}

/* The Xbox raises a vertical-blank interrupt at 60 Hz whether or not the game
 * presents; games count these for timing and wait on them while loading. */
void *kthread_new_ethread(u32 ext);
/* frame rate: the game runs one update per vertical blank, so a higher rate is achieved by
 * raising the emulated vblank rate together with the game's refresh/target-rate variables
 * (debugmenu.c). 60 = original. */
volatile int g_target_fps = 60;
static void *vblank_thread(void *arg)
{
    u8 *eth = kthread_new_ethread(0);
    static u8 tls[256];
    *(void **)(eth + KT_TLSDATA) = tls;
    xthread_setup_current(eth, tls + sizeof tls, NULL);
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);
    for (;;) {
        next.tv_nsec += 1000000000L / (g_target_fps > 0 ? g_target_fps : 60);
        if (next.tv_nsec >= 1000000000L) { next.tv_sec++; next.tv_nsec -= 1000000000L; }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
        g_vblank_count++;
        if (G.vblank_cb) { u32 data[3] = { g_vblank_count, G.frame, 0 }; ((vblank_cb_t)G.vblank_cb)(data); }
    }
    return NULL;
}

static u32 STDCALL h_D3DDevice_SetVerticalBlankCallback(void *cb)
{
    static int started;
    G.vblank_cb = cb;
    klog("SetVerticalBlankCallback(%p)\n", cb);
    if (!started++) { pthread_t t; pthread_create(&t, NULL, vblank_thread, NULL); pthread_detach(t); }
    return 0;
}

typedef void CDECLX (*d3dcallback_t)(u32);
static u32 STDCALL h_D3DDevice_InsertCallback(u32 type, void *cb, u32 ctx)
{
    /* The GPU would call this when it reaches this point; we are always "there". */
    if (cb) ((d3dcallback_t)cb)(ctx);
    return 0;
}

static u32 g_push_scratch[0x4000];
static u32 STDCALL h_D3DDevice_BeginPush_4(u32 count)
{
    if (count > 0x4000) count = 0x4000;
    return (u32)(uintptr_t)g_push_scratch;
}
/* NV2A methods the game writes itself between BeginPush/EndPush */
#define NV097_SET_TRANSFORM_CONSTANT      0x0B80   /* .. 0x0BFC: 32 dwords, auto-increment */
#define NV097_SET_TRANSFORM_CONSTANT_LOAD 0x1EA4
static u32 g_const_load;

/* inline geometry: SET_BEGIN_END(prim) ... INLINE_ARRAY dwords ... SET_BEGIN_END(0) */
#define NV097_SET_BEGIN_END  0x17FC
#define NV097_INLINE_ARRAY   0x1818
static u32 g_inline_prim, *g_inline_buf, g_inline_n, g_inline_cap;

static u32 vattr_size(u32 type)
{
    switch (type & 0x0F) {
    case 0x2: return type == 0x72 ? 12 : ((type >> 4) & 0xF) * 4;
    case 0x0: return 4;
    case 0x5: case 0x1: return ((type >> 4) & 0xF) * 2;
    case 0x4: return (type >> 4) & 0xF;
    case 0x6: return 4;
    }
    return 0;
}

static void inline_draw(void)
{
    const struct vattr *at = G.use_direct ? G.direct_attrs : G.vs ? G.vs->attrs : NULL;
    if (!at || !g_inline_n) return;
    /* inline vertices are packed in attribute order */
    u32 stride = 0;
    struct vattr saved[16];
    memcpy(saved, at, sizeof saved);
    struct vattr *w = (struct vattr *)at;
    for (int a = 0; a < 16; a++) if (w[a].present) { w[a].offset = (u16)stride; w[a].stream = 0; stride += vattr_size(w[a].type); }
    if (stride) {
        u32 nverts = g_inline_n * 4 / stride;
        gl_draw(g_inline_prim, nverts, NULL, (const u8 *)g_inline_buf, stride);
    }
    memcpy(w, saved, sizeof saved);
}

static void push_method(u32 method, u32 value)
{
    if (method == NV097_SET_BEGIN_END) {
        if (value) { g_inline_prim = value; g_inline_n = 0; }
        else { inline_draw(); g_inline_n = 0; g_inline_prim = 0; }
        return;
    }
    if (method == NV097_INLINE_ARRAY) {
        if (g_inline_n == g_inline_cap) { g_inline_cap = g_inline_cap ? g_inline_cap * 2 : 1024; g_inline_buf = realloc(g_inline_buf, g_inline_cap * 4); }
        g_inline_buf[g_inline_n++] = value;
        return;
    }
    if (method == NV097_SET_TRANSFORM_CONSTANT_LOAD) { g_const_load = value * 4; return; }
    if (method >= NV097_SET_TRANSFORM_CONSTANT && method < NV097_SET_TRANSFORM_CONSTANT + 0x80) {
        if (g_const_load < 192 * 4) { memcpy(&G.vsc[0][0] + g_const_load, &value, 4); G.vsc_dirty = 1; }
        g_const_load++;
        return;
    }
    static u32 seen[64]; static int nseen;
    for (int i = 0; i < nseen; i++) if (seen[i] == method) return;
    if (nseen < 64) { seen[nseen++] = method; klog("game pushbuffer: unhandled NV2A method %#x (value %08x)\n", method, value); }
}

static u32 STDCALL h_D3DDevice_EndPush(u32 *end)
{
    u32 n = end - g_push_scratch, i = 0;
    static int trace_frame = -2;
    if (trace_frame == -2) trace_frame = getenv("XBOX_TRACE_PUSH") ? (int)parse_u32(getenv("XBOX_TRACE_PUSH"), 10) : -1;
    int tr = trace_frame >= 0 && G.frame == (u32)trace_frame;
    while (i < n) {
        u32 hdr = g_push_scratch[i++];
        if (!hdr) continue;
        u32 count = (hdr >> 18) & 0x7FF, method = hdr & 0x1FFC;
        int no_inc = (hdr & 0x40000000) != 0;
        if (tr) klog("push: hdr %08x method %#x count %u%s first %08x (%g)\n", hdr, method, count, no_inc ? " noinc" : "",
                     i < n ? g_push_scratch[i] : 0, i < n ? *(float *)&g_push_scratch[i] : 0.0f);
        for (u32 k = 0; k < count && i < n; k++, i++)
            push_method(no_inc ? method : method + 4 * k, g_push_scratch[i]);
    }
    return 0;
}

static u32 STDCALL h_KickAndWait(void) { gl_flush(); return 0; }

static u32 STDCALL h_D3DDevice_SetGammaRamp(u32 flags, void *ramp) { return 0; }

/* ---- display mode / field status ---- */
static u32 STDCALL h_D3DDevice_GetDisplayMode(u32 *mode)
{
    mode[0] = G.bb_width; mode[1] = G.bb_height; mode[2] = 60; mode[3] = 0x07;
    return 0;
}
static u32 STDCALL h_D3DDevice_GetDisplayFieldStatus(u32 *st)
{
    st[0] = g_vblank_count & 1; st[1] = g_vblank_count;
    return 0;
}

/* ---- render states with dedicated setters (they also store into the table) ---- */
static u32 FASTCALL h_SetRenderState_Simple(u32 method, u32 value) { (void)method; (void)value; return 0; }
#define RS_SETTER(fn, idx) static u32 STDCALL fn(u32 v) { G.rs[idx] = v; return 0; }
RS_SETTER(h_RS_FogColor, XD3DRS_FOGCOLOR)
RS_SETTER(h_RS_CullMode, XD3DRS_CULLMODE)
RS_SETTER(h_RS_FillMode, XD3DRS_FILLMODE)
RS_SETTER(h_RS_ZEnable, XD3DRS_ZENABLE)
RS_SETTER(h_RS_YuvEnable, XD3DRS_YUVENABLE)
RS_SETTER(h_RS_OcclusionCullEnable, XD3DRS_OCCLUSIONCULLENABLE)
RS_SETTER(h_RS_DoNotCullUncompressed, XD3DRS_DONOTCULLUNCOMPRESSED)
static u32 STDCALL h_TS_BorderColor(u32 stage, u32 v) { if (stage < 4) G.tss[stage][XTSS_BORDERCOLOR] = v; return 0; }
static u32 STDCALL h_TS_TexCoordIndex(u32 stage, u32 v) { if (stage < 4) G.tss[stage][XTSS_TEXCOORDINDEX] = v; return 0; }
static u32 STDCALL h_D3DDevice_SetRenderStateNotInline(u32 state, u32 v) { if (state < XD3DRS_COUNT) G.rs[state] = v; return 0; }
static float g_zclip[2] = { 0, 1 };
static u32 STDCALL h_D3DDevice_SetDepthClipPlanes(u32 n, u32 f, u32 flags) { memcpy(&g_zclip[0], &n, 4); memcpy(&g_zclip[1], &f, 4); return 0; }
static u32 STDCALL h_D3DDevice_SetScissors(u32 count, u32 exclusive, const s32 *rects)
{
    G.scissor_count = count > 8 ? 8 : count;
    G.scissor_exclusive = exclusive;
    for (u32 i = 0; i < (u32)G.scissor_count; i++) memcpy(G.scissors[i], rects + 4 * i, 16);
    return 0;
}
static u32 STDCALL h_D3DDevice_SetScreenSpaceOffset(u32 x, u32 y) { memcpy(&G.ss_offset[0], &x, 4); memcpy(&G.ss_offset[1], &y, 4); return 0; }
static u32 STDCALL h_GetScreenSpaceOffset(float *x, float *y) { *x = G.ss_offset[0]; *y = G.ss_offset[1]; return 0; }

/* ---- textures / surfaces ---- */
static u32 STDCALL h_D3DDevice_CreateTexture2(u32 w, u32 h, u32 d, u32 levels, u32 usage, u32 fmt, u32 rtype)
{
    XPixelContainer *t = calloc(1, sizeof *t);
    t->Common = 1 | XCOMMON_TYPE_TEXTURE | XCOMMON_D3DCREATED;
    int cube = rtype == 5, vol = rtype == 4;
    u32 bytes;
    if (tex_is_linear(fmt)) {
        u32 bpp = tex_bpp(fmt) / 8, pitch = ((w * bpp + 63) / 64) * 64;
        t->Format = (fmt << 8) | (2 << 4) | (1 << 16) | 1;
        t->Size = (w - 1) | ((h - 1) << 12) | ((pitch / 64 - 1) << 24);
        bytes = pitch * h;
    } else {
        u32 lw = ilog2(w), lh = ilog2(h), ld = vol ? ilog2(d) : 0;
        u32 maxl = (lw > lh ? lw : lh) + 1;
        if (levels == 0 || levels > maxl) levels = maxl;
        t->Format = (fmt << 8) | ((vol ? 3 : 2) << 4) | (levels << 16) | (lw << 20) | (lh << 24) | (ld << 28) | 1 | (cube ? XFMT_CUBEMAP : 0);
        bytes = cube ? face_bytes(t->Format) * 6 : tex_level_offset(t->Format, 0, levels, NULL, NULL, NULL);
    }
    t->Data = (u32)(uintptr_t)gcalloc(bytes + 64);
    DTRACE("CreateTexture2(%ux%ux%u lv %u usage %#x fmt %#x type %u) -> %p data %08x\n", w, h, d, levels, usage, fmt, rtype, (void *)t, t->Data);
    return (u32)(uintptr_t)t;
}

static u32 STDCALL h_D3DTexture_GetSurfaceLevel2(XPixelContainer *t, u32 level)
{
    XSurface *s = calloc(1, sizeof *s);
    s->Common = 1 | XCOMMON_TYPE_SURFACE | XCOMMON_D3DCREATED;
    s->Parent = t;
    addref(t);
    u32 w, h, pitch;
    if (t->Size) {
        s->Data = t->Data; s->Size = t->Size;
        s->Format = (t->Format & ~0x000F0000u) | (1 << 16);
    } else {
        u32 off = tex_level_offset(t->Format, 0, level, &w, &h, &pitch);
        s->Data = t->Data + off;
        s->Format = (t->Format & 0x0000FFFFu & ~XFMT_CUBEMAP) | (1 << 16) | (ilog2(w) << 20) | (ilog2(h) << 24);
        s->Size = 0;
    }
    return (u32)(uintptr_t)s;
}

static u32 lock_common(XPixelContainer *t, u32 level, u32 *locked)
{
    u32 w, h, d, pitch_l; int lv, lin, pitch;
    tex_dims(t, &w, &h, &d, &lv, &lin, &pitch);
    u32 off = 0;
    if (!t->Size) { off = tex_level_offset(t->Format, 0, level, &w, &h, &pitch_l); pitch = pitch_l; }
    locked[0] = pitch;
    locked[1] = (t->Data & 0x7FFFFFFFu) + off;
    gl_texture_dirty(t->Data);
    return 0;
}
static u32 STDCALL h_D3DTexture_LockRect(XPixelContainer *t, u32 level, u32 *locked, void *rect, u32 flags)
{ return lock_common(t, level, locked); }
static u32 STDCALL h_D3DSurface_LockRect(XSurface *s, u32 *locked, void *rect, u32 flags)
{ return lock_common((XPixelContainer *)s, 0, locked); }

static u32 STDCALL h_D3DResource_Register(XResource *r, u32 base)
{
    r->Data += base;
    return 0;
}

static u32 STDCALL h_D3DResource_Release(XResource *r)
{
    if (!r) return 0;
    u32 rc = r->Common & XCOMMON_REFMASK;
    if (rc) rc--;
    r->Common = (r->Common & ~XCOMMON_REFMASK) | rc;
    if (rc == 0 && (r->Common & XCOMMON_D3DCREATED)) {
        u32 type = r->Common & XCOMMON_TYPE_MASK;
        if (type == XCOMMON_TYPE_SURFACE && ((XSurface *)r)->Parent) {
            h_D3DResource_Release((XResource *)((XSurface *)r)->Parent);
        } else if (r->Data && (type == XCOMMON_TYPE_TEXTURE || type == XCOMMON_TYPE_VB || type == XCOMMON_TYPE_SURFACE)) {
            gl_texture_dirty(r->Data);
            arena_free(r->Data & 0x7FFFFFFFu);
        }
        r->Data = 0;
        /* struct memory is leaked on purpose: the game may still hold stale pointers */
    }
    return rc;
}

static u32 STDCALL h_D3DResource_BlockUntilNotBusy(void *r) { return 0; }

static u32 STDCALL h_D3DDevice_SetTexture(u32 stage, XPixelContainer *t)
{
    if (stage < 4) {
        if (t) addref(t);
        if (G.tex[stage]) h_D3DResource_Release((XResource *)G.tex[stage]);
        G.tex[stage] = t;
    }
    return 0;
}
static u32 STDCALL h_D3DDevice_SetPalette(u32 stage, XResource *p) { if (stage < 4) G.pal[stage] = p; return 0; }

/* ---- vertex data ---- */
static u32 STDCALL h_D3DDevice_CreateVertexBuffer2(u32 length)
{
    XResource *vb = calloc(1, sizeof *vb);
    vb->Common = 1 | XCOMMON_TYPE_VB | XCOMMON_D3DCREATED;
    vb->Data = (u32)(uintptr_t)gcalloc(length + 16);
    return (u32)(uintptr_t)vb;
}
static u32 STDCALL h_D3DVertexBuffer_Lock2(XResource *vb, u32 flags) { return vb->Data & 0x7FFFFFFFu; }

static u32 STDCALL h_D3DDevice_SetStreamSource(u32 n, XResource *vb, u32 stride)
{
    if (n < 16) { G.streams[n].data = vb ? (vb->Data & 0x7FFFFFFFu) : 0; G.streams[n].stride = stride; }
    G.use_direct = 0;
    return 0;
}
/* D3D__IndexData: the game's inline DrawIndexedPrimitive reads it and adds StartIndex*2. */
static u32 g_index_data_addr;
static u32 STDCALL h_D3DDevice_SetIndices(XResource *ib, u32 base_vertex)
{
    G.base_vertex = base_vertex;
    if (g_index_data_addr) wr32(g_index_data_addr, ib ? (ib->Data & 0x7FFFFFFFu) : 0);
    return 0;
}

/* X_VERTEXATTRIBUTEFORMAT { struct { DWORD StreamIndex, Offset, Format; BYTE TessType, TessSource, pad[2]; } Slots[16]; }
 * X_STREAMINPUT { XResource *VertexBuffer; DWORD Stride; DWORD Offset; } */
static u32 STDCALL h_D3DDevice_SetVertexShaderInputDirect(u32 *vaf, u32 nstreams, u32 *inputs)
{
    if (!vaf) { G.use_direct = 0; return 0; }          /* NULL: back to the shader's own declaration */
    memset(G.direct_attrs, 0, sizeof G.direct_attrs);
    for (int i = 0; i < 16; i++) {
        u32 *slot = vaf + 4 * i;     /* X_VERTEXSHADERINPUT: StreamIndex, Offset, Format, {TessType, TessSource, pad, pad} */
        if (slot[2] == 0x02 || slot[2] == 0) continue;     /* NONE */
        G.direct_attrs[i].present = 1;
        G.direct_attrs[i].stream = (u8)slot[0];
        G.direct_attrs[i].offset = (u16)slot[1];
        G.direct_attrs[i].type = slot[2];
    }
    for (u32 s = 0; s < nstreams && s < 16; s++) {
        if (!inputs) break;
        XResource *vb = (XResource *)(uintptr_t)inputs[3 * s];
        G.streams[s].data = vb ? (vb->Data & 0x7FFFFFFFu) + inputs[3 * s + 2] : 0;
        G.streams[s].stride = inputs[3 * s + 1];
    }
    G.use_direct = 1;
    return 0;
}

/* ---- vertex shaders ---- */
static u64 fnv64(const void *p, size_t n)
{
    const u8 *b = p; u64 h = 1469598103934665603ULL;
    while (n--) { h ^= *b++; h *= 1099511628211ULL; }
    return h;
}

static void parse_declaration(struct vs_obj *vs, const u32 *decl)
{
    int stream = 0;
    u32 offset[16] = { 0 };
    for (int guard = 0; guard < 256; guard++) {
        u32 tok = *decl++;
        if (tok == 0xFFFFFFFF) break;
        u32 type = tok >> 29;
        switch (type) {
        case 0: break;                                           /* NOP */
        case 1: stream = tok & 0xF; break;                       /* STREAM */
        case 2:                                                  /* STREAMDATA */
            if (tok & 0x10000000) {                              /* SKIP */
                u32 cnt = (tok >> 16) & 0xF;
                offset[stream] += (tok & 0x08000000) ? cnt : cnt * 4;
            } else {
                u32 dtype = (tok >> 16) & 0xFF, reg = tok & 0x1F;
                if (reg < 16) {
                    vs->attrs[reg].present = dtype != 0x02;
                    vs->attrs[reg].stream = (u8)stream;
                    vs->attrs[reg].offset = (u16)offset[stream];
                    vs->attrs[reg].type = dtype;
                }
                u32 sz;
                switch (dtype & 0x0F) {
                case 0x2: sz = (dtype == 0x72) ? 12 : ((dtype >> 4) & 0xF) * 4; break;  /* FLOATn / FLOAT2H */
                case 0x0: sz = 4; break;                                               /* D3DCOLOR */
                case 0x5: case 0x1: sz = ((dtype >> 4) & 0xF) * 2; break;               /* SHORTn / NORMSHORTn */
                case 0x4: sz = (dtype >> 4) & 0xF; break;                              /* PBYTEn */
                case 0x6: sz = 4; break;                                               /* NORMPACKED3 */
                default: sz = 0;
                }
                if (dtype == 0x02) sz = 0;
                offset[stream] += sz;
            }
            break;
        case 3: break;                                           /* TESSELLATOR */
        case 4: {                                                /* CONSTMEM */
            u32 cnt = (tok >> 25) & 0xF;
            u32 *ct = malloc((2 + cnt * 4) * 4);
            ct[0] = tok & 0xFF; ct[1] = cnt;
            memcpy(ct + 2, decl, cnt * 16);
            free(vs->const_tokens);
            vs->const_tokens = ct; vs->nconst_tokens = cnt;
            decl += cnt * 4;
            break; }
        case 5: decl += (tok >> 24) & 0x1F; break;               /* EXT */
        default: break;
        }
    }
}

static u32 STDCALL h_D3DDevice_CreateVertexShader(const u32 *decl, const u32 *func, u32 *handle, u32 usage)
{
    struct vs_obj *vs = calloc(1, sizeof *vs);
    vs->magic = VS_MAGIC;
    if (decl) parse_declaration(vs, decl);
    if (func) {
        vs->type = func[0] & 0xFFFF;
        vs->ninstr = func[0] >> 16;
        vs->prog = malloc(vs->ninstr * 16);
        memcpy(vs->prog, func + 1, vs->ninstr * 16);
        vs->has_program = 1;
        vs->hash = fnv64(vs->prog, vs->ninstr * 16);
    }
    *handle = (u32)(uintptr_t)vs | 1;
    { extern void gl_dump_vs(struct vs_obj *); gl_dump_vs(vs); }
    DTRACE("CreateVertexShader(type %#x, %d instr) -> %08x\n", vs->type, vs->ninstr, *handle);
    return 0;
}

/* NV2A vertex program memory: 136 instruction slots. Programs are loaded at an address
 * (LoadVertexShader / SetVertexShader -> 0) and execution starts at the selected address
 * (SelectVertexShader / SetVertexShader -> 0) and runs to the FINAL instruction. */
#define VSMEM_SLOTS 136
static u32 g_vsmem[VSMEM_SLOTS * 4];
static u32 g_vs_start;
#define VPROG_CACHE 1024
static struct vs_obj *g_vprog_cache[VPROG_CACHE];

static void vsmem_load(const struct vs_obj *vs, u32 addr)
{
    if (!vs->has_program || addr >= VSMEM_SLOTS) return;
    u32 n = vs->ninstr;
    if (addr + n > VSMEM_SLOTS) n = VSMEM_SLOTS - addr;
    memcpy(g_vsmem + addr * 4, vs->prog, n * 16);
}

static void vsmem_resolve(void)
{
    u32 a = g_vs_start, n = 0;
    while (a + n < VSMEM_SLOTS) { n++; if (g_vsmem[(a + n - 1) * 4 + 3] & 1) break; }
    if (!n) { G.vprog = NULL; return; }
    u64 h = fnv64(g_vsmem + a * 4, n * 16);
    u32 slot = (u32)(h % VPROG_CACHE);
    for (u32 k = 0; k < VPROG_CACHE; k++, slot = (slot + 1) % VPROG_CACHE) {
        struct vs_obj *o = g_vprog_cache[slot];
        if (!o) break;
        if (o->hash == h && o->ninstr == (int)n) { G.vprog = o; return; }
    }
    struct vs_obj *o = calloc(1, sizeof *o);
    o->magic = VS_MAGIC; o->has_program = 1; o->ninstr = (int)n; o->hash = h;
    o->prog = malloc(n * 16); memcpy(o->prog, g_vsmem + a * 4, n * 16);
    if (!g_vprog_cache[slot]) g_vprog_cache[slot] = o;
    G.vprog = o;
}

static struct vs_obj *vs_from_handle(u32 handle)
{
    if (!(handle & 1)) return NULL;
    struct vs_obj *vs = (struct vs_obj *)(uintptr_t)(handle & ~1u);
    if (vs->magic != VS_MAGIC) { klog("vertex shader: bad handle %08x\n", handle); return NULL; }
    return vs;
}

static void vs_apply_const_tokens(const struct vs_obj *vs)
{
    if (!vs->const_tokens) return;
    u32 a = vs->const_tokens[0], n = vs->const_tokens[1];
    for (u32 i = 0; i < n && a + i < 192; i++) memcpy(G.vsc[a + i], vs->const_tokens + 2 + 4 * i, 16);
    G.vsc_dirty = 1;
}

static u32 STDCALL h_D3DDevice_LoadVertexShader(u32 handle, u32 address)
{
    struct vs_obj *vs = vs_from_handle(handle);
    if (vs) { vsmem_load(vs, address); vsmem_resolve(); }
    return 0;
}

static u32 STDCALL h_D3DDevice_SelectVertexShader(u32 handle, u32 address)
{
    if (handle) {
        struct vs_obj *vs = vs_from_handle(handle);
        if (vs) { G.vs = vs; G.fvf = 0; vs_apply_const_tokens(vs); }
    }
    g_vs_start = address < VSMEM_SLOTS ? address : 0;
    G.use_direct = 0;
    vsmem_resolve();
    return 0;
}

static u32 STDCALL h_D3DDevice_SetVertexShader(u32 handle)
{
    G.vs_handle = handle;
    if (handle & 1) {
        G.vs = vs_from_handle(handle);
        if (G.vs) {
            vs_apply_const_tokens(G.vs);
            if (G.vs->has_program) { vsmem_load(G.vs, 0); g_vs_start = 0; vsmem_resolve(); }
        }
        G.fvf = 0;
    } else {
        G.vs = NULL;
        G.fvf = handle;
    }
    G.use_direct = 0;
    return 0;
}

static int trace_const(void)
{
    static int f = -2;
    if (f == -2) f = getenv("XBOX_TRACE_PUSH") ? (int)parse_u32(getenv("XBOX_TRACE_PUSH"), 10) : -1;
    return f >= 0 && G.frame == (u32)f;
}
static u32 FASTCALL h_SetVSConst1Fast(u32 reg, const float *data)
{
    if (trace_const()) klog("SetVSConst1Fast(%u) %g %g %g %g\n", reg, data[0], data[1], data[2], data[3]);
    if (reg < 192) { memcpy(G.vsc[reg], data, 16); G.vsc_dirty = 1; }
    return 0;
}
static u32 FASTCALL h_SetVSConstNotInlineFast(u32 reg, const float *data, u32 count)
{
    if (getenv("XBOX_TRACE_CALLERS") && reg == 96) {
        static u32 seen[32]; static int ns;
        u32 ra = (u32)(uintptr_t)__builtin_return_address(0);
        int k; for (k = 0; k < ns && seen[k] != ra; k++) ;
        if (k == ns && ns < 32) { seen[ns++] = ra; klog("vsconst96 caller %08x data %p count %u\n", ra, (void *)data, count); }
    }
    if (trace_const()) klog("SetVSConstNotInlineFast(%u, count %u) %g %g %g %g\n", reg, count, data[0], data[1], data[2], data[3]);
    /* count is in DWORDs (the native code pushes it straight into the method header) */
    for (u32 i = 0; i < count && reg * 4 + i < 192 * 4; i++) memcpy(&G.vsc[0][0] + reg * 4 + i, data + i, 4);
    G.vsc_dirty = 1;
    return 0;
}
/* gcc fastcall: callee pops the stack argument, matching MSVC __fastcall */
static u32 STDCALL h_D3DDevice_SetShaderConstantMode(u32 mode) { klog("SetShaderConstantMode(%#x)\n", mode); return 0; }

/* ---- pixel shaders ---- */
static u32 STDCALL h_D3DDevice_CreatePixelShader(const u32 *def, u32 *handle)
{
    struct ps_obj *ps = calloc(1, sizeof *ps);
    ps->magic = PS_MAGIC;
    memcpy(ps->def, def, sizeof ps->def);
    *handle = (u32)(uintptr_t)ps;
    return 0;
}
static u32 STDCALL h_D3DDevice_SetPixelShader(u32 handle)
{
    struct ps_obj *ps = (struct ps_obj *)(uintptr_t)handle;
    if (ps && ps->magic != PS_MAGIC) { klog("SetPixelShader: bad handle %08x\n", handle); ps = NULL; }
    G.ps = ps;
    if (ps) {
        memcpy(G.rs, ps->def, 57 * 4);           /* PS registers mirror the active definition */
        G.rs[XD3DRS_PSTEXTUREMODES] = ps->def[54];  /* PSTextureModes lives in its own render state */
    }
    return 0;
}
static u32 STDCALL h_D3DDevice_SetPixelShaderConstant(u32 reg, const float *data, u32 count)
{
    /* As the XDK does: convert to D3DCOLOR and store into every PSConstant0/1 and final-combiner
     * constant render state that the current shader's C0/C1/FC mappings route to this register. */
    for (u32 i = 0; i < count && reg + i < 16; i++) {
        u32 r = reg + i;
        memcpy(G.psc[r], data + 4 * i, 16);
        if (!G.ps) continue;
        u32 c[4];
        for (int k = 0; k < 4; k++) {
            float f = data[4 * i + k];
            if (!(f > 0)) f = 0;
            if (f > 1) f = 1;
            c[k] = (u32)(f * 255.0f + 0.5f);
        }
        u32 col = c[0] << 16 | c[1] << 8 | c[2] | c[3] << 24;
        const u32 *d = G.ps->def;
        for (int k = 0; k < 8; k++) {
            if (((d[57] >> (4 * k)) & 0xF) == r) G.rs[XD3DRS_PSCONSTANT0_0 + k] = col;
            if (((d[58] >> (4 * k)) & 0xF) == r) G.rs[XD3DRS_PSCONSTANT1_0 + k] = col;
        }
        if ((d[59] & 0xF) == r) G.rs[XD3DRS_PSFINALCOMBINERCONSTANT0] = col;
        if (((d[59] >> 4) & 0xF) == r) G.rs[XD3DRS_PSFINALCOMBINERCONSTANT1] = col;
    }
    return 0;
}

/* ---- draws ---- */
static u32 STDCALL h_D3DDevice_DrawVerticesUP(u32 prim, u32 count, const u8 *data, u32 stride)
{
    gl_draw(prim, count, NULL, data, stride);
    return 0;
}
static u32 STDCALL h_D3DDevice_DrawIndexedVertices(u32 prim, u32 count, const u16 *indices)
{
    gl_draw(prim, count, indices, NULL, 0);
    return 0;
}

/* ================= install ================= */
struct hook { const char *name; void *fn; };

void d3d_install(void)
{
    g_d3d_trace = getenv("XBOX_D3D_TRACE") != NULL;
    G.rs = (u32 *)(uintptr_t)hle_sym("D3D_g_RenderState");
    G.tss = (u32 (*)[XTSS_COUNT])(uintptr_t)hle_sym("D3D_g_DeferredTextureState");
    g_dev_ptr_addr = hle_sym("D3D_g_pDevice");
    if (!G.rs || !G.tss || !g_dev_ptr_addr) { klog("d3d: required symbols missing, graphics HLE disabled\n"); return; }
    g_fake_device = calloc(1, 0x4000);
    /* In XDK 5849 the D3D globals run: IndexData, dirty flags, TextureState[4][32], RenderState[] */
    g_index_data_addr = (u32)(uintptr_t)G.tss - 0xC;
    klog("d3d: render states %p, texture states %p, index data %08x\n", (void *)G.rs, (void *)G.tss, g_index_data_addr);
    static const struct hook hooks[] = {
        { "Direct3D_CreateDevice", h_Direct3D_CreateDevice },
        { "D3DDevice_Reset", h_D3DDevice_Reset },
        { "D3DDevice_Release", h_D3DDevice_Release },
        { "D3D_SetPushBufferSize", h_D3D_SetPushBufferSize },
        { "D3DDevice_GetBackBuffer2", h_D3DDevice_GetBackBuffer2 },
        { "D3DDevice_GetRenderTarget2", h_D3DDevice_GetRenderTarget2 },
        { "D3DDevice_GetDepthStencilSurface2", h_D3DDevice_GetDepthStencilSurface2 },
        { "D3DDevice_SetRenderTarget", h_D3DDevice_SetRenderTarget },
        { "D3DDevice_Clear", h_D3DDevice_Clear },
        { "D3DDevice_Swap", h_D3DDevice_Swap },
        { "D3DDevice_SetVerticalBlankCallback", h_D3DDevice_SetVerticalBlankCallback },
        { "D3DDevice_InsertCallback", h_D3DDevice_InsertCallback },
        { "D3DDevice_BeginPush_4", h_D3DDevice_BeginPush_4 },
        { "D3DDevice_EndPush", h_D3DDevice_EndPush },
        { "D3DDevice_SetGammaRamp", h_D3DDevice_SetGammaRamp },
        { "D3DDevice_GetDisplayMode", h_D3DDevice_GetDisplayMode },
        { "D3DDevice_GetDisplayFieldStatus", h_D3DDevice_GetDisplayFieldStatus },
        { "D3DDevice_SetRenderState_Simple", h_SetRenderState_Simple },
        { "D3DDevice_SetRenderStateNotInline", h_D3DDevice_SetRenderStateNotInline },
        { "D3DDevice_SetRenderState_FogColor", h_RS_FogColor },
        { "D3DDevice_SetRenderState_CullMode", h_RS_CullMode },
        { "D3DDevice_SetRenderState_FillMode", h_RS_FillMode },
        { "D3DDevice_SetRenderState_ZEnable", h_RS_ZEnable },
        { "D3DDevice_SetRenderState_YuvEnable", h_RS_YuvEnable },
        { "D3DDevice_SetRenderState_OcclusionCullEnable", h_RS_OcclusionCullEnable },
        { "D3DDevice_SetRenderState_DoNotCullUncompressed", h_RS_DoNotCullUncompressed },
        { "D3DDevice_SetTextureState_BorderColor", h_TS_BorderColor },
        { "D3DDevice_SetTextureState_TexCoordIndex", h_TS_TexCoordIndex },
        { "D3DDevice_SetDepthClipPlanes", h_D3DDevice_SetDepthClipPlanes },
        { "D3DDevice_SetScissors", h_D3DDevice_SetScissors },
        { "D3DDevice_SetScreenSpaceOffset", h_D3DDevice_SetScreenSpaceOffset },
        { "D3DDevice_CreateTexture2", h_D3DDevice_CreateTexture2 },
        { "D3DTexture_GetSurfaceLevel2", h_D3DTexture_GetSurfaceLevel2 },
        { "D3DTexture_LockRect", h_D3DTexture_LockRect },
        { "D3DSurface_LockRect", h_D3DSurface_LockRect },
        { "D3DResource_Register", h_D3DResource_Register },
        { "D3DResource_Release", h_D3DResource_Release },
        { "D3DResource_BlockUntilNotBusy", h_D3DResource_BlockUntilNotBusy },
        { "D3DDevice_SetTexture", h_D3DDevice_SetTexture },
        { "D3DDevice_SetPalette", h_D3DDevice_SetPalette },
        { "D3DDevice_CreateVertexBuffer2", h_D3DDevice_CreateVertexBuffer2 },
        { "D3DVertexBuffer_Lock2", h_D3DVertexBuffer_Lock2 },
        { "D3DDevice_SetStreamSource", h_D3DDevice_SetStreamSource },
        { "D3DDevice_SetIndices", h_D3DDevice_SetIndices },
        { "D3DDevice_SetVertexShaderInputDirect", h_D3DDevice_SetVertexShaderInputDirect },
        { "D3DDevice_CreateVertexShader", h_D3DDevice_CreateVertexShader },
        { "D3DDevice_LoadVertexShader", h_D3DDevice_LoadVertexShader },
        { "D3DDevice_SelectVertexShader", h_D3DDevice_SelectVertexShader },
        { "D3DDevice_SetVertexShader", h_D3DDevice_SetVertexShader },
        { "D3DDevice_SetVertexShaderConstant1Fast", h_SetVSConst1Fast },
        { "D3DDevice_SetVertexShaderConstantNotInlineFast", h_SetVSConstNotInlineFast },
        { "D3DDevice_SetShaderConstantMode", h_D3DDevice_SetShaderConstantMode },
        { "D3DDevice_CreatePixelShader", h_D3DDevice_CreatePixelShader },
        { "D3DDevice_SetPixelShader", h_D3DDevice_SetPixelShader },
        { "D3DDevice_SetPixelShaderConstant", h_D3DDevice_SetPixelShaderConstant },
        { "D3DDevice_DrawVerticesUP", h_D3DDevice_DrawVerticesUP },
        { "D3DDevice_DrawIndexedVertices", h_D3DDevice_DrawIndexedVertices },
    };
    int ok = 0;
    for (size_t i = 0; i < sizeof hooks / sizeof hooks[0]; i++) ok += hle_hook(hooks[i].name, hooks[i].fn);
    klog("d3d: %d/%zu graphics functions replaced\n", ok, sizeof hooks / sizeof hooks[0]);

    /* Unnamed by the symbol scan; identified by hand for this build (see NOTES.md). */
    struct { u32 addr; const u8 sig[6]; void *fn; const char *what; } manual[] = {
        { 0x2838b0, { 0xA1, 0x08, 0x39, 0x29, 0x00, 0x8B }, h_KickAndWait, "KickOffAndWait" },
        { 0x286330, { 0xA1, 0x08, 0x39, 0x29, 0x00, 0xD9 }, h_GetScreenSpaceOffset, "GetScreenSpaceOffset" },
    };
    for (size_t i = 0; i < sizeof manual / sizeof manual[0]; i++) {
        if (!memcmp((void *)(uintptr_t)manual[i].addr, manual[i].sig, 6)) hle_patch_jmp(manual[i].addr, manual[i].fn);
        else klog("d3d: %s signature mismatch at %08x, not hooked\n", manual[i].what, manual[i].addr);
    }
    (void)h_D3DDevice_GetDeviceCaps_unused;
}
