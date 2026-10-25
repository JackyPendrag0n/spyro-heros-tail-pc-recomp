/* OpenGL 3.3 core backend (SDL2 window). */
#define _GNU_SOURCE
#include "gfx.h"
#include <SDL2/SDL.h>
#include <GL/glcorearb.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

int g_gfx_null;
void input_sdl_init(void);
void input_sdl_event(SDL_Event *e);
void input_sdl_poll(void);

/* ---- GL function loading ---- */
#define GLFUNCS \
    X(PFNGLCLEARPROC, glClear) X(PFNGLCLEARCOLORPROC, glClearColor) X(PFNGLCLEARDEPTHPROC, glClearDepth) \
    X(PFNGLCLEARSTENCILPROC, glClearStencil) X(PFNGLVIEWPORTPROC, glViewport) X(PFNGLSCISSORPROC, glScissor) \
    X(PFNGLENABLEPROC, glEnable) X(PFNGLDISABLEPROC, glDisable) X(PFNGLBLENDFUNCPROC, glBlendFunc) \
    X(PFNGLBLENDEQUATIONPROC, glBlendEquation) X(PFNGLBLENDCOLORPROC, glBlendColor) X(PFNGLDEPTHFUNCPROC, glDepthFunc) \
    X(PFNGLDEPTHMASKPROC, glDepthMask) X(PFNGLCOLORMASKPROC, glColorMask) X(PFNGLCULLFACEPROC, glCullFace) \
    X(PFNGLFRONTFACEPROC, glFrontFace) X(PFNGLPOLYGONMODEPROC, glPolygonMode) X(PFNGLPOLYGONOFFSETPROC, glPolygonOffset) \
    X(PFNGLSTENCILFUNCPROC, glStencilFunc) X(PFNGLSTENCILOPPROC, glStencilOp) X(PFNGLSTENCILMASKPROC, glStencilMask) \
    X(PFNGLGENTEXTURESPROC, glGenTextures) X(PFNGLDELETETEXTURESPROC, glDeleteTextures) X(PFNGLBINDTEXTUREPROC, glBindTexture) \
    X(PFNGLTEXIMAGE2DPROC, glTexImage2D) X(PFNGLTEXIMAGE3DPROC, glTexImage3D) X(PFNGLTEXPARAMETERIPROC, glTexParameteri) \
    X(PFNGLTEXPARAMETERFPROC, glTexParameterf) X(PFNGLTEXPARAMETERFVPROC, glTexParameterfv) \
    X(PFNGLACTIVETEXTUREPROC, glActiveTexture) X(PFNGLGENBUFFERSPROC, glGenBuffers) X(PFNGLBINDBUFFERPROC, glBindBuffer) \
    X(PFNGLBUFFERDATAPROC, glBufferData) X(PFNGLGENVERTEXARRAYSPROC, glGenVertexArrays) X(PFNGLBINDVERTEXARRAYPROC, glBindVertexArray) \
    X(PFNGLVERTEXATTRIBPOINTERPROC, glVertexAttribPointer) X(PFNGLENABLEVERTEXATTRIBARRAYPROC, glEnableVertexAttribArray) \
    X(PFNGLDISABLEVERTEXATTRIBARRAYPROC, glDisableVertexAttribArray) X(PFNGLVERTEXATTRIB4FPROC, glVertexAttrib4f) \
    X(PFNGLDRAWARRAYSPROC, glDrawArrays) X(PFNGLDRAWELEMENTSPROC, glDrawElements) \
    X(PFNGLCREATESHADERPROC, glCreateShader) X(PFNGLSHADERSOURCEPROC, glShaderSource) X(PFNGLCOMPILESHADERPROC, glCompileShader) \
    X(PFNGLGETSHADERIVPROC, glGetShaderiv) X(PFNGLGETSHADERINFOLOGPROC, glGetShaderInfoLog) X(PFNGLCREATEPROGRAMPROC, glCreateProgram) \
    X(PFNGLATTACHSHADERPROC, glAttachShader) X(PFNGLLINKPROGRAMPROC, glLinkProgram) X(PFNGLGETPROGRAMIVPROC, glGetProgramiv) \
    X(PFNGLGETPROGRAMINFOLOGPROC, glGetProgramInfoLog) X(PFNGLUSEPROGRAMPROC, glUseProgram) X(PFNGLDELETESHADERPROC, glDeleteShader) \
    X(PFNGLBINDATTRIBLOCATIONPROC, glBindAttribLocation) X(PFNGLGETUNIFORMLOCATIONPROC, glGetUniformLocation) \
    X(PFNGLUNIFORM1IPROC, glUniform1i) X(PFNGLUNIFORM1FPROC, glUniform1f) X(PFNGLUNIFORM2FVPROC, glUniform2fv) \
    X(PFNGLUNIFORM4FVPROC, glUniform4fv) X(PFNGLUNIFORM2FPROC, glUniform2f) \
    X(PFNGLGENFRAMEBUFFERSPROC, glGenFramebuffers) X(PFNGLBINDFRAMEBUFFERPROC, glBindFramebuffer) \
    X(PFNGLFRAMEBUFFERTEXTURE2DPROC, glFramebufferTexture2D) X(PFNGLGENRENDERBUFFERSPROC, glGenRenderbuffers) \
    X(PFNGLBINDRENDERBUFFERPROC, glBindRenderbuffer) X(PFNGLRENDERBUFFERSTORAGEPROC, glRenderbufferStorage) \
    X(PFNGLFRAMEBUFFERRENDERBUFFERPROC, glFramebufferRenderbuffer) X(PFNGLCHECKFRAMEBUFFERSTATUSPROC, glCheckFramebufferStatus) \
    X(PFNGLBLITFRAMEBUFFERPROC, glBlitFramebuffer) X(PFNGLGETERRORPROC, glGetError) X(PFNGLFLUSHPROC, glFlush) \
    X(PFNGLPIXELSTOREIPROC, glPixelStorei) X(PFNGLGETSTRINGPROC, glGetString) X(PFNGLLINEWIDTHPROC, glLineWidth) \
    X(PFNGLPOINTSIZEPROC, glPointSize) \
    X(PFNGLGENQUERIESPROC, glGenQueries) X(PFNGLBEGINQUERYPROC, glBeginQuery) X(PFNGLENDQUERYPROC, glEndQuery) \
    X(PFNGLGETQUERYOBJECTUIVPROC, glGetQueryObjectuiv) X(PFNGLREADPIXELSPROC, glReadPixels)
#define X(t, n) static t n;
GLFUNCS
#undef X

static SDL_Window *g_win;
SDL_Window *gl_window(void) { return g_win; }
static int g_diag = -1;
static SDL_GLContext g_ctx;
/* internal resolution: guest pixels -> host pixels, per axis. In widescreen the game draws 16:9
 * anamorphically into 640x480, so the horizontal scale is 4/3 of the vertical (square pixels). */
static float g_sx = 2, g_sy = 2;
static int g_widescreen = 1;
static inline int SX(float v) { return (int)lroundf(v * g_sx); }
static inline int SY(float v) { return (int)lroundf(v * g_sy); }
int gl_widescreen(void) { return g_widescreen; }
int g_show_fps;
void ov_begin(int *w, int *h);
void ov_rect(float x, float y, float w, float h, u32 rgba);
float ov_text(float x, float y, float size, u32 rgba, const char *s);
void ov_end(void);
static u32 g_vao, g_vbo, g_ebo;
static GLuint g_tex_dummy2d, g_tex_dummycube, g_tex_dummy3d;
static u64 g_draws, g_draws_frame, g_gl_errors, g_shader_fail;
/* XBOX_DIAG=1: per-render-target pixel statistics (numbers only) */
struct diag_rt { u32 data, w, h; u32 draws, zero_draws, clears; u64 samples; };
static struct diag_rt g_diag_rt[16]; static int g_diag_n;
static GLuint g_diag_query;
static int g_diag_logged_zero;
static double g_t_tex, g_t_prog, g_t_vtx, g_t_submit, g_t_swap, g_t_present_wait;
static u64 g_tex_uploads, g_tex_upload_bytes;
static int getenv_debug_gl(void) { static int v = -1; if (v < 0) v = getenv("XBOX_GL_DEBUG") != NULL; return v; }
static double tnow(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static FILE *g_shader_log;

/* ---- render targets ---- */
struct target {
    u32 data;           /* guest address of colour data (0 = backbuffer) */
    u32 w, h;
    int pw, ph;         /* host pixel size */
    GLuint fbo, color, depth;
    int used;
    float fx, fy;       /* the part of it in use (split screen: each view is drawn at its size, see gl_set_view_fraction) */
};
#define MAXRT 32
static struct target g_rts[MAXRT];
static struct target *g_cur_rt;
static u32 g_cur_w, g_cur_h;
static float g_zscale = 16777215.0f;
static float g_rfx = 1, g_rfy = 1;
static int rt_vw(const struct target *t) { int v = (int)lroundf(t->pw * t->fx); return v < 1 ? 1 : v; }
static int rt_vh(const struct target *t) { int v = (int)lroundf(t->ph * t->fy); return v < 1 ? 1 : v; }
/* scissor / probe coordinates in the current target */
static inline int CX(float v) { return (int)lroundf(v * g_sx * (g_cur_rt ? g_cur_rt->fx : 1)); }
static inline int CY(float v) { return (int)lroundf(v * g_sy * (g_cur_rt ? g_cur_rt->fy : 1)); }
/* co-op split screen: render targets bound from now on are drawn in their lower-left fx x fy part
 * only (a half-width view: 0.5 x 1, a quarter: 0.5 x 0.5), so a view costs the pixels it is shown
 * at instead of a whole screen's. Sampling a render target scales the coordinates the same way. */
void gl_set_view_fraction(float fx, float fy)
{
    g_rfx = fx; g_rfy = fy;
    if (g_cur_rt && (g_cur_rt->fx != fx || g_cur_rt->fy != fy)) { g_cur_rt->fx = fx; g_cur_rt->fy = fy; glViewport(0, 0, rt_vw(g_cur_rt), rt_vh(g_cur_rt)); }
}

static struct target *rt_get(u32 data, u32 w, u32 h)
{
    struct target *free_slot = NULL;
    for (int i = 0; i < MAXRT; i++) {
        if (g_rts[i].used && g_rts[i].data == data && g_rts[i].w == w && g_rts[i].h == h) return &g_rts[i];
        if (!g_rts[i].used && !free_slot) free_slot = &g_rts[i];
    }
    if (!free_slot) { klog("gl: render target table full\n"); return &g_rts[0]; }
    struct target *t = free_slot;
    memset(t, 0, sizeof *t);
    t->used = 1; t->data = data; t->w = w; t->h = h; t->fx = t->fy = 1;
    t->pw = SX(w); t->ph = SY(h); if (t->pw < 1) t->pw = 1; if (t->ph < 1) t->ph = 1;
    glGenFramebuffers(1, &t->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
    glGenTextures(1, &t->color);
    glBindTexture(GL_TEXTURE_2D, t->color);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, t->pw, t->ph, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);   /* single level: stays complete under mip filters */
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t->color, 0);
    glGenRenderbuffers(1, &t->depth);
    glBindRenderbuffer(GL_RENDERBUFFER, t->depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, t->pw, t->ph);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, t->depth);
    GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    klog("gl: render target %08x %ux%u (%dx%d) fbo %u status %#x\n", data, w, h, t->pw, t->ph, t->fbo, st);
    return t;
}

