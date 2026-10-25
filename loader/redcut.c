/* Red in cutscenes.
 *
 * A cutscene (XSEItemHandler_CutScene: vtable 0x316690, update slot 16 0xadbf0) plays its own
 * scene animator (handler +0x408; +0x44c bit 0 = finished) with its own actors - the player's item
 * is only locked meanwhile, never animated. So while Red is the player the scene still shows the
 * cutscene's Spyro (an actor from spyronpc.edb, 0x01000086) and Red is left standing wherever the
 * player was. Here, every frame of a scene: the Spyro actor is found among the scene's tracks
 * (scene +0x104: 0x20-byte entries, count byte +0x118, entry +8 = actor animator, vt 0x33a3f8 -
 * recreated at every shot change), not drawn (bit 0 of +0x10), and the player item - Red - is put
 * where his root is (bone record 0 of [out+0x28]: +0x18 position, +0x28 quaternion xyzw, in world
 * space), facing his way, at a size that fits the dialogue framing. In shots without Spyro Red
 * isn't drawn either. Red stands in his idle: Spyro's cutscene clips are for Spyro's skeleton.
 * When the scene ends he is put back where he stood, unless the game moved the player itself. */
#include "xbox.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

extern int kmem_committed(u32 a);
static int ok_ptr(u32 p) { return p >= 0x10000 && p < 0x80000000u && (p < 0x10000000u || kmem_committed(p & ~0xfffu)); }
#define PLAYER_OBJ 0x44c704u
#define CS_VTABLE  0x316690u

static u32 g_cs[16]; static int g_ncs;            /* cutscene handlers seen updating */
u32 cs_upd_orig;
void cs_upd_c(u32 h)
{
    for (int i = 0; i < g_ncs; i++) if (g_cs[i] == h) return;
    for (int i = 0; i < g_ncs; i++) if (!ok_ptr(g_cs[i]) || *(u32 *)(uintptr_t)g_cs[i] != CS_VTABLE) { g_cs[i] = h; return; }
    if (g_ncs < 16) g_cs[g_ncs++] = h;
}
__asm__(".globl cs_upd_thunk\ncs_upd_thunk:\n pushal\n pushl %ecx\n call cs_upd_c\n addl $4,%esp\n popal\n jmp *cs_upd_orig\n");
void cs_upd_thunk(void);

static u32 file_of_res(u32 res)
{
    u32 tab = *(u32 *)(uintptr_t)0x34eca4u, n = *(u32 *)(uintptr_t)0x34eca0u;
    if (!ok_ptr(tab)) return 0;
    for (u32 i = 0; i < n && i < 4096; i++) if (((u32 *)(uintptr_t)tab)[2 * i + 1] == res) return ((u32 *)(uintptr_t)tab)[2 * i];
    return 0;
}
static u32 cs_scene(void)
{
    for (int i = 0; i < g_ncs; i++) {
        u32 h = g_cs[i];
        if (!ok_ptr(h) || *(u32 *)(uintptr_t)h != CS_VTABLE) continue;
        u32 sc = *(u32 *)(uintptr_t)(h + 0x408);
        if (ok_ptr(sc) && !(*(u32 *)(uintptr_t)(h + 0x44c) & 1)) return sc;
    }
    return 0;
}
static u32 spyro_actor(u32 scene)
{
    u32 arr = *(u32 *)(uintptr_t)(scene + 0x104); int cnt = *(u8 *)(uintptr_t)(scene + 0x118);
    for (int e = 0; e < cnt && e < 64 && ok_ptr(arr); e++) {
        u32 q = *(u32 *)(uintptr_t)(arr + e * 0x20 + 8);
        if (!ok_ptr(q) || *(u32 *)(uintptr_t)q != 0x33a3f8u) continue;
        u32 out = *(u32 *)(uintptr_t)(q + 0x104);
        if (ok_ptr(out) && file_of_res(*(u32 *)(uintptr_t)(out + 0x18)) == 0x01000086u) return q;
    }
    return 0;
}

