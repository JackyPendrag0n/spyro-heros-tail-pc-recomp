/* Minimal 2D overlay on the window (debug menu, FPS counter): solid rectangles and text from
 * an embedded font atlas. Drawn after the game frame is presented into the default framebuffer. */
#include <math.h>
#include "gfx.h"
#include <SDL2/SDL.h>
#include <GL/glcorearb.h>
#include <string.h>
#include <stdlib.h>
#include "font.h"

/* GL entry points are loaded by gl.c */
#define OVF(X) X(PFNGLCREATESHADERPROC, glCreateShader) X(PFNGLSHADERSOURCEPROC, glShaderSource) \
    X(PFNGLCOMPILESHADERPROC, glCompileShader) X(PFNGLCREATEPROGRAMPROC, glCreateProgram) \
    X(PFNGLATTACHSHADERPROC, glAttachShader) X(PFNGLLINKPROGRAMPROC, glLinkProgram) X(PFNGLUSEPROGRAMPROC, glUseProgram) \
    X(PFNGLGETUNIFORMLOCATIONPROC, glGetUniformLocation) X(PFNGLUNIFORM2FPROC, glUniform2f) X(PFNGLUNIFORM1IPROC, glUniform1i) \
    X(PFNGLGENVERTEXARRAYSPROC, glGenVertexArrays) X(PFNGLBINDVERTEXARRAYPROC, glBindVertexArray) \
    X(PFNGLGENBUFFERSPROC, glGenBuffers) X(PFNGLBINDBUFFERPROC, glBindBuffer) X(PFNGLBUFFERDATAPROC, glBufferData) \
    X(PFNGLENABLEVERTEXATTRIBARRAYPROC, glEnableVertexAttribArray) X(PFNGLVERTEXATTRIBPOINTERPROC, glVertexAttribPointer) \
    X(PFNGLDRAWARRAYSPROC, glDrawArrays) X(PFNGLGENTEXTURESPROC, glGenTextures) X(PFNGLBINDTEXTUREPROC, glBindTexture) \
    X(PFNGLTEXIMAGE2DPROC, glTexImage2D) X(PFNGLTEXPARAMETERIPROC, glTexParameteri) X(PFNGLACTIVETEXTUREPROC, glActiveTexture) \
    X(PFNGLENABLEPROC, glEnable) X(PFNGLDISABLEPROC, glDisable) X(PFNGLBLENDFUNCPROC, glBlendFunc) \
    X(PFNGLBLENDEQUATIONPROC, glBlendEquation) X(PFNGLCOLORMASKPROC, glColorMask) X(PFNGLPOLYGONMODEPROC, glPolygonMode) \
    X(PFNGLVIEWPORTPROC, glViewport) X(PFNGLBINDFRAMEBUFFERPROC, glBindFramebuffer) X(PFNGLPIXELSTOREIPROC, glPixelStorei) \
    X(PFNGLDEPTHMASKPROC, glDepthMask)
#define X(t, n) static t o_##n;
OVF(X)
#undef X

static GLuint g_prog, g_vao, g_vbo, g_tex;
static GLint u_screen, u_tex;
static int g_w, g_h, g_ready;
static float *g_v; static int g_nv, g_cap;
extern SDL_Window *gl_window(void);

static const char *vs =
    "#version 330 core\n"
    "layout(location=0) in vec2 p; layout(location=1) in vec2 uv; layout(location=2) in vec4 c;\n"
    "uniform vec2 screen; out vec2 vuv; out vec4 vc;\n"
    "void main() { gl_Position = vec4(p.x / screen.x * 2.0 - 1.0, 1.0 - p.y / screen.y * 2.0, 0.0, 1.0); vuv = uv; vc = c; }\n";
static const char *fs =
    "#version 330 core\n"
    "in vec2 vuv; in vec4 vc; out vec4 o; uniform sampler2D tex;\n"
    "void main() { float a = vuv.x < 0.0 ? 1.0 : texture(tex, vuv).r; o = vec4(vc.rgb, vc.a * a); }\n";

static int init(void)
{
#define X(t, n) o_##n = (t)SDL_GL_GetProcAddress(#n); if (!o_##n) return -1;
    OVF(X)
#undef X
    GLuint v = o_glCreateShader(GL_VERTEX_SHADER), f = o_glCreateShader(GL_FRAGMENT_SHADER);
    o_glShaderSource(v, 1, &vs, NULL); o_glCompileShader(v);
    o_glShaderSource(f, 1, &fs, NULL); o_glCompileShader(f);
    g_prog = o_glCreateProgram(); o_glAttachShader(g_prog, v); o_glAttachShader(g_prog, f); o_glLinkProgram(g_prog);
    u_screen = o_glGetUniformLocation(g_prog, "screen"); u_tex = o_glGetUniformLocation(g_prog, "tex");
    o_glGenVertexArrays(1, &g_vao); o_glGenBuffers(1, &g_vbo);
    o_glBindVertexArray(g_vao); o_glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    o_glEnableVertexAttribArray(0); o_glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 32, (void *)0);
    o_glEnableVertexAttribArray(1); o_glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 32, (void *)8);
    o_glEnableVertexAttribArray(2); o_glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, 32, (void *)16);
    o_glGenTextures(1, &g_tex); o_glActiveTexture(GL_TEXTURE0); o_glBindTexture(GL_TEXTURE_2D, g_tex);
    o_glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    o_glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, FONT_COLS * FONT_CW, FONT_ROWS * FONT_CH, 0, GL_RED, GL_UNSIGNED_BYTE, font_atlas);
    o_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    o_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    o_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    g_ready = 1;
    return 0;
}