void gl_set_render_target(XSurface *rt, XSurface *zs)
{
    if (g_gfx_null || !g_win) return;
    u32 w = G.bb_width, h = G.bb_height, data = 0;
    if (rt) {
        if (rt->Size) { w = (rt->Size & 0xFFF) + 1; h = ((rt->Size >> 12) & 0xFFF) + 1; }
        else { w = 1u << XFMT_USIZE(rt->Format); h = 1u << XFMT_VSIZE(rt->Format); }
        data = rt->Data & 0x7FFFFFFFu;
    }
    g_cur_rt = rt_get(data, w, h);
    g_cur_w = w; g_cur_h = h;
    g_cur_rt->fx = g_rfx; g_cur_rt->fy = g_rfy;
    glBindFramebuffer(GL_FRAMEBUFFER, g_cur_rt->fbo);
    glViewport(0, 0, rt_vw(g_cur_rt), rt_vh(g_cur_rt));
    u32 zf = zs ? XFMT_FMT(zs->Format) : 0x2A;
    g_zscale = (zf == 0x2C || zf == 0x30 || zf == 0x2D || zf == 0x31) ? 65535.0f : 16777215.0f;
}

/* ---- textures ---- */
struct gtex {
    u32 data, format, size, palhash;
    u32 sample_hash;
    GLuint id;
    int kind;            /* 1 2D, 2 linear 2D, 3 cube, 4 3D */
    int is_rt, alpha_one; /* render-target texture; its format has no alpha (sample a = 1) */
    float rfx, rfy;      /* render-target texture: the part of it drawn in (see gl_set_view_fraction) */
    u32 w, h;
    int dirty;
    u32 last_used;
};
#define MAXTEX 4096
static struct gtex g_texs[MAXTEX];

void gl_texture_dirty(u32 data)
{
    data &= 0x7FFFFFFFu;
    for (int i = 0; i < MAXTEX; i++) if (g_texs[i].id && g_texs[i].data == data) g_texs[i].dirty = 1;
}

static u32 sample_hash(const u8 *p, u32 bytes)
{
    u32 h = 2166136261u;
    if (bytes < 4) return h;
    u32 n = bytes / 4, step = n > 256 ? n / 256 : 1;
    const u32 *w = (const u32 *)p;
    for (u32 i = 0; i < n; i += step) { h ^= w[i]; h *= 16777619u; }
    h ^= w[n - 1];
    return h * 16777619u;
}

static void upload_level(GLenum target, int level, u32 fmt, const u8 *src, u32 w, u32 h, u32 pitch, const u32 *pal)
{
    u8 *rgba = malloc((size_t)w * h * 4 + 64);
    tex_decode(fmt, src, w, h, pitch, pal, rgba);
    glTexImage2D(target, level, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    free(rgba);
}

static u32 tex_total_bytes(XPixelContainer *t)
{
    u32 w, h, d; int lv, lin, pitch;
    tex_dims(t, &w, &h, &d, &lv, &lin, &pitch);
    if (t->Size) return pitch * h;
    return tex_level_offset(t->Format, 0, lv, NULL, NULL, NULL) * ((t->Format & XFMT_CUBEMAP) ? 6 : 1);
}

static struct gtex *tex_get(XPixelContainer *t, XResource *pal, int stage)
{
    u32 data = t->Data & 0x7FFFFFFFu;
    if (!data) {
        static int said;
        if (g_diag > 0 && said++ < 5) klog("diag: stage %d samples a texture with no data (format %08x size %08x)\n", stage, t->Format, t->Size);
        return NULL;
    }
    /* render-target textures: use the FBO colour attachment */
    for (int i = 0; i < MAXRT; i++)
        if (g_rts[i].used && g_rts[i].data == data && data) {
            static u32 said_rt[MAXRT];
            if (g_diag > 0 && !said_rt[i]++) klog("diag: stage %d samples render target %08x (%ux%u) fmt %08x size %08x\n",
                                                 stage, data, g_rts[i].w, g_rts[i].h, t->Format, t->Size);
            static struct gtex rtts[4]; struct gtex *rp = &rtts[stage & 3]; struct gtex rtt = *rp;
            rtt.id = g_rts[i].color; rtt.kind = 1; rtt.w = g_rts[i].w; rtt.h = g_rts[i].h;
            if (tex_is_linear(XFMT_FMT(t->Format))) rtt.kind = 2;
            rtt.is_rt = 1; rtt.rfx = g_rts[i].fx; rtt.rfy = g_rts[i].fy;
            { u32 f = XFMT_FMT(t->Format);
              rtt.alpha_one = f == 0x07 || f == 0x1E || f == 0x05 || f == 0x11 || f == 0x03 || f == 0x1C || f == 0x27 || f == 0x37; }
            *rp = rtt;
            return rp;
        }
    u32 fmt = XFMT_FMT(t->Format);
    u32 bytes = tex_total_bytes(t);
    if (bytes == 0 || bytes > 64u << 20) return NULL;
    const u32 *palette = NULL;
    u32 palhash = 0;
    if (fmt == 0x0B && pal && pal->Data) { palette = guestp(pal->Data); palhash = sample_hash((const u8 *)palette, 1024); }
    u32 sh = sample_hash((const u8 *)guestp(data), bytes);
    struct gtex *e = NULL, *victim = NULL;
    for (int i = 0; i < MAXTEX; i++) {
        struct gtex *c = &g_texs[i];
        if (c->id && c->data == data && c->format == t->Format && c->size == t->Size && c->palhash == palhash) { e = c; break; }
        if (!victim || !c->id || (victim->id && c->last_used < victim->last_used)) victim = c;
    }
    if (e && !e->dirty && e->sample_hash == sh) { e->last_used = G.frame; return e; }
    if (!e) {
        e = victim;
        if (e->id) glDeleteTextures(1, &e->id);
        memset(e, 0, sizeof *e);
        glGenTextures(1, &e->id);
    }
    g_tex_uploads++; g_tex_upload_bytes += bytes;
    e->data = data; e->format = t->Format; e->size = t->Size; e->palhash = palhash;
    e->sample_hash = sh; e->dirty = 0; e->last_used = G.frame;
    u32 w, h, d; int lv, lin, pitch;
    tex_dims(t, &w, &h, &d, &lv, &lin, &pitch);
    e->w = w; e->h = h;
    const u8 *src = (const u8 *)guestp(data);
    if (t->Format & XFMT_CUBEMAP) {
        e->kind = 3;
        glBindTexture(GL_TEXTURE_CUBE_MAP, e->id);
        u32 fb = (tex_level_offset(t->Format, 0, lv, NULL, NULL, NULL) + 127) & ~127u;
        for (int f = 0; f < 6; f++)
            for (int l = 0; l < lv; l++) {
                u32 lw, lh, lp, off = tex_level_offset(t->Format, 0, l, &lw, &lh, &lp);
                upload_level(GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, l, fmt, src + f * fb + off, lw, lh, lp, palette);
            }
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAX_LEVEL, lv - 1);
    } else if (d > 1) {
        e->kind = 4;   /* volume textures: base level only for now */
        glBindTexture(GL_TEXTURE_3D, e->id);
        u32 bpp = tex_bpp(fmt) / 8;
        u8 *lin3 = malloc((size_t)w * h * d * (bpp ? bpp : 1));
        u8 *rgba = malloc((size_t)w * h * d * 4);
        if (bpp && !tex_is_dxt(fmt)) {
            tex_unswizzle(src, lin3, w, h, d, bpp);
            for (u32 z = 0; z < d; z++)
                tex_decode(fmt | 0, lin3 + z * w * h * bpp, w, h, w * bpp, palette, rgba + z * w * h * 4);
        } else memset(rgba, 255, (size_t)w * h * d * 4);
        glTexImage3D(GL_TEXTURE_3D, 0, GL_RGBA8, w, h, d, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAX_LEVEL, 0);
        free(lin3); free(rgba);
    } else {
        e->kind = lin ? 2 : 1;
        glBindTexture(GL_TEXTURE_2D, e->id);
        if (t->Size) {
            upload_level(GL_TEXTURE_2D, 0, fmt, src, w, h, pitch, palette);
            lv = 1;
        } else {
            for (int l = 0; l < lv; l++) {
                u32 lw, lh, lp, off = tex_level_offset(t->Format, 0, l, &lw, &lh, &lp);
                upload_level(GL_TEXTURE_2D, l, fmt, src + off, lw, lh, lp, palette);
            }
        }
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, lv - 1);
    }
    return e;
}

static GLint wrap_mode(u32 a)
{
    switch (a) { case 2: return GL_MIRRORED_REPEAT; case 3: case 5: return GL_CLAMP_TO_EDGE; case 4: return GL_CLAMP_TO_BORDER; }
    return GL_REPEAT;
}

static void apply_sampler(GLenum target, int stage, int levels_ok)
{
    const u32 *t = G.tss[stage];
    u32 mag = t[XTSS_MAGFILTER], min = t[XTSS_MINFILTER], mip = t[XTSS_MIPFILTER];
    GLint gmag = mag == 1 ? GL_NEAREST : GL_LINEAR;
    GLint gmin;
    if (!levels_ok || mip == 0) gmin = min == 1 ? GL_NEAREST : GL_LINEAR;
    else if (mip == 1) gmin = min == 1 ? GL_NEAREST_MIPMAP_NEAREST : GL_LINEAR_MIPMAP_NEAREST;
    else gmin = min == 1 ? GL_NEAREST_MIPMAP_LINEAR : GL_LINEAR_MIPMAP_LINEAR;
    glTexParameteri(target, GL_TEXTURE_MAG_FILTER, gmag);
    glTexParameteri(target, GL_TEXTURE_MIN_FILTER, gmin);
    glTexParameteri(target, GL_TEXTURE_WRAP_S, wrap_mode(t[XTSS_ADDRESSU]));
    glTexParameteri(target, GL_TEXTURE_WRAP_T, wrap_mode(t[XTSS_ADDRESSV]));
    glTexParameteri(target, GL_TEXTURE_WRAP_R, wrap_mode(t[XTSS_ADDRESSW]));
    u32 bc = t[XTSS_BORDERCOLOR];
    float b[4] = { (bc >> 16 & 255) / 255.f, (bc >> 8 & 255) / 255.f, (bc & 255) / 255.f, (bc >> 24) / 255.f };
    glTexParameterfv(target, GL_TEXTURE_BORDER_COLOR, b);
}