int g_red_in_cutscene;
/* after the frame's Red animation (it sets his size and draw bit) */
void redcut_tick(u32 red_anim)
{
    static int installed;
    if (!installed) {
        installed = 1;
        if (getenv("XBOX_CS_SPYRO")) return;                          /* keep the game's Spyro */
        u32 *vt = (u32 *)(uintptr_t)CS_VTABLE;
        cs_upd_orig = vt[16]; vt[16] = (u32)(uintptr_t)cs_upd_thunk;
    }
    if (!cs_upd_orig) return;
    static int was; static float home[3], put[3]; static float home_yaw;
    u32 pl = *(u32 *)(uintptr_t)PLAYER_OBJ;
    u32 sc = red_anim && ok_ptr(pl) ? cs_scene() : 0;
    u32 item = ok_ptr(pl) ? *(u32 *)(uintptr_t)(pl + 4) : 0;
    if (!sc || !ok_ptr(item)) {
        if (was && ok_ptr(item)) {        /* back where he stood, unless the game put the player somewhere */
            float *P = (float *)(uintptr_t)(item + 0xc8);
            if (fabsf(P[0] - put[0]) + fabsf(P[1] - put[1]) + fabsf(P[2] - put[2]) < 1.0f) {
                float *m = (float *)(uintptr_t)(pl + 0x118);
                for (int k = 0; k < 3; k++) { P[k] = home[k]; m[k] = home[k]; ((float *)(uintptr_t)(pl + 0x128))[k] = home[k]; }
                u32 ph = *(u32 *)(uintptr_t)(item + 0x148); if (ok_ptr(ph)) memcpy((void *)(uintptr_t)(ph + 0x140), home, 12);
                *(float *)(uintptr_t)(item + 0xdc) = home_yaw; *(u8 *)(uintptr_t)(item + 0x7e) = 0;
            }
            klog("redcut: cutscene over\n");
        }
        was = 0; g_red_in_cutscene = 0;
        return;
    }
    float *P = (float *)(uintptr_t)(item + 0xc8);
    if (!was) { was = 1; memcpy(home, P, 12); memcpy(put, P, 12); home_yaw = *(float *)(uintptr_t)(item + 0xdc); klog("redcut: cutscene, Red stands in for Spyro\n"); }
    g_red_in_cutscene = 1;
    u32 q = spyro_actor(sc);
    if (!q) { *(u8 *)(uintptr_t)(red_anim + 0x10) &= ~1; return; }     /* a shot without Spyro: no Red either */
    u32 out = *(u32 *)(uintptr_t)(q + 0x104);
    const float *R = (const float *)(uintptr_t)*(u32 *)(uintptr_t)(out + 0x28);
    if (!ok_ptr((u32)(uintptr_t)R)) return;
    *(u8 *)(uintptr_t)(q + 0x10) &= ~1;                                 /* the cutscene's Spyro: not drawn */
    float *rs = (float *)(uintptr_t)(red_anim + 0xe8);
    static float size = -1; if (size < 0) size = getenv("XBOX_CS_RED_SCALE") ? atof(getenv("XBOX_CS_RED_SCALE")) : 0.35f;
    rs[0] = rs[1] = rs[2] = size;
    *(u8 *)(uintptr_t)(red_anim + 0x10) |= 1;
    float p[3] = { R[6], R[7], R[8] }, x = R[10], y = R[11], z = R[12], w = R[13];
    float fx = -(2 * (x * z + w * y)), fz = -(1 - 2 * (x * x + y * y));  /* his root's -z axis */
    float *m = (float *)(uintptr_t)(pl + 0x118), off[3];
    for (int k = 0; k < 3; k++) { off[k] = ((float *)(uintptr_t)(pl + 0x4b4))[k] - m[k]; if (fabsf(off[k]) > 4) off[k] = 0; }
    for (int k = 0; k < 3; k++) {
        m[k] = p[k]; ((float *)(uintptr_t)(pl + 0x128))[k] = p[k]; ((float *)(uintptr_t)(pl + 0x4b4))[k] = p[k] + off[k];
        P[k] = p[k];
    }
    memcpy(put, p, 12);
    *(float *)(uintptr_t)(item + 0xdc) = atan2f(-fx, -fz);
    *(u8 *)(uintptr_t)(item + 0x7e) = 0;
}
