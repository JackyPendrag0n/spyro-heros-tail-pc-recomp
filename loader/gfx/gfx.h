/* Xbox Direct3D 8 HLE -> OpenGL. Shared declarations. */
#pragma once
#include "../xbox.h"
#include "../d3d_rs.h"
#include <stdarg.h>

/* ---- Xbox resource structs (byte-identical to the XDK) ---- */
typedef struct { u32 Common, Data, Lock; } XResource;                       /* VB, IB, palette */
typedef struct { u32 Common, Data, Lock, Format, Size; } XPixelContainer;   /* texture */
typedef struct { u32 Common, Data, Lock, Format, Size; XPixelContainer *Parent; } XSurface;

#define XCOMMON_REFMASK      0x0000FFFFu
#define XCOMMON_TYPE_MASK    0x00070000u
#define XCOMMON_TYPE_VB      0x00000000u
#define XCOMMON_TYPE_IB      0x00010000u
#define XCOMMON_TYPE_PB      0x00020000u
#define XCOMMON_TYPE_PALETTE 0x00030000u
#define XCOMMON_TYPE_TEXTURE 0x00040000u
#define XCOMMON_TYPE_SURFACE 0x00050000u
#define XCOMMON_D3DCREATED   0x01000000u

#define XFMT_CUBEMAP  0x4u
#define XFMT_DIM(f)   (((f) >> 4) & 0xF)
#define XFMT_FMT(f)   (((f) >> 8) & 0xFF)
#define XFMT_MIPS(f)  (((f) >> 16) & 0xF)
#define XFMT_USIZE(f) (((f) >> 20) & 0xF)
#define XFMT_VSIZE(f) (((f) >> 24) & 0xF)
#define XFMT_PSIZE(f) (((f) >> 28) & 0xF)

/* Xbox texture stage state indices (deferred texture state table, 32 per stage) */
enum {
    XTSS_ADDRESSU = 0, XTSS_ADDRESSV, XTSS_ADDRESSW, XTSS_MAGFILTER, XTSS_MINFILTER,
    XTSS_MIPFILTER, XTSS_MIPMAPLODBIAS, XTSS_MAXMIPLEVEL, XTSS_MAXANISOTROPY,
    XTSS_COLORKEYOP, XTSS_COLORSIGN, XTSS_ALPHAKILL, XTSS_COLOROP, XTSS_COLORARG0,
    XTSS_COLORARG1, XTSS_COLORARG2, XTSS_ALPHAOP, XTSS_ALPHAARG0, XTSS_ALPHAARG1,
    XTSS_ALPHAARG2, XTSS_RESULTARG, XTSS_TEXTURETRANSFORMFLAGS, XTSS_BUMPENVMAT00,
    XTSS_BUMPENVMAT01, XTSS_BUMPENVMAT11, XTSS_BUMPENVMAT10, XTSS_BUMPENVLSCALE,
    XTSS_BUMPENVLOFFSET, XTSS_TEXCOORDINDEX, XTSS_BORDERCOLOR, XTSS_COLORKEYCOLOR,
    XTSS_COUNT = 32
};

/* ---- shader objects created by the HLE ---- */
struct vattr { u8 present, stream; u16 offset; u32 type; };

#define VS_MAGIC 0x56534844u   /* 'DHSV' */
struct vs_obj {
    u32 magic;
    struct vattr attrs[16];
    int has_program;
    u32 *prog;              /* 4 DWORDs per instruction */
    int ninstr;
    u32 type;               /* program header type (0x2078 = vertex shader) */
    u64 hash;
    u32 *const_tokens;      /* D3DVSD_CONST data loaded on SetVertexShader */
    int nconst_tokens;
    void *gl;               /* backend cache */
};

#define PS_MAGIC 0x50534844u
#define PSDEF_DWORDS 60
struct ps_obj {
    u32 magic;
    u32 def[PSDEF_DWORDS];
};

/* ---- device state ---- */
struct stream { u32 data; u32 stride; };

struct gfx_state {
    u32 *rs;                    /* D3D__RenderState (guest) */
    u32 (*tss)[XTSS_COUNT];     /* D3D__TextureState[4][32] (guest) */
    float vsc[192][4];          /* vertex shader constants (hardware index) */
    int vsc_dirty;
    float psc[16][4];           /* SetPixelShaderConstant values */
    struct stream streams[16];
    u32 vs_handle;              /* current vertex shader handle (or FVF) */
    struct vs_obj *vs;          /* declaration (NULL when FVF) */
    struct vs_obj *vprog;       /* program executing from instruction memory (NULL = none) */
    u32 fvf;
    struct vattr direct_attrs[16];
    int use_direct;
    struct ps_obj *ps;
    XPixelContainer *tex[4];
    XResource *pal[4];
    u32 base_vertex;
    XSurface *rt, *zs;          /* current render target / depth */
    XSurface *backbuffer, *depthbuffer;
    u32 bb_width, bb_height;
    float ss_offset[2];
    int scissor_count; s32 scissors[8][4]; int scissor_exclusive;
    u32 frame;
    void *vblank_cb;
};
extern struct gfx_state G;

/* d3d.c */
void d3d_install(void);
u32 *guestp(u32 a);                      /* guest address (physical or virtual) -> pointer */
u32 tex_level_offset(u32 fmt, u32 size, int level, u32 *w, u32 *h, u32 *pitch);
void tex_dims(const XPixelContainer *t, u32 *w, u32 *h, u32 *depth, int *levels, int *linear, int *pitch);
u32 tex_bpp(u32 xfmt);                   /* bits per pixel; 0 for unknown */
int  tex_is_linear(u32 xfmt);
int  tex_is_dxt(u32 xfmt);

/* vsh.c */
char *vsh_translate(const u32 *tokens, int ninstr, int *writes_consts);
extern const char *vsh_helpers;

/* psh.c */
char *psh_generate(const u32 *rs, const u32 (*tss)[XTSS_COUNT], const struct ps_obj *ps,
                   const int tex_kind[4], u64 *key_out);

u64 psh_key(const u32 *rs, const u32 (*tss)[XTSS_COUNT], const struct ps_obj *ps, const int kind[4]);

/* tex.c */
/* Decode one mip level / face into RGBA8 (w*h*4 bytes). Returns 0 on success. */
int tex_decode(u32 xfmt, const u8 *src, u32 w, u32 h, u32 pitch, const u32 *palette, u8 *dst);
void tex_unswizzle(const u8 *src, u8 *dst, u32 w, u32 h, u32 d, u32 bpp_bytes);

/* gl.c backend */
int  gl_init(int width, int height);
void gl_present(void);
void gl_clear(u32 count, const s32 *rects, u32 flags, u32 color, float z, u32 stencil);
void gl_draw(u32 prim, u32 count, const u16 *indices, const u8 *up_data, u32 up_stride);
void gl_set_render_target(XSurface *rt, XSurface *zs);
void gl_texture_dirty(u32 data_addr);
void gl_flush(void);
void gl_pump_events(void);
extern int g_gfx_null;

/* hle.c */
u32 hle_sym(const char *name);
int hle_hook(const char *name, void *target);
void hle_patch_jmp(u32 at, void *target);