/* ---- shaders ---- */
struct prog {
    u64 vkey, pkey;
    GLuint id;
    GLint u_c, u_surf, u_zscale, u_wbias, u_pixscale, u_tex[4], u_texc[4], u_tex3[4], u_texScale, u_tfactor, u_fogColor, u_fogParams,
          u_pc0, u_pc1, u_fc0, u_fc1, u_alphaRef, u_alphaFunc;
    int fail;
    u32 const_gen;     /* G constant generation last uploaded to this program */
};
#define MAXPROG 2048
static struct prog g_progs[MAXPROG];
static int g_nprogs;

static const char *vs_common =
    "#version 330 core\n"
    "layout(location=0) in vec4 v0; layout(location=1) in vec4 v1; layout(location=2) in vec4 v2; layout(location=3) in vec4 v3;\n"
    "layout(location=4) in vec4 v4; layout(location=5) in vec4 v5; layout(location=6) in vec4 v6; layout(location=7) in vec4 v7;\n"
    "layout(location=8) in vec4 v8; layout(location=9) in vec4 v9; layout(location=10) in vec4 v10; layout(location=11) in vec4 v11;\n"
    "layout(location=12) in vec4 v12; layout(location=13) in vec4 v13; layout(location=14) in vec4 v14; layout(location=15) in vec4 v15;\n"
    "out vec4 vD0; out vec4 vD1; out vec4 vFog; out vec4 vT0; out vec4 vT1; out vec4 vT2; out vec4 vT3; out float vW;\n"
    "uniform vec2 surfSize; uniform float zScale; uniform vec2 pixScale;\n";

/* screen-space position (pixels, z in [0,zScale], w) -> clip space */
static const char *vs_epilogue =
    /* NV2A rasterizer: 4 fractional bits, truncated (so a D3D +0.53 screen offset lands on the pixel centre) */
    "  vec2 sp = trunc(oPos.xy * 16.0) / 16.0;\n"
    /* 2D draws (w == 1) whose left/top edge sits on the D3D +0.5 pixel offset cover guest pixel
     * 0 on the Xbox; at a higher internal resolution that edge would leave a strip uncovered, so
     * move such border edges onto the target's edge. Nothing else is moved. */
    "  if (abs(oPos.w - 1.0) < 1.0e-6) {\n"
    "    if (sp.x > 0.0 && sp.x <= 0.5) sp.x = 0.0;\n"
    "    if (sp.y > 0.0 && sp.y <= 0.5) sp.y = 0.0;\n"
    "  }\n"
    /* w is kept away from zero and infinity as the hardware does */
    "  float w = oPos.w;\n"
    "  if (w > 0.0 || floatBitsToUint(w) == 0u) w = clamp(w, uintBitsToFloat(0x1F800000u), uintBitsToFloat(0x5F800000u));\n"
    "  else w = clamp(w, uintBitsToFloat(0xDF800000u), uintBitsToFloat(0x9F800000u));\n"
    "  vec3 ndc = vec3(sp.x / (surfSize.x * 0.5) - 1.0, sp.y / (surfSize.y * 0.5) - 1.0, 2.0 * (oPos.z / zScale) - 1.0);\n"
    "  gl_Position = vec4(ndc * w, w);\n"
    "  gl_PointSize = max(oPts.x, 1.0);\n"
    "  vW = w;\n"   /* perspective-correct w for W-buffering */
    "  vD0 = clamp(oD0, 0.0, 1.0); vD1 = clamp(oD1, 0.0, 1.0); vFog = oFog;\n"
    "  vT0 = oT0; vT1 = oT1; vT2 = oT2; vT3 = oT3;\n";

static char *build_vs_program(struct vs_obj *vs)
{
    int writes_c = 0;
    char *body = vsh_translate(vs->prog, vs->ninstr, &writes_c);
    size_t n = strlen(body) + 8192;
    char *src = malloc(n);
    snprintf(src, n,
             "%s%s%s\n%s"
             "void main() {\n"
             "  vec4 R0 = vec4(0.0), R1 = vec4(0.0), R2 = vec4(0.0), R3 = vec4(0.0), R4 = vec4(0.0), R5 = vec4(0.0);\n"
             "  vec4 R6 = vec4(0.0), R7 = vec4(0.0), R8 = vec4(0.0), R9 = vec4(0.0), R10 = vec4(0.0), R11 = vec4(0.0);\n"
             "  vec4 oPos = vec4(0.0, 0.0, 0.0, 1.0);\n"
             "  #define R12 oPos\n"
             "  vec4 oD0 = vec4(0.0, 0.0, 0.0, 1.0), oD1 = vec4(0.0), oFog = vec4(1.0), oPts = vec4(1.0);\n"
             "  vec4 oB0 = vec4(0.0), oB1 = vec4(0.0), oT0 = vec4(0.0, 0.0, 0.0, 1.0), oT1 = vec4(0.0, 0.0, 0.0, 1.0);\n"
             "  vec4 oT2 = vec4(0.0, 0.0, 0.0, 1.0), oT3 = vec4(0.0, 0.0, 0.0, 1.0);\n"
             "  vec4 oUnused1, oUnused2, oUnused13, oUnused14, oUnused15;\n"
             "  vec4 macT = vec4(0.0), iluT = vec4(0.0); int A0 = 0;\n"
             "%s"
             "%s"
             "%s}\n",
             vs_common,
             writes_c ? "uniform vec4 uc[192];\n" : "uniform vec4 c[192];\n",
             "",
             vsh_helpers,
             writes_c ? "  vec4 c[192] = uc;\n" : "",
             body, vs_epilogue);
    free(body);
    return src;
}

/* fixed-function vertex path for FVF handles (pre-transformed or passthrough) */
static char *build_vs_fvf(u32 fvf)
{
    char *src = malloc(4096);
    int rhw = (fvf & 0x00E) == 0x004;
    snprintf(src, 4096,
             "%suniform vec4 c[192];\n"
             "void main() {\n"
             "  vec4 oPos = %s;\n"
             "  vec4 oD0 = %s, oD1 = %s, oFog = vec4(1.0), oPts = vec4(1.0);\n"
             "  vec4 oT0 = v9, oT1 = v10, oT2 = v11, oT3 = v12;\n"
             "%s}\n",
             vs_common,
             rhw ? "vec4(v0.x, v0.y, v0.z * zScale, 1.0 / max(v0.w, 1.0e-30))" : "vec4(v0.xyz, 1.0)",
             (fvf & 0x040) ? "v3" : "vec4(1.0)", (fvf & 0x080) ? "v4" : "vec4(0.0)",
             rhw ? vs_epilogue : "  gl_Position = oPos; vW = oPos.w; vD0 = oD0; vD1 = oD1; vFog = oFog; vT0 = oT0; vT1 = oT1; vT2 = oT2; vT3 = oT3;\n");
    return src;
}

static GLuint compile(GLenum type, const char *src, const char *what)
{
    if (getenv("XBOX_DUMP_SHADERS")) {
        static int n;
        char fn[512];
        snprintf(fn, sizeof fn, "%s/%04d.%s", getenv("XBOX_DUMP_SHADERS"), n++, type == GL_VERTEX_SHADER ? "vert" : "frag");
        FILE *f = fopen(fn, "w");
        if (f) { fputs(src, f); fclose(f); }
    }
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetShaderInfoLog(s, sizeof log, NULL, log);
        klog("gl: %s shader failed to compile: %.300s\n", what, log);
        if (!g_shader_log) g_shader_log = fopen("shader_errors.log", "w");
        if (g_shader_log) { fprintf(g_shader_log, "==== %s\n%s\n---- source\n%s\n", what, log, src); fflush(g_shader_log); }
        g_shader_fail++;
        glDeleteShader(s);
        return 0;
    }
    return s;
}

static int tex_kind_for(int stage, struct gtex **out)
{
    *out = NULL;
    if (!G.tex[stage]) return 0;
    struct gtex *g = tex_get(G.tex[stage], G.pal[stage], stage);
    *out = g;
    return g ? g->kind : 0;
}

static struct prog *get_program(const int kind[4])
{
    struct vs_obj *vp = ((G.vs || G.use_direct) && G.vprog) ? G.vprog : NULL;
    u64 vkey = vp ? vp->hash : G.vs ? (0xF1F0000000000000ULL) : (0xF0F0000000000000ULL | G.fvf);
    { extern int psh_vs_program; psh_vs_program = vp != NULL; }
    u64 pkey = psh_key(G.rs, (const u32 (*)[XTSS_COUNT])G.tss, G.ps, kind);
    static struct prog *last;
    if (last && last->vkey == vkey && last->pkey == pkey) return last->fail ? NULL : last;
    for (int i = 0; i < g_nprogs; i++)
        if (g_progs[i].vkey == vkey && g_progs[i].pkey == pkey) { last = &g_progs[i]; return last->fail ? NULL : last; }
    if (g_nprogs >= MAXPROG) { klog("gl: program cache full\n"); return NULL; }
    u64 unused;
    char *fs = psh_generate(G.rs, (const u32 (*)[XTSS_COUNT])G.tss, G.ps, kind, &unused);
    struct prog *p = &g_progs[g_nprogs++];
    memset(p, 0, sizeof *p);
    p->vkey = vkey; p->pkey = pkey;
    last = p;
    char *vsrc = vp ? build_vs_program(vp) : build_vs_fvf(G.vs ? 0x002 : G.fvf);
    GLuint vs = compile(GL_VERTEX_SHADER, vsrc, "vertex");
    GLuint ps = compile(GL_FRAGMENT_SHADER, fs, "fragment");
    free(vsrc); free(fs);
    if (!vs || !ps) { p->fail = 1; return NULL; }
    p->id = glCreateProgram();
    glAttachShader(p->id, vs); glAttachShader(p->id, ps);
    glLinkProgram(p->id);
    GLint ok = 0;
    glGetProgramiv(p->id, GL_LINK_STATUS, &ok);
    glDeleteShader(vs); glDeleteShader(ps);
    if (!ok) {
        char log[2048]; glGetProgramInfoLog(p->id, sizeof log, NULL, log);
        klog("gl: link failed: %.300s\n", log);
        p->fail = 1; g_shader_fail++;
        return NULL;
    }
    p->u_c = glGetUniformLocation(p->id, "c");
    if (p->u_c < 0) p->u_c = glGetUniformLocation(p->id, "uc");
    p->u_surf = glGetUniformLocation(p->id, "surfSize");
    p->u_zscale = glGetUniformLocation(p->id, "zScale");
    p->u_wbias = glGetUniformLocation(p->id, "wBias");
    p->u_pixscale = glGetUniformLocation(p->id, "pixScale");
    char nm[16];
    for (int i = 0; i < 4; i++) {
        snprintf(nm, sizeof nm, "tex%d", i); p->u_tex[i] = glGetUniformLocation(p->id, nm);
        snprintf(nm, sizeof nm, "tex%dc", i); p->u_texc[i] = glGetUniformLocation(p->id, nm);
        snprintf(nm, sizeof nm, "tex%d3", i); p->u_tex3[i] = glGetUniformLocation(p->id, nm);
    }
    p->u_texScale = glGetUniformLocation(p->id, "texScale");
    p->u_tfactor = glGetUniformLocation(p->id, "tfactor");
    p->u_fogColor = glGetUniformLocation(p->id, "fogColor");
    p->u_fogParams = glGetUniformLocation(p->id, "fogParams");
    p->u_pc0 = glGetUniformLocation(p->id, "pc0");
    p->u_pc1 = glGetUniformLocation(p->id, "pc1");
    p->u_fc0 = glGetUniformLocation(p->id, "fc0");
    p->u_fc1 = glGetUniformLocation(p->id, "fc1");
    p->u_alphaRef = glGetUniformLocation(p->id, "alphaRef");
    p->u_alphaFunc = glGetUniformLocation(p->id, "alphaFunc");
    if (g_nprogs % 50 == 1) klog("gl: %d shader programs compiled\n", g_nprogs);
    return p;
}

