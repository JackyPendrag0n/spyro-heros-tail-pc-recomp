/* Free camera (renderer level).
 *
 * The game keeps its camera in the render context ([0x4703f0]):
 *   +0x8b4  projection (4x4, row-vector convention)
 *   +0x8f4  view * projection * depth scale (4x4) = VPg
 *   +0x934  camera position (world)     +0x944  camera forward direction (world)
 * Every 3D draw uploads M = (W * VPg)^T as vertex constants c96..c99. With a free camera view Vf
 * and Vg rebuilt from the game's position/direction, drawing with
 *     M' = X^T * M,   X = VPg^-1 * Vf * Vg^-1 * VPg
 * renders the world as seen from the free camera (exactly the game view when Vf == Vg).
 * The game gets no input while the free camera is on, so it stands still.
 * Limitation: the game still culls with its own camera, so geometry far outside the original
 * view can be missing. */
#include "gfx.h"
#include <SDL2/SDL.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>

extern int g_freecam_enabled;
#define RCTX_PTR 0x4703f0u

static double g_pos[3], g_dir[3];          /* free camera, world space */
static double g_up_sign = 1, g_right_sign = 1;
static float g_T[16];                      /* row-major, applied to constant columns */
static int g_T_valid, g_was_enabled;

static int inv4(const double *m, double *o)
{
    double a[4][8];
    for (int i = 0; i < 4; i++) for (int j = 0; j < 8; j++) a[i][j] = j < 4 ? m[i * 4 + j] : (j - 4 == i);
    for (int c = 0; c < 4; c++) {
        int p = c; for (int r = c + 1; r < 4; r++) if (fabs(a[r][c]) > fabs(a[p][c])) p = r;
        if (fabs(a[p][c]) < 1e-15) return -1;
        for (int j = 0; j < 8; j++) { double t = a[c][j]; a[c][j] = a[p][j]; a[p][j] = t; }
        double d = a[c][c]; for (int j = 0; j < 8; j++) a[c][j] /= d;
        for (int r = 0; r < 4; r++) if (r != c) { double f = a[r][c]; for (int j = 0; j < 8; j++) a[r][j] -= f * a[c][j]; }
    }
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) o[i * 4 + j] = a[i][j + 4];
    return 0;
}
static void mul4(const double *a, const double *b, double *o)
{
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) {
        double s = 0; for (int k = 0; k < 4; k++) s += a[i * 4 + k] * b[k * 4 + j]; o[i * 4 + j] = s;
    }
}
static void norm3(double *v) { double l = sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); if (l > 1e-12) for (int i = 0; i < 3; i++) v[i] /= l; }
static void cross3(const double *a, const double *b, double *o)
{ o[0] = a[1] * b[2] - a[2] * b[1]; o[1] = a[2] * b[0] - a[0] * b[2]; o[2] = a[0] * b[1] - a[1] * b[0]; }

/* row-vector look-at (any fixed handedness works: it cancels against the game's own view) */
static void lookat(const double *eye, const double *dir, double *V)
{
    double z[3] = { dir[0], dir[1], dir[2] }, up[3] = { 0, 1, 0 }, x[3], y[3];
    norm3(z);
    cross3(up, z, x); norm3(x);
    cross3(z, x, y);
    double M[16] = { x[0], y[0], z[0], 0,  x[1], y[1], z[1], 0,  x[2], y[2], z[2], 0,
                     -(x[0] * eye[0] + x[1] * eye[1] + x[2] * eye[2]), -(y[0] * eye[0] + y[1] * eye[1] + y[2] * eye[2]),
                     -(z[0] * eye[0] + z[1] * eye[1] + z[2] * eye[2]), 1 };
    memcpy(V, M, sizeof M);
}

static int guest_ok(u32 p) { return p >= 0x10000 && p < 0x80000000u; }