void ov_begin(int *w, int *h)
{
    if (!g_ready && init()) { *w = *h = 1; return; }
    SDL_GL_GetDrawableSize(gl_window(), &g_w, &g_h);
    *w = g_w; *h = g_h;
    g_nv = 0;
}

static void vert(float x, float y, float u, float v, u32 c)
{
    if (g_nv + 1 > g_cap) { g_cap = g_cap ? g_cap * 2 : 4096; g_v = realloc(g_v, g_cap * 32); }
    float *p = g_v + 8 * g_nv++;
    p[0] = x; p[1] = y; p[2] = u; p[3] = v;
    p[4] = (c >> 16 & 255) / 255.f; p[5] = (c >> 8 & 255) / 255.f; p[6] = (c & 255) / 255.f; p[7] = (c >> 24) / 255.f;
}
static void quad(float x, float y, float w, float h, float u0, float v0, float u1, float v1, u32 c)
{
    vert(x, y, u0, v0, c); vert(x + w, y, u1, v0, c); vert(x + w, y + h, u1, v1, c);
    vert(x, y, u0, v0, c); vert(x + w, y + h, u1, v1, c); vert(x, y + h, u0, v1, c);
}

/* a soft round glow: full colour in the middle, fading to nothing at radius r */
void ov_glow(float x, float y, float r, u32 c)
{
    u32 edge = c & 0x00ffffffu;
    for (int i = 0; i < 12; i++) {
        float a0 = i * 6.2831853f / 12, a1 = (i + 1) * 6.2831853f / 12;
        vert(x, y, -1, -1, c);
        vert(x + r * cosf(a0), y + r * sinf(a0), -1, -1, edge);
        vert(x + r * cosf(a1), y + r * sinf(a1), -1, -1, edge);
    }
}

/* one solid triangle with a colour per corner (the staff's slash trail) */
void ov_tri(float x0, float y0, u32 c0, float x1, float y1, u32 c1, float x2, float y2, u32 c2)
{
    vert(x0, y0, -1, -1, c0); vert(x1, y1, -1, -1, c1); vert(x2, y2, -1, -1, c2);
}
/* this batch adds light instead of covering (glows that brighten what's behind) */
static int g_additive;
void ov_additive(int on) { g_additive = on; }

/* rgba as 0xAARRGGBB */
void ov_rect(float x, float y, float w, float h, u32 c) { quad(x, y, w, h, -1, -1, -1, -1, c); }

/* size = line height in pixels; returns the x after the text */
float ov_text(float x, float y, float size, u32 c, const char *s)
{
    float ch = size, cw = size * FONT_CW / FONT_CH;
    float aw = FONT_COLS * FONT_CW, ah = FONT_ROWS * FONT_CH;
    for (; *s; s++) {
        int g = (unsigned char)*s - 32;
        if (g > 0 && g < 95) {
            float u0 = (g % FONT_COLS) * FONT_CW / aw, v0 = (g / FONT_COLS) * FONT_CH / ah;
            quad(x + 1, y + 1, cw, ch, u0, v0, u0 + FONT_CW / aw, v0 + FONT_CH / ah, 0x90000000u);  /* shadow */
            quad(x, y, cw, ch, u0, v0, u0 + FONT_CW / aw, v0 + FONT_CH / ah, c);
        }
        x += cw;
    }
    return x;
}

void ov_end(void)
{
    if (!g_ready || !g_nv) return;
    o_glBindFramebuffer(GL_FRAMEBUFFER, 0);
    o_glViewport(0, 0, g_w, g_h);
    o_glDisable(GL_DEPTH_TEST); o_glDisable(GL_CULL_FACE); o_glDisable(GL_SCISSOR_TEST); o_glDisable(GL_STENCIL_TEST);
    o_glDepthMask(GL_FALSE);
    o_glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    o_glColorMask(1, 1, 1, 1);
    o_glEnable(GL_BLEND); o_glBlendEquation(GL_FUNC_ADD); o_glBlendFunc(GL_SRC_ALPHA, g_additive ? GL_ONE : GL_ONE_MINUS_SRC_ALPHA);
    o_glUseProgram(g_prog);
    o_glUniform2f(u_screen, (float)g_w, (float)g_h);
    o_glUniform1i(u_tex, 0);
    o_glActiveTexture(GL_TEXTURE0); o_glBindTexture(GL_TEXTURE_2D, g_tex);
    o_glBindVertexArray(g_vao); o_glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    o_glBufferData(GL_ARRAY_BUFFER, g_nv * 32, g_v, GL_STREAM_DRAW);
    o_glDrawArrays(GL_TRIANGLES, 0, g_nv);
    o_glDisable(GL_BLEND);
    g_nv = 0; g_additive = 0;
}