static void color4(u32 c, float *o) { o[0] = (c >> 16 & 255) / 255.f; o[1] = (c >> 8 & 255) / 255.f; o[2] = (c & 255) / 255.f; o[3] = (c >> 24) / 255.f; }

/* ---- vertex fetch ---- */
static int attr_fetch(u32 type, const u8 *p, float *o)
{
    o[0] = o[1] = o[2] = 0; o[3] = 1;
    int n = (type >> 4) & 0xF;
    switch (type & 0xF) {
    case 0x2:
        if (type == 0x72) { const float *f = (const float *)p; o[0] = f[0]; o[1] = f[1]; o[2] = 0; o[3] = f[2]; return 1; }
        for (int i = 0; i < n && i < 4; i++) o[i] = ((const float *)p)[i];
        return 1;
    case 0x0: o[0] = p[2] / 255.f; o[1] = p[1] / 255.f; o[2] = p[0] / 255.f; o[3] = p[3] / 255.f; return 1;  /* D3DCOLOR */
    case 0x5: for (int i = 0; i < n && i < 4; i++) o[i] = ((const s16 *)p)[i]; return 1;
    case 0x1: for (int i = 0; i < n && i < 4; i++) { float v = ((const s16 *)p)[i] / 32767.f; o[i] = v < -1 ? -1 : v; } return 1;
    case 0x4: for (int i = 0; i < n && i < 4; i++) o[i] = p[i] / 255.f; return 1;
    case 0x6: {
        u32 v = *(const u32 *)p;
        s32 x = (s32)(v << 21) >> 21, y = (s32)(v << 10) >> 21, z = (s32)v >> 22;
        o[0] = x / 1023.f; o[1] = y / 1023.f; o[2] = z / 511.f; return 1; }
    }
    return 0;
}

static void fvf_layout(u32 fvf, struct vattr *a, u32 *stride)
{
    memset(a, 0, sizeof(struct vattr) * 16);
    u32 off = 0;
    switch (fvf & 0x00E) {
    case 0x002: a[0] = (struct vattr){ 1, 0, off, 0x32 }; off += 12; break;
    case 0x004: a[0] = (struct vattr){ 1, 0, off, 0x42 }; off += 16; break;
    default: if (fvf & 0x00E) { int nb = ((fvf & 0x00E) - 0x004) / 2; a[0] = (struct vattr){ 1, 0, off, 0x32 }; off += 12;
                 if (nb > 0) { a[1] = (struct vattr){ 1, 0, off, (u32)(nb << 4 | 2) }; off += 4 * nb; } }
    }
    if (fvf & 0x010) { a[2] = (struct vattr){ 1, 0, off, 0x32 }; off += 12; }
    if (fvf & 0x040) { a[3] = (struct vattr){ 1, 0, off, 0x40 }; off += 4; }
    if (fvf & 0x080) { a[4] = (struct vattr){ 1, 0, off, 0x40 }; off += 4; }
    int ntex = (fvf >> 8) & 0xF;
    for (int i = 0; i < ntex && i < 4; i++) {
        u32 sz = (fvf >> (16 + 2 * i)) & 3;
        int comps = sz == 0 ? 2 : sz == 1 ? 3 : sz == 2 ? 4 : 1;
        a[9 + i] = (struct vattr){ 1, 0, off, (u32)(comps << 4 | 2) };
        off += 4 * comps;
    }
    *stride = off;
}

static float *g_vtx;
static size_t g_vtx_cap;
static u16 *g_idx;
static size_t g_idx_cap;

static GLenum prim_mode(u32 prim)
{
    switch (prim) {
    case 1: return GL_POINTS; case 2: return GL_LINES; case 3: return GL_LINE_LOOP; case 4: return GL_LINE_STRIP;
    case 5: return GL_TRIANGLES; case 6: return GL_TRIANGLE_STRIP; case 7: return GL_TRIANGLE_FAN;
    case 8: return GL_TRIANGLES; case 9: return GL_TRIANGLE_STRIP; case 10: return GL_TRIANGLE_FAN;
    }
    return GL_TRIANGLES;
}

static void apply_state(void)
{
    const u32 *rs = G.rs;
    if (rs[XD3DRS_ALPHABLENDENABLE]) {
        glEnable(GL_BLEND);
        glBlendFunc(rs[XD3DRS_SRCBLEND], rs[XD3DRS_DESTBLEND]);     /* Xbox values are GL enums */
        u32 op = rs[XD3DRS_BLENDOP];
        glBlendEquation(op == 0xF005 /* ADDSIGNED */ ? GL_FUNC_ADD : op);
        float bc[4]; color4(rs[XD3DRS_BLENDCOLOR], bc);
        glBlendColor(bc[0], bc[1], bc[2], bc[3]);
    } else glDisable(GL_BLEND);
    static int no_depth = -1;
    if (no_depth < 0) no_depth = getenv("XBOX_NO_DEPTH") != NULL;
    if (rs[XD3DRS_ZENABLE] && G.zs && !no_depth) {
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(rs[XD3DRS_ZFUNC]);
    } else glDisable(GL_DEPTH_TEST);
    glDepthMask(rs[XD3DRS_ZWRITEENABLE] && G.zs ? GL_TRUE : GL_FALSE);
    u32 cw = rs[XD3DRS_COLORWRITEENABLE];
    glColorMask((cw >> 16) & 1, (cw >> 8) & 1, cw & 1, (cw >> 24) & 1);
    u32 cull = rs[XD3DRS_CULLMODE];
    if (cull == 0x900 || cull == 0x901) {
        /* we render with y pointing down, so D3D clockwise is GL counter-clockwise */
        glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);
        glFrontFace(cull == 0x900 ? GL_CW : GL_CCW);
    } else glDisable(GL_CULL_FACE);
    if (rs[XD3DRS_STENCILENABLE] && G.zs) {
        glEnable(GL_STENCIL_TEST);
        glStencilFunc(rs[XD3DRS_STENCILFUNC], rs[XD3DRS_STENCILREF], rs[XD3DRS_STENCILMASK]);
        glStencilOp(rs[XD3DRS_STENCILFAIL], rs[XD3DRS_STENCILZFAIL], rs[XD3DRS_STENCILPASS]);
        glStencilMask(rs[XD3DRS_STENCILWRITEMASK]);
    } else glDisable(GL_STENCIL_TEST);
    u32 fill = rs[XD3DRS_FILLMODE];
    glPolygonMode(GL_FRONT_AND_BACK, fill == 0x1B00 ? GL_POINT : fill == 0x1B01 ? GL_LINE : GL_FILL);
    if (rs[XD3DRS_ZBIAS]) { glEnable(GL_POLYGON_OFFSET_FILL); glPolygonOffset(0, -(float)rs[XD3DRS_ZBIAS]); }
    else glDisable(GL_POLYGON_OFFSET_FILL);
    if (G.scissor_count > 0 && !G.scissor_exclusive) {
        glEnable(GL_SCISSOR_TEST);
        const s32 *r = G.scissors[0];
        glScissor(CX(r[0]), CY(r[1]), CX(r[2]) - CX(r[0]), CY(r[3]) - CY(r[1]));
    } else glDisable(GL_SCISSOR_TEST);
}

static struct diag_rt *diag_cur(void)
{
    u32 d = g_cur_rt ? g_cur_rt->data : 0;
    for (int i = 0; i < g_diag_n; i++) if (g_diag_rt[i].data == d) return &g_diag_rt[i];
    if (g_diag_n < 16) { struct diag_rt *r = &g_diag_rt[g_diag_n++]; memset(r, 0, sizeof *r); r->data = d;
        r->w = g_cur_w; r->h = g_cur_h; return r; }
    return &g_diag_rt[15];
}

/* XBOX_DIAG_VTX=frame: run the vertex program on the CPU for the first vertices of each draw
 * in that frame and log inputs and screen-space outputs. */