/* screen-space sign of a world offset seen from the game camera (+1 right/up) */
static double screen_sign(const double *VP, const double *pos, const double *dir, const double *off, int axis)
{
    double p[4] = { pos[0] + dir[0] * 10 + off[0], pos[1] + dir[1] * 10 + off[1], pos[2] + dir[2] * 10 + off[2], 1 };
    double c[4] = { 0 };
    for (int j = 0; j < 4; j++) for (int k = 0; k < 4; k++) c[j] += p[k] * VP[k * 4 + j];
    if (fabs(c[3]) < 1e-9) return 1;
    return (c[axis] / c[3]) >= 0 ? 1 : -1;
}

void freecam_note_world(const float (*c)[4]) { (void)c; }

void freecam_frame(float dt)
{
    if (!g_freecam_enabled) { g_T_valid = 0; g_was_enabled = 0; return; }
    u32 rc = *(u32 *)(uintptr_t)RCTX_PTR;
    if (!guest_ok(rc)) { g_T_valid = 0; return; }
    const float *vpf = (const float *)(uintptr_t)(rc + 0x8f4);
    const float *gpos = (const float *)(uintptr_t)(rc + 0x934);
    const float *gdir = (const float *)(uintptr_t)(rc + 0x944);
    double VPg[16], VPgi[16], gp[3] = { gpos[0], gpos[1], gpos[2] }, gd[3] = { gdir[0], gdir[1], gdir[2] };
    for (int i = 0; i < 16; i++) VPg[i] = vpf[i];
    norm3(gd);
    if (inv4(VPg, VPgi) || !isfinite(gp[0]) || fabs(gd[0]) + fabs(gd[1]) + fabs(gd[2]) < 0.5) { g_T_valid = 0; return; }
    if (!g_was_enabled) {            /* start exactly at the game camera */
        memcpy(g_pos, gp, sizeof gp); memcpy(g_dir, gd, sizeof gd);
        double up[3] = { 0, 1, 0 }, x[3];
        cross3(up, gd, x); norm3(x);
        g_right_sign = screen_sign(VPg, gp, gd, x, 0);
        g_up_sign = 1;                  /* world +y is up (the projection's y axis points down, so no screen test) */
        g_was_enabled = 1;
        if (getenv("XBOX_NOCLIP_LOG")) klog("freecam: right sign %.0f up sign %.0f\n", g_right_sign, g_up_sign);
    }
    extern void freecam_read_controls(float *mx, float *my, float *mz, float *lx, float *ly, int *fast);
    float mx, my, mz, lx, ly; int fast;
    freecam_read_controls(&mx, &my, &mz, &lx, &ly, &fast);
    if (dt > 0.1f) dt = 0.1f;
    /* basis of the free camera, oriented as the player sees it */
    double up[3] = { 0, g_up_sign, 0 }, right[3], wup[3] = { 0, 1, 0 };
    cross3(wup, g_dir, right); norm3(right);
    for (int i = 0; i < 3; i++) right[i] *= g_right_sign;
    /* look: yaw towards screen-right, pitch towards screen-up */
    double a = lx * 2.0 * dt, b = ly * 1.5 * dt;
    double nd[3];
    for (int i = 0; i < 3; i++) nd[i] = g_dir[i] * cos(a) + right[i] * sin(a);
    norm3(nd);
    double nd2[3], vup[3];
    cross3(nd, right, vup);                                   /* camera-relative up, sign fixed below */
    if (vup[0] * up[0] + vup[1] * up[1] + vup[2] * up[2] < 0) for (int i = 0; i < 3; i++) vup[i] = -vup[i];
    for (int i = 0; i < 3; i++) nd2[i] = nd[i] * cos(b) + vup[i] * sin(b);
    norm3(nd2);
    if (fabs(nd2[1]) < 0.985) memcpy(g_dir, nd2, sizeof nd2); else memcpy(g_dir, nd, sizeof nd);
    cross3(wup, g_dir, right); norm3(right);
    for (int i = 0; i < 3; i++) right[i] *= g_right_sign;
    double speed = (fast ? 40.0 : 8.0) * dt;
    for (int i = 0; i < 3; i++) g_pos[i] += (right[i] * mx + up[i] * my + g_dir[i] * mz) * speed;
    if (getenv("XBOX_NOCLIP_LOG")) { static int n; if (n++ % 30 == 0) klog("freecam pos %.2f %.2f %.2f dir %.2f %.2f %.2f\n", g_pos[0], g_pos[1], g_pos[2], g_dir[0], g_dir[1], g_dir[2]); }

    double Vg[16], Vgi[16], Vf[16], t1[16], t2[16], X[16];
    lookat(gp, gd, Vg);
    lookat(g_pos, g_dir, Vf);
    if (inv4(Vg, Vgi)) { g_T_valid = 0; return; }
    mul4(Vf, Vgi, t1);       /* Vf * Vg^-1 */
    mul4(t1, VPg, t2);       /* ... * VPg  */
    mul4(VPgi, t2, X);       /* VPg^-1 * ... */
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) g_T[i * 4 + j] = (float)X[j * 4 + i];   /* T = X^T */
    g_T_valid = 1;
}