extern int vsh_cpu_run(const u32 *tokens, int ninstr, const float (*v)[4], const float (*cin)[4], float (*out)[4]);
static void diag_vertices(u32 prim, u32 count, const u16 *indices, u32 vmin, u32 nv, const int *present, int np,
                          const struct vattr *attrs, int is_up)
{
    static int frame = -2;
    if (frame == -2) frame = getenv("XBOX_DIAG_VTX") ? (int)parse_u32(getenv("XBOX_DIAG_VTX"), 10) : -1;
    if (frame < 0 || G.frame != (u32)frame) return;
    static int ndraw; if (ndraw++ > 2000) return;
    klog("vtx draw %d: rt %08x %ux%u vs %016llx fvf %#x prim %u count %u idx %s base %u vmin %u nv %u up %d zscale %g\n",
         ndraw, g_cur_rt ? g_cur_rt->data : 0, g_cur_w, g_cur_h, G.vprog ? (unsigned long long)G.vprog->hash : 0ULL, G.fvf,
         prim, count, indices ? "yes" : "no", G.base_vertex, vmin, nv, is_up, g_zscale);
    klog("   state: blend %u %#x/%#x op %#x | ps %s | s0 cop %u aop %u tex0 %08x | s1 cop %u tex1 %08x | atest %u cw %08x | tf %08x pc0[0] %08x pc0[1] %08x pc1[0] %08x fc %08x/%08x\n",
         G.rs[XD3DRS_ALPHABLENDENABLE], G.rs[XD3DRS_SRCBLEND], G.rs[XD3DRS_DESTBLEND], G.rs[XD3DRS_BLENDOP], G.ps ? "yes" : "no",
         G.tss[0][XTSS_COLOROP], G.tss[0][XTSS_ALPHAOP], G.tex[0] ? G.tex[0]->Data : 0, G.tss[1][XTSS_COLOROP], G.tex[1] ? G.tex[1]->Data : 0,
         G.rs[XD3DRS_ALPHATESTENABLE], G.rs[XD3DRS_COLORWRITEENABLE], G.rs[XD3DRS_TEXTUREFACTOR],
         G.rs[XD3DRS_PSCONSTANT0_0], G.rs[XD3DRS_PSCONSTANT0_1], G.rs[XD3DRS_PSCONSTANT1_0],
         G.rs[XD3DRS_PSFINALCOMBINERCONSTANT0], G.rs[XD3DRS_PSFINALCOMBINERCONSTANT1]);
    for (int k = 0; k < np; k++) {
        const struct vattr *at = &attrs[present[k]];
        klog("   v%d: stream %u off %u type %#x stride %u data %08x\n", present[k], at->stream, at->offset, at->type,
             G.streams[at->stream].stride, G.streams[at->stream].data);
    }
    if (ndraw < 3 || ndraw % 20 == 0) {
        klog("   c58 %g %g %g %g  c59 %g %g %g %g\n", G.vsc[58][0], G.vsc[58][1], G.vsc[58][2], G.vsc[58][3],
             G.vsc[59][0], G.vsc[59][1], G.vsc[59][2], G.vsc[59][3]);
        for (int r = 0; r < 192; r++) if (G.vsc[r][0] != 0 || G.vsc[r][1] != 0 || G.vsc[r][2] != 0 || G.vsc[r][3] != 0)
            klog("   hw c%d (D3D c%d) %g %g %g %g\n", r, r - 96, G.vsc[r][0], G.vsc[r][1], G.vsc[r][2], G.vsc[r][3]);
    }
    for (u32 j = 0; j < 3 && j < count; j++) {
        u32 vi = (indices ? indices[j] : j) - vmin;
        if (vi >= nv) continue;
        float v[16][4] = { { 0 } }, o[16][4];
        for (int a = 0; a < 16; a++) v[a][3] = 1;
        for (int k = 0; k < np; k++) memcpy(v[present[k]], g_vtx + ((size_t)k * nv + vi) * 4, 16);
        char ib[600]; int n = 0;
        for (int k = 0; k < np; k++) n += snprintf(ib + n, sizeof ib - n, " v%d(%.4g %.4g %.4g %.4g)", present[k],
                                                  v[present[k]][0], v[present[k]][1], v[present[k]][2], v[present[k]][3]);
        klog("   in[%u]%s\n", j, ib);
        if ((G.vs || G.use_direct) && G.vprog) {
            vsh_cpu_run(G.vprog->prog, G.vprog->ninstr, (const float (*)[4])v, (const float (*)[4])G.vsc, o);
            klog("   out[%u] oPos (%.6g %.6g %.6g %.6g) oD0 (%.3g %.3g %.3g %.3g) oT0 (%.4g %.4g %.4g %.4g) oT1 (%.4g %.4g %.4g %.4g) oFog %.4g\n", j,
                 o[0][0], o[0][1], o[0][2], o[0][3], o[3][0], o[3][1], o[3][2], o[3][3], o[9][0], o[9][1], o[9][2], o[9][3],
                 o[10][0], o[10][1], o[10][2], o[10][3], o[5][0]);
        }
    }
}

/* XBOX_PROBE=frame,x,y: log every draw in that frame that changes the back-buffer pixel (x,y). */
static void probe_pixel(u32 prim, u32 count, const int *kind, struct gtex **gt)
{
    static int pf = -2, px, py; static u32 last, prt; static int idx;
    if (pf == -2) {
        pf = -1;
        const char *e = getenv("XBOX_PROBE");
        if (e) { int a, b, c; if (sscanf(e, "%d,%d,%d,%x", &a, &b, &c, &prt) >= 3) { pf = a; px = b; py = c; } }
    }
    if (pf < 0 || G.frame != (u32)pf || !g_cur_rt || !G.backbuffer) return;
    if (g_cur_rt->data != (prt ? prt : (G.backbuffer->Data & 0x7FFFFFFFu))) return;
    u8 c[4]; float dz = -1; static float lastz = -2;
    glReadPixels(CX(px), CY(py), 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, c);
    if (G.zs) glReadPixels(CX(px), CY(py), 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, &dz);
    u32 v = c[0] | c[1] << 8 | c[2] << 16 | (u32)c[3] << 24;
    idx++;
    if (prim == 0) { klog("probe clear: pixel %08x depth %.9f\n", v, dz); last = v; lastz = dz; return; }
    if (v == last && dz == lastz) return;
    last = v; lastz = dz;
    klog("probe draw %d: depth now %.9f\n", idx, dz);
    const u32 *t0 = G.tss[0], *t1 = G.tss[1];
    klog("probe draw %d: pixel now %02x%02x%02x a%02x | prim %u n %u vs %016llx ps %s | blend %u %#x/%#x atest %u/%#x ref %u | z %u/%#x zw %u | tex0 %08x fmt %#x kind %d tex1 %08x kind %d | "
         "s0 cop %u %#x %#x aop %u %#x %#x s1 cop %u | fog %u tf %08x cw %08x\n",
         idx, c[0], c[1], c[2], c[3], prim, count, G.vprog ? (unsigned long long)G.vprog->hash : 0ULL, G.ps ? "yes" : "no",
         G.rs[XD3DRS_ALPHABLENDENABLE], G.rs[XD3DRS_SRCBLEND], G.rs[XD3DRS_DESTBLEND], G.rs[XD3DRS_ALPHATESTENABLE],
         G.rs[XD3DRS_ALPHAFUNC], G.rs[XD3DRS_ALPHAREF], G.rs[XD3DRS_ZENABLE], G.rs[XD3DRS_ZFUNC], G.rs[XD3DRS_ZWRITEENABLE],
         G.tex[0] ? G.tex[0]->Data : 0, G.tex[0] ? (G.tex[0]->Format >> 8) & 0xFF : 0, kind[0],
         G.tex[1] ? G.tex[1]->Data : 0, kind[1],
         t0[XTSS_COLOROP], t0[XTSS_COLORARG1], t0[XTSS_COLORARG2], t0[XTSS_ALPHAOP], t0[XTSS_ALPHAARG1], t0[XTSS_ALPHAARG2],
         t1[XTSS_COLOROP], G.rs[XD3DRS_FOGENABLE], G.rs[XD3DRS_TEXTUREFACTOR], G.rs[XD3DRS_COLORWRITEENABLE]);
    klog("probe   fog table %u start %g end %g density %g color %08x | s1 args %#x %#x aop %u | tex1 fmt %#x\n",
         G.rs[XD3DRS_FOGTABLEMODE], *(float *)&G.rs[XD3DRS_FOGSTART], *(float *)&G.rs[XD3DRS_FOGEND],
         *(float *)&G.rs[XD3DRS_FOGDENSITY], G.rs[XD3DRS_FOGCOLOR], t1[XTSS_COLORARG1], t1[XTSS_COLORARG2], t1[XTSS_ALPHAOP],
         G.tex[1] ? (G.tex[1]->Format >> 8) & 0xFF : 0);
    (void)gt;
}

void gl_draw(u32 prim, u32 count, const u16 *indices, const u8 *up_data, u32 up_stride)
{
    g_draws++; g_draws_frame++;
    if (g_gfx_null || !g_win || !count) return;
    if (prim == 0 || prim > 10) return;

    /* vertex attribute layout */
    struct vattr fattrs[16];
    const struct vattr *attrs;
    u32 fvf_stride = 0;
    if (G.use_direct) attrs = G.direct_attrs;
    else if (G.vs) attrs = G.vs->attrs;
    else { fvf_layout(G.fvf, fattrs, &fvf_stride); attrs = fattrs; }

    double t0 = tnow();
    /* vertex range */
    u32 vmin = 0, vmax = count - 1;
    if (indices) {
        vmin = 0xFFFF; vmax = 0;
        for (u32 i = 0; i < count; i++) { if (indices[i] < vmin) vmin = indices[i]; if (indices[i] > vmax) vmax = indices[i]; }
    }
    u32 nv = vmax - vmin + 1;
    int present[16], np = 0;
    for (int a = 0; a < 16; a++) if (attrs[a].present) present[np++] = a;
    size_t need = (size_t)nv * np * 4;
    if (need > g_vtx_cap) { g_vtx_cap = need * 2; g_vtx = realloc(g_vtx, g_vtx_cap * sizeof(float)); }
    for (int k = 0; k < np; k++) {
        const struct vattr *at = &attrs[present[k]];
        const u8 *base; u32 stride;
        if (up_data) { base = up_data; stride = up_stride; }
        else {
            base = (const u8 *)(uintptr_t)G.streams[at->stream].data;
            stride = G.streams[at->stream].stride;
            if (!G.vs && !G.use_direct && !stride) stride = fvf_stride;
        }
        float *dst = g_vtx + (size_t)k * nv * 4;
        if (!base) { for (u32 v = 0; v < nv; v++) { dst[4*v] = dst[4*v+1] = dst[4*v+2] = 0; dst[4*v+3] = 1; } continue; }
        for (u32 v = 0; v < nv; v++) {
            u32 vi = vmin + v + (indices ? G.base_vertex : 0);
            attr_fetch(at->type, base + (size_t)vi * stride + at->offset, dst + 4 * v);
        }
    }
    diag_vertices(prim, count, indices, vmin, nv, present, np, attrs, up_data != NULL);
    glBindVertexArray(g_vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    glBufferData(GL_ARRAY_BUFFER, need * sizeof(float), g_vtx, GL_STREAM_DRAW);
    for (int a = 0; a < 16; a++) glDisableVertexAttribArray(a);
    for (int k = 0; k < np; k++) {
        glEnableVertexAttribArray(present[k]);
        glVertexAttribPointer(present[k], 4, GL_FLOAT, GL_FALSE, 0, (void *)((size_t)k * nv * 16));
    }
    glVertexAttrib4f(3, 1, 1, 1, 1);   /* default diffuse when not supplied */
    double t1 = tnow(); g_t_vtx += t1 - t0;

    /* textures + program */
    int kind[4];
    struct gtex *gt[4];
    for (int i = 0; i < 4; i++) kind[i] = tex_kind_for(i, &gt[i]);
    double t2 = tnow(); g_t_tex += t2 - t1;
    struct prog *p = get_program(kind);
    double t3 = tnow(); g_t_prog += t3 - t2;
    if (!p) return;
    glUseProgram(p->id);
    float scale[4][2];
    for (int i = 0; i < 4; i++) {
        glActiveTexture(GL_TEXTURE0 + i);
        glBindTexture(GL_TEXTURE_2D, (kind[i] == 1 || kind[i] == 2) ? gt[i]->id : g_tex_dummy2d);
        if (kind[i] == 1 || kind[i] == 2) {
            apply_sampler(GL_TEXTURE_2D, i, !G.tex[i]->Size && !gt[i]->is_rt);
            /* render targets are RGBA8 here; formats without alpha read back as opaque */
            if (gt[i]->is_rt) glTexParameteri(GL_TEXTURE_2D, 0x8E45 /* GL_TEXTURE_SWIZZLE_A */, gt[i]->alpha_one ? GL_ONE : GL_ALPHA);
        }
        glActiveTexture(GL_TEXTURE4 + i);
        glBindTexture(GL_TEXTURE_CUBE_MAP, kind[i] == 3 ? gt[i]->id : g_tex_dummycube);
        if (kind[i] == 3) apply_sampler(GL_TEXTURE_CUBE_MAP, i, 1);
        glActiveTexture(GL_TEXTURE8 + i);
        glBindTexture(GL_TEXTURE_3D, kind[i] == 4 ? gt[i]->id : g_tex_dummy3d);
        if (p->u_tex[i] >= 0) glUniform1i(p->u_tex[i], i);
        if (p->u_texc[i] >= 0) glUniform1i(p->u_texc[i], 4 + i);
        if (p->u_tex3[i] >= 0) glUniform1i(p->u_tex3[i], 8 + i);
        scale[i][0] = kind[i] == 2 ? 1.0f / gt[i]->w : 1.0f;
        scale[i][1] = kind[i] == 2 ? 1.0f / gt[i]->h : 1.0f;
        if ((kind[i] == 1 || kind[i] == 2) && gt[i]->is_rt) { scale[i][0] *= gt[i]->rfx; scale[i][1] *= gt[i]->rfy; }
    }
    if (p->u_texScale >= 0) glUniform2fv(p->u_texScale, 4, &scale[0][0]);
    static u32 const_gen = 1;
    if (G.vsc_dirty) { const_gen++; G.vsc_dirty = 0; }
    {
        extern int freecam_active(void); extern int freecam_apply(float (*c)[4]);
        static float fc[192][4];
        int done = 0;
        extern int g_coop_split; extern int coop_apply(float (*c)[4]);
        if ((freecam_active() || g_coop_split) && p->u_c >= 0 && G.vprog && (G.vs || G.use_direct) && g_cur_rt && G.backbuffer &&
            g_cur_rt->data == (G.backbuffer->Data & 0x7FFFFFFFu)) {
            memcpy(fc, G.vsc, sizeof fc);
            int ch = freecam_active() && freecam_apply(fc);
            if (g_coop_split) ch |= coop_apply(fc);           /* split screen: half as wide a view */
            if (ch) { glUniform4fv(p->u_c, 192, &fc[0][0]); p->const_gen = 0; done = 1; }
        }
        if (!done && p->u_c >= 0 && p->const_gen != const_gen) { glUniform4fv(p->u_c, 192, &G.vsc[0][0]); p->const_gen = const_gen; }
    }
    if (p->u_surf >= 0) glUniform2f(p->u_surf, (float)g_cur_w, (float)g_cur_h);
    if (p->u_zscale >= 0) glUniform1f(p->u_zscale, g_zscale);
    if (p->u_pixscale >= 0 && g_cur_rt)
        glUniform2f(p->u_pixscale, (float)rt_vw(g_cur_rt) / g_cur_rt->w, (float)rt_vh(g_cur_rt) / g_cur_rt->h);
    if (p->u_wbias >= 0) glUniform1f(p->u_wbias, -(float)G.rs[XD3DRS_ZBIAS]);   /* depth-buffer units, like the z path */
    float v4[4];
    color4(G.rs[XD3DRS_TEXTUREFACTOR], v4); if (p->u_tfactor >= 0) glUniform4fv(p->u_tfactor, 1, v4);
    color4(G.rs[XD3DRS_FOGCOLOR], v4); if (p->u_fogColor >= 0) glUniform4fv(p->u_fogColor, 1, v4);
    float fp[4];
    memcpy(&fp[0], &G.rs[XD3DRS_FOGSTART], 4); memcpy(&fp[1], &G.rs[XD3DRS_FOGEND], 4); memcpy(&fp[2], &G.rs[XD3DRS_FOGDENSITY], 4);
    fp[3] = (float)G.rs[XD3DRS_FOGTABLEMODE];
    if (p->u_fogParams >= 0) glUniform4fv(p->u_fogParams, 1, fp);
    if (G.ps) {
        float pc[2][8][4];
        for (int i = 0; i < 8; i++) {
            color4(G.rs[XD3DRS_PSCONSTANT0_0 + i], pc[0][i]);
            color4(G.rs[XD3DRS_PSCONSTANT1_0 + i], pc[1][i]);
        }
        /* the render-state table is authoritative (SetPixelShaderConstant writes into it, as do titles) */
        if (p->u_pc0 >= 0) glUniform4fv(p->u_pc0, 8, &pc[0][0][0]);
        if (p->u_pc1 >= 0) glUniform4fv(p->u_pc1, 8, &pc[1][0][0]);
        color4(G.rs[XD3DRS_PSFINALCOMBINERCONSTANT0], v4);
        if (p->u_fc0 >= 0) glUniform4fv(p->u_fc0, 1, v4);
        color4(G.rs[XD3DRS_PSFINALCOMBINERCONSTANT1], v4);
        if (p->u_fc1 >= 0) glUniform4fv(p->u_fc1, 1, v4);
    }
    int afunc = 7;
    if (G.rs[XD3DRS_ALPHATESTENABLE]) afunc = (int)(G.rs[XD3DRS_ALPHAFUNC] - 0x200);
    if (p->u_alphaFunc >= 0) glUniform1i(p->u_alphaFunc, afunc);
    if (p->u_alphaRef >= 0) glUniform1f(p->u_alphaRef, (float)(G.rs[XD3DRS_ALPHAREF] & 0xFF));
    apply_state();

    /* indices (rebased) + primitive conversion */
    GLenum mode = prim_mode(prim);
    u32 nidx = count;
    if (prim == 8) nidx = count / 4 * 6;
    if (nidx + 8 > g_idx_cap) { g_idx_cap = (nidx + 8) * 2; g_idx = realloc(g_idx, g_idx_cap * 2); }
    if (prim == 8) {
        for (u32 q = 0; q < count / 4; q++) {
            u16 a = indices ? indices[4*q] - vmin : 4*q, b = indices ? indices[4*q+1] - vmin : 4*q+1;
            u16 c = indices ? indices[4*q+2] - vmin : 4*q+2, d = indices ? indices[4*q+3] - vmin : 4*q+3;
            u16 *o = g_idx + 6 * q;
            o[0] = a; o[1] = b; o[2] = c; o[3] = a; o[4] = c; o[5] = d;
        }
    } else if (indices) {
        for (u32 i = 0; i < count; i++) g_idx[i] = indices[i] - vmin;
    }
    if (g_diag > 0) glBeginQuery(GL_SAMPLES_PASSED, g_diag_query);
    static int fast_boot = -1; if (fast_boot < 0) fast_boot = getenv("XBOX_FAST_BOOT") != NULL;
    extern u32 debugmenu_play_frame(void);
    if (fast_boot && !debugmenu_play_frame()) ;        /* tests: nothing rasterised until gameplay begins */
    else if (indices || prim == 8) {
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_ebo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, nidx * 2, g_idx, GL_STREAM_DRAW);
        glDrawElements(mode, nidx, GL_UNSIGNED_SHORT, 0);
    } else {
        glDrawArrays(mode, 0, count);
    }
    probe_pixel(prim, count, kind, gt);
    {   /* camera research: remember the world transform of the main world shader each frame */
        extern float g_dbg_world_m[16]; extern u32 g_dbg_world_frame;
        if ((G.vs || G.use_direct) && G.vprog && G.vprog->hash == 0x92b511c778189d30ULL && g_dbg_world_frame != G.frame) {
            memcpy(g_dbg_world_m, G.vsc[96], 64); g_dbg_world_frame = G.frame;
            extern void freecam_note_world(const float (*c)[4]);
            freecam_note_world((const float (*)[4])&G.vsc[96]);
        }
    }
    if (g_diag > 0 && getenv("XBOX_DIAG_FRAME") && G.frame == (u32)parse_u32(getenv("XBOX_DIAG_FRAME"), 10)
        && g_cur_rt && G.backbuffer && g_cur_rt->data == (G.backbuffer->Data & 0x7FFFFFFFu)) {
        /* trace how each draw changes the back buffer's average colour (numbers only) */
        static u8 *px; static double last[4] = { -1, -1, -1, -1 }; static int idx;
        int W = g_cur_rt->pw, H = g_cur_rt->ph;
        px = realloc(px, (size_t)W * H * 4);
        glEndQuery(GL_SAMPLES_PASSED);
        GLuint n = 0; glGetQueryObjectuiv(g_diag_query, GL_QUERY_RESULT, &n);
        glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
        double a[4] = { 0 };
        for (int i = 0; i < W * H; i++) for (int c = 0; c < 4; c++) a[c] += px[4 * i + c];
        for (int c = 0; c < 4; c++) a[c] /= (double)W * H;
        double d = fabs(a[0] - last[0]) + fabs(a[1] - last[1]) + fabs(a[2] - last[2]);
        const u32 *t0 = G.tss[0];
        if (idx < 3) {
            const struct vattr *at = G.use_direct ? G.direct_attrs : G.vs ? G.vs->attrs : NULL;
            char ab[400] = ""; int k = 0;
            for (int a = 0; at && a < 16; a++) if (at[a].present)
                k += snprintf(ab + k, sizeof ab - k, " v%d:s%u+%u t%#x", a, at[a].stream, at[a].offset, at[a].type);
            klog("diag draw %3d: vs %016llx (%d instr) direct %d attrs%s strides %u/%u\n", idx,
                 G.vprog ? (unsigned long long)G.vprog->hash : 0ULL, G.vprog ? G.vprog->ninstr : 0, G.use_direct, ab,
                 G.streams[0].stride, G.streams[1].stride);
        }
        if (d > 0.5 || idx < 3)
            klog("diag draw %3d: px %6u avg rgba (%.1f %.1f %.1f %.1f) | prim %u n %u blend %u %#x/%#x op %#x atest %u aref %u | ps %s tex0 %08x | s0 colorop %u args %#x,%#x alphaop %u args %#x,%#x | tfactor %08x fog %u\n",
                 idx, n, a[0], a[1], a[2], a[3], prim, count, G.rs[XD3DRS_ALPHABLENDENABLE], G.rs[XD3DRS_SRCBLEND],
                 G.rs[XD3DRS_DESTBLEND], G.rs[XD3DRS_BLENDOP], G.rs[XD3DRS_ALPHATESTENABLE], G.rs[XD3DRS_ALPHAREF],
                 G.ps ? "yes" : "no", G.tex[0] ? G.tex[0]->Data : 0, t0[XTSS_COLOROP], t0[XTSS_COLORARG1], t0[XTSS_COLORARG2],
                 t0[XTSS_ALPHAOP], t0[XTSS_ALPHAARG1], t0[XTSS_ALPHAARG2], G.rs[XD3DRS_TEXTUREFACTOR], G.rs[XD3DRS_FOGENABLE]);
        memcpy(last, a, sizeof a);
        idx++;
        glBeginQuery(GL_SAMPLES_PASSED, g_diag_query);   /* keep the outer query balanced */
    }
    if (g_diag > 0) {
        glEndQuery(GL_SAMPLES_PASSED);
        GLuint n = 0;
        glGetQueryObjectuiv(g_diag_query, GL_QUERY_RESULT, &n);
        struct diag_rt *r = diag_cur();
        r->draws++; r->samples += n;
        if (!n) {
            r->zero_draws++;
            if (g_diag_logged_zero < 12 && G.frame > 600) {
                g_diag_logged_zero++;
                /* first vertex position as fetched (attribute v0) */
                float *v0 = g_vtx;
                klog("diag: zero-pixel draw rt %08x prim %u count %u vs %016llx ps %p fvf %#x ztest %u zfunc %#x cull %#x blend %u cw %08x v0=(%g %g %g %g) c58=(%g %g %g %g) c59=(%g %g %g %g)\n",
                     g_cur_rt ? g_cur_rt->data : 0, prim, count, G.vprog ? (unsigned long long)G.vprog->hash : 0ULL, (void *)G.ps, G.fvf,
                     G.rs[XD3DRS_ZENABLE], G.rs[XD3DRS_ZFUNC], G.rs[XD3DRS_CULLMODE], G.rs[XD3DRS_ALPHABLENDENABLE],
                     G.rs[XD3DRS_COLORWRITEENABLE], v0[0], v0[1], v0[2], v0[3],
                     G.vsc[58][0], G.vsc[58][1], G.vsc[58][2], G.vsc[58][3], G.vsc[59][0], G.vsc[59][1], G.vsc[59][2], G.vsc[59][3]);
            }
        }
    }
    if (getenv_debug_gl()) {
        GLenum err = glGetError();
        if (err) { if (g_gl_errors++ < 20) klog("gl: error %#x after draw (prim %u count %u)\n", err, prim, count); }
    }
    g_t_submit += tnow() - t3;
}