/* apply to a copy of the constants about to be uploaded: columns c96..c99 */
int freecam_apply(float (*c)[4])
{
    if (!g_T_valid) return 0;
    /* only perspective transforms (screen-space quads and HUD use affine ones) */
    float w2 = c[96][3] * c[96][3] + c[97][3] * c[97][3] + c[98][3] * c[98][3];
    float x2 = c[96][0] * c[96][0] + c[97][0] * c[97][0] + c[98][0] * c[98][0];
    if (!(w2 > 1e-6f * x2)) return 0;
    for (int j = 96; j < 100; j++) {
        float v[4];
        for (int i = 0; i < 4; i++) v[i] = g_T[i * 4 + 0] * c[j][0] + g_T[i * 4 + 1] * c[j][1] + g_T[i * 4 + 2] * c[j][2] + g_T[i * 4 + 3] * c[j][3];
        memcpy(c[j], v, 16);
    }
    return 1;
}

int freecam_active(void) { return g_T_valid; }

/* camera-relative movement basis for noclip: forward on the ground plane, screen-right, up */
int freecam_ground_basis(double fwd[3], double right[3], double up[3])
{
    u32 rc = *(u32 *)(uintptr_t)RCTX_PTR;
    if (!guest_ok(rc)) return -1;
    const float *vpf = (const float *)(uintptr_t)(rc + 0x8f4);
    const float *gpos = (const float *)(uintptr_t)(rc + 0x934);
    const float *gdir = (const float *)(uintptr_t)(rc + 0x944);
    double VP[16], gp[3] = { gpos[0], gpos[1], gpos[2] }, gd[3] = { gdir[0], 0, gdir[2] };
    for (int i = 0; i < 16; i++) VP[i] = vpf[i];
    norm3(gd);
    if (!isfinite(gd[0]) || fabs(gd[0]) + fabs(gd[2]) < 0.5) return -1;
    double wup[3] = { 0, 1, 0 }, x[3];
    cross3(wup, gd, x); norm3(x);
    /* which way do +x and +y point on screen: project a point ahead and the same point offset */
    double rs = 1, us = 1, P[3][4];
    for (int k = 0; k < 3; k++) {
        double p[4] = { gp[0] + gdir[0] * 10, gp[1] + gdir[1] * 10, gp[2] + gdir[2] * 10, 1 };
        for (int i = 0; i < 3; i++) p[i] += k == 1 ? x[i] : k == 2 ? wup[i] : 0;
        for (int j = 0; j < 4; j++) { P[k][j] = 0; for (int i = 0; i < 4; i++) P[k][j] += p[i] * VP[i * 4 + j]; }
        if (fabs(P[k][3]) < 1e-9) return -1;
    }
    if (P[1][0] / P[1][3] < P[0][0] / P[0][3]) rs = -1;
    if (P[2][1] / P[2][3] < P[0][1] / P[0][3]) us = -1;
    (void)us;                       /* clip-space y points down here; world +y is up */
    for (int i = 0; i < 3; i++) { fwd[i] = gd[i]; right[i] = x[i] * rs; up[i] = wup[i]; }
    return 0;
}