static void probe_pixel(u32 prim, u32 count, const int *kind, struct gtex **gt);
static void gl_clear_inner(u32 count, const s32 *rects, u32 flags, u32 color, float z, u32 stencil);
void gl_clear(u32 count, const s32 *rects, u32 flags, u32 color, float z, u32 stencil)
{
    gl_clear_inner(count, rects, flags, color, z, stencil);
    if (g_win && !g_gfx_null) { int k[4] = { 0 }; probe_pixel(0, 0, k, NULL); }
}
static void gl_clear_inner(u32 count, const s32 *rects, u32 flags, u32 color, float z, u32 stencil)
{
    if (g_gfx_null || !g_win) return;
    if (g_diag > 0) diag_cur()->clears++;
    GLbitfield m = 0;
    if (flags & 0xF0) { m |= GL_COLOR_BUFFER_BIT; float c[4]; color4(color, c); glClearColor(c[0], c[1], c[2], c[3]);
                        glColorMask(flags >> 6 & 1, flags >> 5 & 1, flags >> 4 & 1, flags >> 7 & 1); }
    if (flags & 0x01) { m |= GL_DEPTH_BUFFER_BIT; glClearDepth(z); glDepthMask(GL_TRUE); }
    if (flags & 0x02) { m |= GL_STENCIL_BUFFER_BIT; glClearStencil(stencil); glStencilMask(0xFF); }
    if (count && rects) {
        glEnable(GL_SCISSOR_TEST);
        for (u32 i = 0; i < count; i++) {
            const s32 *r = rects + 4 * i;
            glScissor(CX(r[0]), CY(r[1]), CX(r[2]) - CX(r[0]), CY(r[3]) - CY(r[1]));
            glClear(m);
        }
        glDisable(GL_SCISSOR_TEST);
    } else {
        glDisable(GL_SCISSOR_TEST);
        glClear(m);
    }
    glColorMask(1, 1, 1, 1);
}

void gl_flush(void) { if (g_win && !g_gfx_null) glFlush(); }

void gl_pump_events(void)
{
    if (!g_win) return;
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) { klog("window closed\n"); fflush(NULL); _exit(0); }
        input_sdl_event(&e);
    }
    input_sdl_poll();
}

/* XBOX_SHOT=dir [XBOX_SHOT_EVERY=n, default 60]: write the back buffer as PPM files (for testing). */
static void shot_maybe(struct target *bb)
{
    static const char *dir; static int every = -1;
    if (every < 0) {
        dir = getenv("XBOX_SHOT");
        every = getenv("XBOX_SHOT_EVERY") ? (int)parse_u32(getenv("XBOX_SHOT_EVERY"), 10) : 60;
        if (every < 1) every = 1;
    }
    static u32 from, to = 0xFFFFFFFF; static int rng;
    if (!rng) { rng = 1; if (getenv("XBOX_SHOT_FROM")) from = parse_u32(getenv("XBOX_SHOT_FROM"), 10);
                if (getenv("XBOX_SHOT_TO")) to = parse_u32(getenv("XBOX_SHOT_TO"), 10); }
    u32 fno = G.frame;
    if (getenv("XBOX_SHOT_REL")) {                  /* frames counted from when gameplay began */
        extern u32 debugmenu_play_frame(void);
        fno = debugmenu_play_frame();
        if (!fno) return;
    }
    if (!dir || fno % every || fno < from || fno > to) return;
    if (getenv("XBOX_SHOT_ALLRT")) {
        for (int r = 0; r < MAXRT; r++) {
            struct target *t = &g_rts[r];
            if (!t->used || t == bb) continue;
            int W = t->pw, H = t->ph;
            u8 *px = malloc((size_t)W * H * 4);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, t->fbo);
            glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
            char fn[512];
            snprintf(fn, sizeof fn, "%s/f%06u_rt%08x.pam", dir, fno, t->data);
            FILE *f = fopen(fn, "wb");
            if (f) { fprintf(f, "P7\nWIDTH %d\nHEIGHT %d\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n", W, H);
                     fwrite(px, 4, (size_t)W * H, f); fclose(f); }
            free(px);
        }
    }
    if (getenv("XBOX_SHOT_WINDOW")) {           /* the window, with the port's overlays */
        int W, H; SDL_GL_GetDrawableSize(g_win, &W, &H);
        u8 *px = malloc((size_t)W * H * 3);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0); glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, W, H, GL_RGB, GL_UNSIGNED_BYTE, px);
        char fn[512]; snprintf(fn, sizeof fn, "%s/f%06u.ppm", dir, fno);
        FILE *f = fopen(fn, "wb");
        if (f) { fprintf(f, "P6\n%d %d\n255\n", W, H); for (int y = H - 1; y >= 0; y--) fwrite(px + (size_t)y * W * 3, 3, W, f); fclose(f); }
        free(px);
        return;
    }
    int W = bb->pw, H = bb->ph;
    int win = getenv("XBOX_SHOT_WINDOW") != NULL;      /* capture the window (with overlays) instead */
    if (win) SDL_GL_GetDrawableSize(g_win, &W, &H);
    u8 *px = malloc((size_t)W * H * 4);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, win ? 0 : bb->fbo);
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
    char fn[512];
    snprintf(fn, sizeof fn, "%s/f%06u.ppm", dir, fno);
    FILE *f = fopen(fn, "wb");
    if (f) {
        fprintf(f, "P6 %d %d 255\n", W, H);
        for (int y = 0; y < H; y++) {
            const u8 *row = px + (size_t)4 * W * (win ? H - 1 - y : y);
            for (int x = 0; x < W; x++) fwrite(row + 4 * x, 1, 3, f);
        }
        fclose(f);
    }
    free(px);
}

u32 gl_frame_number(void) { return G.frame; }
int g_pres_rect[4];          /* where the game's picture sits in the window (x, y, w, h) */

static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

/* co-op split screen: the back buffer as drawn for player v (0..3) */
static struct { GLuint fbo, tex; int w, h, ok, uw, uh; } g_view[4];   /* (uw x uh: the part drawn in) */
static int g_coop_cap[4];
void gl_coop_capture(int v)
{
    if (g_gfx_null || !g_win) return;
    struct target *bb = rt_get(G.backbuffer ? G.backbuffer->Data & 0x7FFFFFFFu : 0, G.bb_width, G.bb_height);
    v &= 3;
    if (!g_view[v].fbo || g_view[v].w != bb->pw || g_view[v].h != bb->ph) {
        if (!g_view[v].fbo) { glGenFramebuffers(1, &g_view[v].fbo); glGenTextures(1, &g_view[v].tex); }
        glBindTexture(GL_TEXTURE_2D, g_view[v].tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, bb->pw, bb->ph, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, g_view[v].fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_view[v].tex, 0);
        g_view[v].w = bb->pw; g_view[v].h = bb->ph;
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, bb->fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_view[v].fbo);
    glDisable(GL_SCISSOR_TEST);                    /* (set again by the next draw) */
    int uw = rt_vw(bb), uh = rt_vh(bb);
    glBlitFramebuffer(0, 0, uw, uh, 0, 0, uw, uh, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, g_cur_rt ? g_cur_rt->fbo : bb->fbo);
    g_view[v].ok = 1; g_coop_cap[v] = 1; g_view[v].uw = uw; g_view[v].uh = uh;
}

void gl_present(void)
{
    if (g_gfx_null || !g_win) { gl_pump_events(); return; }
    struct target *bb = rt_get(G.backbuffer ? G.backbuffer->Data & 0x7FFFFFFFu : 0, G.bb_width, G.bb_height);
    int ww, wh;
    SDL_GL_GetDrawableSize(g_win, &ww, &wh);
    /* letterbox to the game's aspect (4:3, or 16:9 if the game renders widescreen) */
    float aspect = g_widescreen ? 16.0f / 9.0f : 4.0f / 3.0f;
    int dw = ww, dh = (int)(ww / aspect);
    if (dh > wh) { dh = wh; dw = (int)(wh * aspect); }
    int dx = (ww - dw) / 2, dy = (wh - dh) / 2;
    { extern int g_pres_rect[4]; g_pres_rect[0] = dx; g_pres_rect[1] = dy; g_pres_rect[2] = dw; g_pres_rect[3] = dh; }
    extern int g_coop_split, g_coop_view;
    if (g_coop_split) {          /* co-op: both views are normally drawn this frame (coop.c render hook) and
                                  * kept by gl_coop_capture; a frame drawn another way is its player's view */
        if (!g_coop_cap[0] && !g_coop_cap[1] && !g_coop_cap[2] && !g_coop_cap[3]) gl_coop_capture(g_coop_view & 3);
        if (!g_view[0].ok) gl_coop_capture(0);
    } else for (int v = 0; v < 4; v++) g_view[v].ok = 0;
    for (int v = 0; v < 4; v++) g_coop_cap[v] = 0;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, bb->fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0, 0, 0, 1);
    glColorMask(1, 1, 1, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    /* our targets are stored top-row-first; flip on the way to the window */
    if (g_coop_split == 2) {     /* left half player 1, right half player 2 (each frame squeezed to half width) */
        int gap = dw / 400 + 1, hw = (dw - gap) / 2;
        glBindFramebuffer(GL_READ_FRAMEBUFFER, g_view[0].fbo);
        glBlitFramebuffer(0, 0, g_view[0].uw, g_view[0].uh, dx, dy + dh, dx + hw, dy, GL_COLOR_BUFFER_BIT, GL_LINEAR);
        extern int coop_view_live(int v); if (g_view[1].ok && coop_view_live(1)) { glBindFramebuffer(GL_READ_FRAMEBUFFER, g_view[1].fbo);
            glBlitFramebuffer(0, 0, g_view[1].uw, g_view[1].uh, dx + dw - hw, dy + dh, dx + dw, dy, GL_COLOR_BUFFER_BIT, GL_LINEAR); }
        glBindFramebuffer(GL_READ_FRAMEBUFFER, bb->fbo);
    } else if (g_coop_split) {   /* quarters: 1 top left, 2 top right, 3 bottom left, 4 bottom right (an empty one black) */
        int gap = dw / 400 + 1, hw = (dw - gap) / 2, hh = (dh - gap) / 2;
        for (int v = 0; v < 4; v++) {
            extern int coop_view_live(int v); if (!g_view[v].ok || !coop_view_live(v)) continue;
            int x0 = dx + (v & 1 ? dw - hw : 0), y0 = dy + (v & 2 ? 0 : dh - hh);   /* (GL: y up - the top row is the high one) */
            glBindFramebuffer(GL_READ_FRAMEBUFFER, g_view[v].fbo);
            glBlitFramebuffer(0, 0, g_view[v].uw, g_view[v].uh, x0, y0 + hh, x0 + hw, y0, GL_COLOR_BUFFER_BIT, GL_LINEAR);
        }
        glBindFramebuffer(GL_READ_FRAMEBUFFER, bb->fbo);
    } else
    glBlitFramebuffer(0, 0, rt_vw(bb), rt_vh(bb), dx, dy + dh, dx + dw, dy, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    { GLenum err = glGetError(); if (err && g_gl_errors++ < 20) klog("gl: error %#x during frame %u\n", err, G.frame); }
    if (g_diag > 0) {
        if (G.frame % 120 == 0 || (g_draws_frame > 50 && G.frame % 10 == 0)) {
            /* average colour of the backbuffer (numbers only) */
            static u8 *px; int W = bb->pw, H = bb->ph;
            px = realloc(px, (size_t)W * H * 4);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, bb->fbo);
            glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
            u64 sum[3] = { 0 }, nonblack = 0;
            for (int i = 0; i < W * H; i++) { sum[0] += px[4*i]; sum[1] += px[4*i+1]; sum[2] += px[4*i+2];
                                              if (px[4*i] | px[4*i+1] | px[4*i+2]) nonblack++; }
            klog("diag frame %u: backbuffer avg rgb (%llu %llu %llu), non-black %.1f%%\n", G.frame,
                 sum[0] / (W * H), sum[1] / (W * H), sum[2] / (W * H), 100.0 * nonblack / (W * H));
            for (int i = 0; i < g_diag_n; i++) {
                struct diag_rt *r = &g_diag_rt[i];
                klog("diag   rt %08x %ux%u: %u draws (%u wrote no pixels), %llu samples, %u clears\n",
                     r->data, r->w, r->h, r->draws, r->zero_draws, (unsigned long long)r->samples, r->clears);
            }
        }
        g_diag_n = 0;
    }
    /* free camera: controls + transform for the next frame */
    {
        extern void freecam_frame(float dt);
        static double lastf; double tf = tnow();
        freecam_frame(lastf ? (float)(tf - lastf) : 0.0f); lastf = tf;
    }
    /* port overlays: debug menu, FPS */
    {
        extern void debugmenu_draw(void);
        debugmenu_draw();
        if (g_show_fps) {
            static double last; static int nf; static char buf[48];
            double t = tnow(); nf++;
            if (t - last >= 0.5) { snprintf(buf, sizeof buf, "%.0f fps", nf / (t - last)); last = t; nf = 0; }
            int w, h; ov_begin(&w, &h);
            float sz = h / 40.0f;
            ov_rect(sz * 0.5f, sz * 0.5f, sz * 5.5f, sz * 1.3f, 0x90000000);
            ov_text(sz * 0.8f, sz * 0.65f, sz, 0xFF80FF80, buf);
            ov_end();
        }
    }
    shot_maybe(bb);
    { extern void coop_frame_done(void); coop_frame_done(); }
    double ts0 = tnow();
    SDL_GL_SwapWindow(g_win);
    g_t_swap += tnow() - ts0;
    glBindFramebuffer(GL_FRAMEBUFFER, g_cur_rt ? g_cur_rt->fbo : bb->fbo);
    { struct target *ct = g_cur_rt ? g_cur_rt : bb; glViewport(0, 0, rt_vw(ct), rt_vh(ct)); }

    /* pacing at the emulated vertical-blank rate: the game logic is tied to it */
    static double next;
    double t = now_s();
    if (next == 0 || t - next > 0.1) next = t;
    { extern volatile int g_target_fps; next += 1.0 / (g_target_fps > 0 ? g_target_fps : 60); }
    double wait = next - now_s();
    if (wait > 0) { struct timespec ts = { 0, (long)(wait * 1e9) }; nanosleep(&ts, NULL); }

    static double last_report;
    static u32 frames;
    frames++;
    if (t - last_report > 5.0) {
        if (last_report) {
            double f = frames ? frames : 1;
            klog("gl: %.1f fps, %llu draws last frame, %d programs, %llu shader failures, %llu GL errors\n",
                 frames / (t - last_report), (unsigned long long)g_draws_frame, g_nprogs,
                 (unsigned long long)g_shader_fail, (unsigned long long)g_gl_errors);
            klog("gl: per frame ms: vertex %.1f, textures %.1f (%llu uploads, %.1f MB), shaders %.1f, submit %.1f, swap %.1f\n",
                 g_t_vtx * 1000 / f, g_t_tex * 1000 / f, (unsigned long long)g_tex_uploads, g_tex_upload_bytes / 1e6,
                 g_t_prog * 1000 / f, g_t_submit * 1000 / f, g_t_swap * 1000 / f);
            g_t_vtx = g_t_tex = g_t_prog = g_t_submit = g_t_swap = 0; g_tex_uploads = g_tex_upload_bytes = 0;
        }
        last_report = t; frames = 0;
    }
    g_draws_frame = 0;
    gl_pump_events();
}

int gl_init(int width, int height)
{
    if (getenv("XBOX_GFX") && !strcmp(getenv("XBOX_GFX"), "null")) { g_gfx_null = 1; klog("gl: null renderer\n"); return 0; }
    g_widescreen = !(getenv("XBOX_WIDESCREEN") && getenv("XBOX_WIDESCREEN")[0] == '0');
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) != 0) {
        klog("SDL_Init failed: %s\n", SDL_GetError());
        g_gfx_null = 1;
        return -1;
    }
    /* internal resolution: XBOX_SCALE=n (n x 480 lines), default: the desktop's height */
    SDL_DisplayMode dm;
    int desk_w = 1920, desk_h = 1080;
    if (SDL_GetDesktopDisplayMode(0, &dm) == 0 && dm.w > 0 && dm.h > 0) { desk_w = dm.w; desk_h = dm.h; }
    float sy = getenv("XBOX_SCALE") ? (float)atof(getenv("XBOX_SCALE")) : desk_h / (float)height;
    if (sy < 1 && !getenv("XBOX_SCALE")) sy = 1;
    if (sy < 0.25f) sy = 0.25f;                       /* below 1: tests only */
    if (sy > 8) sy = 8;
    g_sy = sy;
    g_sx = g_widescreen ? sy * 4.0f / 3.0f : sy;
    klog("gl: %s, internal resolution %dx%d (game %dx%d)\n", g_widescreen ? "widescreen 16:9" : "4:3",
         SX(width), SY(height), width, height);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    /* window: the presentation aspect, at most ~85% of the desktop */
    float aspect = g_widescreen ? 16.0f / 9.0f : 4.0f / 3.0f;
    int wh = desk_h * 85 / 100, ww = (int)(wh * aspect);
    if (ww > desk_w * 85 / 100) { ww = desk_w * 85 / 100; wh = (int)(ww / aspect); }
    g_win = SDL_CreateWindow("Spyro: A Hero's Tail (native)", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                             ww, wh, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!g_win) { klog("SDL_CreateWindow failed: %s\n", SDL_GetError()); g_gfx_null = 1; return -1; }
    g_ctx = SDL_GL_CreateContext(g_win);
    if (!g_ctx) { klog("OpenGL 3.3 context failed: %s\n", SDL_GetError()); g_gfx_null = 1; return -1; }
    SDL_GL_MakeCurrent(g_win, g_ctx);
    SDL_GL_SetSwapInterval(getenv("XBOX_VSYNC") ? 1 : 0);
#define X(t, n) n = (t)SDL_GL_GetProcAddress(#n); if (!n) { klog("missing GL function %s\n", #n); g_gfx_null = 1; return -1; }
    GLFUNCS
#undef X
    klog("gl: %s / %s / %s\n", (const char *)glGetString(GL_VENDOR),
         (const char *)glGetString(GL_RENDERER), (const char *)glGetString(GL_VERSION));
    glGenVertexArrays(1, &g_vao);
    glBindVertexArray(g_vao);
    glGenBuffers(1, &g_vbo);
    glGenBuffers(1, &g_ebo);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glEnable(GL_PROGRAM_POINT_SIZE);
    u8 white[4] = { 255, 255, 255, 255 };
    glGenTextures(1, &g_tex_dummy2d);
    glBindTexture(GL_TEXTURE_2D, g_tex_dummy2d);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    glGenTextures(1, &g_tex_dummycube);
    glBindTexture(GL_TEXTURE_CUBE_MAP, g_tex_dummycube);
    for (int f = 0; f < 6; f++) glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAX_LEVEL, 0);
    glGenTextures(1, &g_tex_dummy3d);
    glBindTexture(GL_TEXTURE_3D, g_tex_dummy3d);
    glTexImage3D(GL_TEXTURE_3D, 0, GL_RGBA8, 1, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAX_LEVEL, 0);
    g_diag = getenv("XBOX_DIAG") != NULL;
    if (g_diag) glGenQueries(1, &g_diag_query);
    input_sdl_init();
    return 0;
}

/* XBOX_DUMP_SHADERS: also translate every vertex program at creation time */
void gl_dump_vs(struct vs_obj *vs)
{
    if (!getenv("XBOX_DUMP_SHADERS") || !vs->has_program) return;
    char *src = build_vs_program(vs);
    char fn[512];
    snprintf(fn, sizeof fn, "%s/created_%016llx.vert", getenv("XBOX_DUMP_SHADERS"), (unsigned long long)vs->hash);
    FILE *f = fopen(fn, "w");
    if (f) { fputs(src, f); fclose(f); }
    free(src);
}
