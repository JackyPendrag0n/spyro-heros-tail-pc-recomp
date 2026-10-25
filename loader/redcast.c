/* Red's spellcasting.
 *
 * Breath -> his own particles. Spyro's breath effects are started through 0x376d0 (cdecl: file,
 * effect, pos[4], rot[4], flags, scale) from Spyro's file 0x01000040; Red's boss file bossred.edb
 * (0x010000db) holds his own versions under the same ids - 0x040000b7 his flame breath, 0x0400007a
 * his ice - which his boss starts the same way (message handler 0xbee50). While Red is the player
 * the breath starts are pointed at his file instead: the game's breath (and everything it does to
 * enemies and puzzles) is unchanged, only what comes out looks like his.
 *
 * Aimed casts. Spyro's first-person view (mode 32, White / left trigger) fires the current breath
 * as a projectile: breath object slot 1 (thiscall obj, 1, 0) - fire 0xe3370, ice 0xe3a80, electric
 * 0xe4090, water 0xe5670 - spawns it from the game camera's eye (camera controller [0x42bb18]+0x4b8:
 * +0x284 eye, +0x294 look-at) along eye -> look-at, and the shot does the breath's job (burns,
 * freezes, powers, puts out) when it hits. Breath objects live in the player handler: +0x13f0
 * fire (type 1), +0x1460 electric (2), +0x1428 ice (4), +0x1498 water (8); the current type is
 * [0x43cd10]. Red's cast calls slot 1 with the camera's eye / look-at swapped for his staff head and
 * the aim point for the length of the call. Spyro's shots use up his bombs (0x43cd24 fire, +0x28
 * ice, +0x2c electric); Red's don't. */
#include "xbox.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#define PLAYER_OBJ 0x44c704u
#define RED_FILE   0x010000dbu
#define SPYRO_FILE 0x01000040u
static int ok_ptr(u32 p) { return p >= 0x10000 && p < 0x80000000u; }
extern u32 g_redchar_anim;
u32 debugmenu_play_frame(void);

/* ---- breath effects -> his ---- */
u32 g_fx_remapped;
void fx_remap_c(u32 *args)                 /* args[-1] = return address, args[0] file, args[1] effect */
{
    if (getenv("XBOX_FX_LOG2") && args[-1] >= 0xc0000 && args[-1] < 0x106000) klog("fx start %08x %08x from %08x\n", args[0], args[1], args[-1]);
    extern int g_quiet_impact;
    if (g_quiet_impact && args[1] == 0x04000068u) { args[0] = RED_FILE; args[1] = 0x040001feu; return; }   /* (not in his file: nothing) */
    {   extern float g_impact_scale_pub;               /* a charged shot / ice shard: a bigger burst */
        if (g_impact_scale_pub > 1 && args[1] == 0x04000068u) ((float *)args)[11] *= g_impact_scale_pub; }
    if (!g_redchar_anim || args[0] != SPYRO_FILE || getenv("XBOX_NO_FXREMAP")) return;
    u32 ret = args[-1];
    if (ret < 0xc0000 || ret >= 0x106000) return;      /* only the player's own code */
    if (args[1] == 0x040000b7u || args[1] == 0x0400007au) { args[0] = RED_FILE; g_fx_remapped++; }
}
__asm__(".globl fx_start_stub\n"
        "fx_start_stub:\n"
        "  pushal\n"
        "  leal 36(%esp), %eax\n"          /* 32 bytes of registers, then the return address */
        "  pushl %eax\n"
        "  call fx_remap_c\n"
        "  addl $4, %esp\n"
        "  popal\n"
        "  movl 0x30(%esp), %eax\n"
        "  movl 0x2c(%esp), %ecx\n"
        "  pushl $0x376d8\n"
        "  ret\n");
void fx_start_stub(void);

/* start one of his effects at a point (no owner): returns the effect object */
u32 red_fx_at(u32 effect, const float pos[3], float yaw)
{
    typedef u32 (*fx_fn)(u32, u32, float, float, float, float, float, float, float, float, u32, float);
    static int ax = -1; if (ax < 0) ax = getenv("XBOX_FX_ROTAX") ? atoi(getenv("XBOX_FX_ROTAX")) : 1;
    float r[3] = { 0, 0, 0 }; r[ax] = yaw;
    return ((fx_fn)(uintptr_t)0x376d0u)(RED_FILE, effect, pos[0], pos[1], pos[2], 0, r[0], r[1], r[2], 0, 0, 1.0f);
}

/* his staff head in the world (redpose.c's FK of his final pose, model space) */
int red_staff_tip(float out[3])
{
    extern float g_staff_head[3]; extern int g_staff_head_ok;
    u32 pl = *(u32 *)(uintptr_t)PLAYER_OBJ;
    if (!g_redchar_anim || !g_staff_head_ok || !ok_ptr(pl)) return 0;
    u32 item = *(u32 *)(uintptr_t)(pl + 4); if (!ok_ptr(item)) return 0;
    const float *P = (const float *)(uintptr_t)(item + 0xc8), *sc = (const float *)(uintptr_t)(g_redchar_anim + 0xe8);
    float yaw = *(float *)(uintptr_t)(item + 0xdc), c = cosf(yaw), s = sinf(yaw);
    float lx = g_staff_head[0] * sc[0], ly = g_staff_head[1] * sc[1], lz = g_staff_head[2] * sc[2];
    out[0] = P[0] + lx * c + lz * s; out[1] = P[1] + ly; out[2] = P[2] - lx * s + lz * c;
    return 1;
}

/* ---- a cast: the current breath's shot from 'from' toward 'to' ---- */
float g_cast_power = 1.0f;
int red_cast_shot(const float from[3], const float to[3])
{
    u32 pl = *(u32 *)(uintptr_t)PLAYER_OBJ;
    u32 mgr = *(u32 *)(uintptr_t)0x42bb18u;
    if (!ok_ptr(pl) || !ok_ptr(mgr)) return 0;
    u32 cam = *(u32 *)(uintptr_t)(mgr + 0x4b8);
    if (!ok_ptr(cam)) return 0;
    u32 type = *(u32 *)(uintptr_t)0x43cd10u;
    u32 off = type == 2 ? 0x1460 : type == 4 ? 0x1428 : type == 8 ? 0x1498 : 0x13f0;
    u32 obj = pl + off, vt = *(u32 *)(uintptr_t)obj;
    if (!ok_ptr(vt)) return 0;
    typedef u32 (__attribute__((thiscall)) *shot_fn)(void *self, u32 aimed, u32 b);
    shot_fn fn = (shot_fn)(uintptr_t)*(u32 *)(uintptr_t)(vt + 4);
    float *ce = (float *)(uintptr_t)(cam + 0x284), *ct = (float *)(uintptr_t)(cam + 0x294);
    float save_e[4], save_t[4]; memcpy(save_e, ce, 16); memcpy(save_t, ct, 16);
    u8 ammo[12]; memcpy(ammo, (void *)(uintptr_t)0x43cd20u, 12);
    float shake[6]; memcpy(shake, (void *)(uintptr_t)(cam + 0x3a0), sizeof shake);
    for (int k = 0; k < 3; k++) { ce[k] = from[k]; ct[k] = to[k]; }
    /* +0x24 / +0x28: how long the shot was charged, of the most (speed 0.1 .. 0.3 by that share;
     * both 0 outside the first-person view - 0/0) */
    float *chg = (float *)(uintptr_t)(obj + 0x24), save_c[3] = { chg[0], chg[1], chg[2] };
    if (getenv("XBOX_CAST_LOG")) klog("cast charge %g / %g cool %g\n", chg[0], chg[1], *(float *)(uintptr_t)(obj + 0x2c));
    chg[0] = g_cast_power; chg[1] = 1.0f;
    u32 r = fn((void *)(uintptr_t)obj, 1, 0);
    chg[0] = save_c[0]; chg[1] = save_c[1]; chg[2] = save_c[2];   /* (and the breath's own cooldown, +0x2c)*/
    memcpy(ce, save_e, 16); memcpy(ct, save_t, 16);
    memcpy((void *)(uintptr_t)0x43cd20u, ammo, 12);                 /* his casts don't use Spyro's bombs */
    memcpy((void *)(uintptr_t)(cam + 0x3a0), shake, sizeof shake);   /* no first-person kick on the camera */
    if (getenv("XBOX_CAST_LOG")) klog("cast type %u -> %u from %.1f %.1f %.1f to %.1f %.1f %.1f\n", type, r & 0xff, from[0], from[1], from[2], to[0], to[1], to[2]);
    return (r & 0xff) != 0;
}

/* ---- his shots in flight ----
 * Every shot sets its starting velocity through 0xd3cc0 (thiscall handler, vec4*: physics +0xe8 /
 * +0xf0 across per frame, +0x7c up = y * 60 per second), straight after it is spawned. While one of
 * Red's casts is being made that call is caught: the handler is remembered and its velocity made
 * ours - straight along the aim and much faster than Spyro's lob - and its class's update (vtable
 * slot 16, fire 0xda9f0, the ice and electric shots their own) is wrapped to keep it that way
 * (no drop) and, for the staff's hits, to keep it short-lived and unseen. */
typedef int (__attribute__((thiscall)) *upd_fn)(void *self);
static struct { u32 vt; upd_fn orig; } g_vts[6]; static int g_nvt;
typedef struct { u32 h; float dir[3], spd, life, big; int melee, st0, first, hit, gen; float pos[3]; double seen; u32 type; } rproj;
rproj g_rp[24];
extern int g_cast_launch; static void magic_draw(int w, int h, double now); int red_magic_spend(float amt);
static RS int g_in_cast, g_cast_melee, g_cast_gen; static RS float g_cast_dir[3], g_cast_spd, g_cast_life, g_cast_big;
static struct { float pos[3], dir[3]; u32 type; int gen; } g_forks[8]; static int g_nforks;   /* electric bolts forking on a hit */
int g_melee_hit;                                   /* a staff hit landed (read by debugmenu.c) */
int g_quiet_impact;
float g_impact_scale_pub;
static double now_s(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec * 1e-9; }
static rproj *rp_find(u32 h) { for (int i = 0; i < 24; i++) if (g_rp[i].h == h) return &g_rp[i]; return NULL; }
static int __attribute__((thiscall)) proj_update(void *self)
{
    u32 h = (u32)(uintptr_t)self, it = *(u32 *)(uintptr_t)(h + 4), vt = *(u32 *)(uintptr_t)h;
    upd_fn orig = NULL;
    for (int i = 0; i < g_nvt; i++) if (g_vts[i].vt == vt) orig = g_vts[i].orig;
    if (!orig) return 0;
    rproj *r = rp_find(h);
    if (r && ok_ptr(it)) {
        u32 ph = *(u32 *)(uintptr_t)(it + 0x148);
        float *life = (float *)(uintptr_t)(h + 0x410); u32 st = *(u32 *)(uintptr_t)(h + 0x40c);
        if (r->first) { r->first = 0; r->st0 = st; if (r->life > 0) *life = r->life; }
        if (st == (u32)r->st0 && ok_ptr(ph)) {
            *(float *)(uintptr_t)(ph + 0xe8) = r->dir[0] * r->spd;
            *(float *)(uintptr_t)(ph + 0xf0) = r->dir[2] * r->spd;
            *(float *)(uintptr_t)(ph + 0x7c) = r->dir[1] * r->spd * 60.0f;
        }
        static int mvis = -1; if (mvis < 0) mvis = getenv("XBOX_MELEE_VIS") != NULL;
        if (r->melee && !mvis) *(u16 *)(uintptr_t)(it + 0x10) &= ~1;          /* the staff's hit is unseen */
    }
    if (r && r->melee && ok_ptr(it) && getenv("XBOX_HIT_PROBE")) {      /* research: what the game's hit loop sees near a staff hit */
        u32 head = *(u32 *)(uintptr_t)0x446608u, n = ok_ptr(head) ? *(u32 *)(uintptr_t)head : 0;
        const float *Q = (const float *)(uintptr_t)(it + 0xc8);
        for (int guard = 0; ok_ptr(n) && guard < 4000; guard++, n = *(u32 *)(uintptr_t)(n + 4)) {
            u32 t = *(u32 *)(uintptr_t)(n + 8); if (!ok_ptr(t) || t == it) continue;
            const float *T = (const float *)(uintptr_t)(t + 0xc8);
            float d2 = 0; for (int k = 0; k < 4; k++) d2 += (Q[k] - T[k]) * (Q[k] - T[k]);
            if (d2 > 400) continue;
            u32 an = *(u32 *)(uintptr_t)(t + 0x13c);
            klog("hitprobe proj %08x near item %08x d %.1f f10 %02x anim %08x af %02x player %d\n", h, t, sqrtf(d2), *(u8 *)(uintptr_t)(t + 0x10), an,
                 ok_ptr(an) ? *(u8 *)(uintptr_t)(an - 4 + 0x10) : 0xff, t == *(u32 *)(uintptr_t)0x44c708u);
        }
    }
    if (getenv("XBOX_PROJ_LOG") && ok_ptr(it)) { static u32 n; if (r || n++ % 8 == 0) { const float *p = (const float *)(uintptr_t)(it + 0xc8);
        klog("proj %08x%s pos %.1f %.1f %.1f life %.2f st %u vis %x\n", h, r ? (r->melee ? " M" : " R") : "", p[0], p[1], p[2], *(float *)(uintptr_t)(h + 0x410), *(u32 *)(uintptr_t)(h + 0x40c), *(u16 *)(uintptr_t)(it + 0x10)); } }
    /* a staff hit that reaches the end of its reach without meeting anything goes out quietly */
    float life_before = r && ok_ptr(it) ? *(float *)(uintptr_t)(h + 0x410) : 1;
    g_quiet_impact = r && (r->melee || r->gen > 0) && life_before < 0.04f;
    g_impact_scale_pub = r ? r->big : 0;
    int ret = orig(self);
    g_quiet_impact = 0; g_impact_scale_pub = 0;
    if (r && ok_ptr(it) && !r->hit && *(u32 *)(uintptr_t)(h + 0x40c) != (u32)r->st0 && !r->first) {
        r->hit = 1; if (r->melee && life_before >= 0.04f) g_melee_hit++;
        if (!r->melee && r->type == 2 && r->gen < 1 && life_before >= 0.04f && g_nforks < 6) {   /* electric: forks (cast next frame) */
            for (int k = -1; k <= 1; k += 2) {
                float a = 0.75f * k, c = cosf(a), sn = sinf(a);
                g_forks[g_nforks].dir[0] = r->dir[0] * c + r->dir[2] * sn; g_forks[g_nforks].dir[1] = 0.05f;
                g_forks[g_nforks].dir[2] = r->dir[2] * c - r->dir[0] * sn;
                memcpy(g_forks[g_nforks].pos, r->pos, 12); g_forks[g_nforks].pos[1] += 0.5f;
                g_forks[g_nforks].type = r->type; g_forks[g_nforks].gen = r->gen + 1; g_nforks++;
            }
        }
    }
    if (r && ok_ptr(it)) { memcpy(r->pos, (void *)(uintptr_t)(it + 0xc8), 12); r->seen = now_s(); }
    return ret;
}
static void wrap_vt(u32 vt)
{
    for (int i = 0; i < g_nvt; i++) if (g_vts[i].vt == vt) return;
    if (g_nvt == 6 || !ok_ptr(vt) || vt < 0x2f0000 || vt > 0x340000) return;
    u32 *slot = (u32 *)(uintptr_t)(vt + 16 * 4);
    if (*slot < 0x10000 || *slot > 0x2f0000) { klog("redcast: projectile vtable %08x slot 16 = %08x?\n", vt, *slot); return; }
    g_vts[g_nvt].vt = vt; g_vts[g_nvt].orig = (upd_fn)(uintptr_t)*slot; g_nvt++;
    *slot = (u32)(uintptr_t)proj_update;
    klog("redcast: wrapped projectile class %08x (update %08x)\n", vt, (u32)(uintptr_t)g_vts[g_nvt - 1].orig);
}
void vel_set_c(u32 h, float *v)
{
    if (!g_in_cast || !ok_ptr(h)) return;
    u32 vt = *(u32 *)(uintptr_t)h; wrap_vt(vt);
    { void wrap_hit_vt(u32 vt); wrap_hit_vt(vt); }
    int k = 0; for (int i = 1; i < 24; i++) if (g_rp[i].seen < g_rp[k].seen) k = i;   /* the oldest slot */
    rproj *r = &g_rp[k]; memset(r, 0, sizeof *r);
    r->h = h; memcpy(r->dir, g_cast_dir, 12); r->spd = g_cast_spd; r->life = g_cast_life; r->melee = g_cast_melee;
    r->big = g_cast_big; r->gen = g_cast_gen; if (g_cast_melee && g_cast_launch) r->melee = 2;
    r->first = 1; r->seen = now_s(); r->type = *(u32 *)(uintptr_t)0x43cd10u;
    memcpy(r->pos, (void *)(uintptr_t)(*(u32 *)(uintptr_t)(h + 4) + 0xc8), 12);
    v[0] = r->dir[0] * r->spd; v[1] = r->dir[1] * r->spd; v[2] = r->dir[2] * r->spd;
}
__asm__(".globl vel_set_stub\n"
        "vel_set_stub:\n"
        "  pushal\n"
        "  pushl 36(%esp)\n"               /* the velocity */
        "  pushl %ecx\n"                   /* the handler */
        "  call vel_set_c\n"
        "  addl $8, %esp\n"
        "  popal\n"
        "  movl 4(%ecx), %eax\n"
        "  movl 4(%esp), %ecx\n"
        "  pushl $0xd3cc7\n"
        "  ret\n");
void vel_set_stub(void);

/* a cast of his: from 'from' toward 'to' at spd (units a frame); life > 0 cuts it short */
int red_cast(const float from[3], const float to[3], float spd, float life, int melee)
{
    float d[3] = { to[0] - from[0], to[1] - from[1], to[2] - from[2] }, n = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (n < 1e-3f) return 0;
    for (int k = 0; k < 3; k++) g_cast_dir[k] = d[k] / n;
    g_cast_spd = spd; g_cast_life = life; g_cast_melee = melee;
    g_in_cast = 1; int r = red_cast_shot(from, to); g_in_cast = 0;
    return r;
}

void redcast_install(void)
{
    static int done; if (done) return; done = 1;
    static const u8 vwant[] = { 0x8b, 0x41, 0x04, 0x8b, 0x4c, 0x24, 0x04 };
    extern void hle_patch_jmp(u32 at, void *to);
    if (!memcmp((void *)(uintptr_t)0xd3cc0u, vwant, sizeof vwant)) hle_patch_jmp(0xd3cc0u, (void *)vel_set_stub);
    else klog("redcast: velocity hook: unexpected bytes\n");
    if (getenv("XBOX_PROJ_LOG")) wrap_vt(0x319098u);
    static const u8 want[] = { 0x8b, 0x44, 0x24, 0x30, 0x8b, 0x4c, 0x24, 0x2c };
    if (!memcmp((void *)(uintptr_t)0x376d0u, want, sizeof want)) hle_patch_jmp(0x376d0u, (void *)fx_start_stub);
    else klog("redcast: effect start hook: unexpected bytes\n");
}

/* research / tests: XBOX_FX_TEST=+frame:effect[,..] starts one of his effects in front of him;
 * XBOX_CAST_AT=+frame[,..] casts straight ahead */
static int due(const char *list, u32 pf, u32 *arg)
{
    if (!list || !pf) return 0;
    char buf[256]; snprintf(buf, sizeof buf, "%s", list);
    char *sv; for (char *t = strtok_r(buf, ",", &sv); t; t = strtok_r(NULL, ",", &sv)) {
        u32 f = (u32)strtoul(t + (t[0] == '+'), NULL, 10);
        char *c = strchr(t, ':'); if (arg) *arg = c ? (u32)strtoul(c + 1, NULL, 16) : 0;
        if (pf == f) return 1;
    }
    return 0;
}
void redcast_test_tick(void)
{
    redcast_install();
    u32 pf = debugmenu_play_frame();
    u32 pl = *(u32 *)(uintptr_t)PLAYER_OBJ; if (!ok_ptr(pl)) return;
    u32 item = *(u32 *)(uintptr_t)(pl + 4); if (!ok_ptr(item)) return;
    const float *P = (const float *)(uintptr_t)(item + 0xc8);
    float yaw = *(float *)(uintptr_t)(item + 0xdc), fx = -sinf(yaw), fz = -cosf(yaw);
    u32 a;
    if (due(getenv("XBOX_FX_TEST"), pf, &a)) {
        float p[3] = { P[0] + fx * 8, P[1] + 3, P[2] + fz * 8 };
        u32 e = red_fx_at(a, p, yaw);
        klog("fxtest %08x -> %08x at %.1f %.1f %.1f\n", a, e, p[0], p[1], p[2]);
    }
    if (due(getenv("XBOX_BREATH_AT"), pf, &a)) { *(u32 *)(uintptr_t)0x43cd10u = a; klog("breath type -> %u\n", a); }   /* +frame:type (1 2 4 8) */
    if (due(getenv("XBOX_CAST_AT"), pf, &a)) {
        float from[3]; if (!red_staff_tip(from)) { from[0] = P[0] + fx * 2; from[1] = P[1] + 5; from[2] = P[2] + fz * 2; }
        float to[3] = { from[0] + fx * 30, from[1], from[2] + fz * 30 };
        klog("casttest -> %d\n", red_cast_shot(from, to));
    }
    /* XBOX_GOTO_NEAR=+frame:n - stand him 5 units from the n-th nearest animated item (sheep,
     * enemies, NPCs), facing it */
    if (due(getenv("XBOX_GOTO_NEAR"), pf, &a)) {
        u32 best[8] = { 0 }; float bd[8]; int nb = 0;
        u32 head = *(u32 *)(uintptr_t)0x446608u, n = ok_ptr(head) ? *(u32 *)(uintptr_t)head : 0;
        u32 me = *(u32 *)(uintptr_t)0x44c708u;
        for (int guard = 0; ok_ptr(n) && guard < 4000; guard++, n = *(u32 *)(uintptr_t)(n + 4)) {
            u32 it = *(u32 *)(uintptr_t)(n + 8);
            if (!ok_ptr(it) || it == me || it == item || !(*(u8 *)(uintptr_t)(it + 0x10) & 1) || !ok_ptr(*(u32 *)(uintptr_t)(it + 0x13c))) continue;
            const float *Q = (const float *)(uintptr_t)(it + 0xc8);
            float d = (Q[0] - P[0]) * (Q[0] - P[0]) + (Q[2] - P[2]) * (Q[2] - P[2]);
            if (getenv("XBOX_GOTO_ALL")) klog("item %08x vt %08x f10 %02x at %.1f %.1f %.1f d %.1f\n", it, *(u32 *)(uintptr_t)it, *(u8 *)(uintptr_t)(it + 0x10), Q[0], Q[1], Q[2], sqrtf(d));
            { const char *ev = getenv("XBOX_GOTO_VT"); if (ev && *(u32 *)(uintptr_t)it != (u32)strtoul(ev, NULL, 16)) continue; }
            int k; if (nb < 8) k = nb++; else { if (d >= bd[7]) continue; k = 7; }
            while (k > 0 && bd[k - 1] > d) { best[k] = best[k - 1]; bd[k] = bd[k - 1]; k--; }
            best[k] = it; bd[k] = d;
        }
        u32 want = a < 8 ? a : 0;
        for (int i = 0; i < nb; i++) { const float *Q = (const float *)(uintptr_t)(best[i] + 0xc8); klog("near %d: item %08x vt %08x at %.1f %.1f %.1f (%.1f away)\n", i, best[i], *(u32 *)(uintptr_t)best[i], Q[0], Q[1], Q[2], sqrtf(bd[i])); }
        if (want < (u32)nb && getenv("XBOX_GOTO_BRING")) {        /* bring it to him instead: 5 in front */
            float *Q = (float *)(uintptr_t)(best[want] + 0xc8);
            Q[0] = P[0] + fx * 5; Q[1] = P[1] + 0.5f; Q[2] = P[2] + fz * 5;
            u32 ph = *(u32 *)(uintptr_t)(best[want] + 0x148); if (ok_ptr(ph) && *(u32 *)(uintptr_t)(ph + 4) == best[want]) memcpy((void *)(uintptr_t)(ph + 0x140), Q, 12);
            *(u8 *)(uintptr_t)(best[want] + 0x7e) = 0;
            klog("brought %08x to him\n", best[want]); want = 99;
        }
        if (want < (u32)nb) {
            const float *Q = (const float *)(uintptr_t)(best[want] + 0xc8);
            float dx = P[0] - Q[0], dz = P[2] - Q[2], l = sqrtf(dx * dx + dz * dz); if (l < 1e-3f) { dx = 1; dz = 0; l = 1; }
            float *W = (float *)(uintptr_t)(item + 0xc8);
            W[0] = Q[0] + dx / l * 5; W[1] = Q[1] + 1; W[2] = Q[2] + dz / l * 5;
            *(float *)(uintptr_t)(item + 0xdc) = atan2f(dx, dz);     /* forward (-sin, -cos) points at it */
            { u32 ph = *(u32 *)(uintptr_t)(item + 0x148); if (ok_ptr(ph)) memcpy((void *)(uintptr_t)(ph + 0x140), W, 12); }
            *(u8 *)(uintptr_t)(item + 0x7e) = 0;
            klog("goto near %u: %08x\n", want, best[want]);
        }
    }
}

/* ---- aiming ----
 * Left trigger (or the left shoulder / Z) held: an over-the-shoulder view, the right stick aims
 * (he turns with it), the left stick walks him about without turning him, a reticle shows where
 * the shots go, and square casts (held: again and again). The game never sees the trigger or the
 * shoulder while he is the player (they are Spyro's first-person view). Square held without
 * aiming casts straight ahead. */
RS int g_red_aim_in;                        /* input_sdl: trigger / shoulder held */
RS float g_aim_rx, g_aim_ry, g_aim_lx, g_aim_ly;   /* input_sdl: the sticks while aiming (the game gets neither) */
RS int g_red_aiming;                        /* aiming now (input_sdl hides the sticks) */
RS float g_aim_w;                           /* the aim view's blend 0..1 */
RS float g_aim_yaw, g_aim_pitch;
RS float g_cast_recoil;                     /* redpose.c: the staff kicks back on each cast */
RS float g_cam_want_req = NAN;              /* debugmenu.c's modern camera: settle at this yaw */
RS int g_red_casting;                       /* casting (aimed or not) - the aim pose */
float g_shake;                           /* staff hits: a short camera shake */
static RS float g_cast_cool;
RS float g_charge;                          /* a charged cast building 0..1 */
RS int g_red_hovering; static RS int g_red_was_air;
static RS float g_aim_eye[3], g_aim_dir[3];
extern struct pad_state_rc { u16 buttons; u8 analog[8]; s16 lx, ly, rx, ry; } g_pads[4];
extern float g_move_scale, g_move_xz, g_size_cur;
int dm_player_can_change(void);
void sfx_swoosh(float hz, float dur, float vol, float pan);

static float wrapf(float a) { while (a > 3.14159265f) a -= 6.2831853f; while (a < -3.14159265f) a += 6.2831853f; return a; }
static float appr(float v, float t, float k, float dt) { float a = k * dt; if (a > 1) a = 1; return v + (t - v) * a; }
static float red_scale(void) { return g_size_cur > 0.2f ? g_size_cur : 1.0f; }

/* the aim view's eye and the direction it looks (the reticle is where that ray goes) */
#define AIM_BACK 5.5f
static RS float g_aim_room = 1.0f;          /* 0..1: how far back the view may sit (walls behind him) */
static void aim_view(const float *P, float eye[3], float dir[3])
{
    float s = red_scale() * g_aim_room, A = g_aim_yaw, p = g_aim_pitch;
    float f[3] = { -sinf(A), 0, -cosf(A) }, r[3] = { -cosf(A), 0, sinf(A) };
    for (int k = 0; k < 3; k++) dir[k] = f[k] * cosf(p);
    dir[1] = sinf(p);
    float piv[3] = { P[0], P[1] + 4.0f * red_scale(), P[2] };
    float back = AIM_BACK * s, side = 2.1f * s;          /* close over his shoulder */
    for (int k = 0; k < 3; k++) eye[k] = piv[k] - dir[k] * back + r[k] * side;
    if (eye[1] < P[1] + 1.0f * s) eye[1] = P[1] + 1.0f * s;            /* looking up: not under the floor */
}
void red_aim_point(float out[3])
{
    for (int k = 0; k < 3; k++) out[k] = g_aim_eye[k] + g_aim_dir[k] * 70.0f * red_scale();
}

/* one of his spells, from the staff crystal toward the aim (or straight ahead). Each element casts
 * its own way: fire a spread of three, ice one heavy shard (slower, a bigger shatter), electric a
 * fast bolt that forks on a hit, water the game's own. power > 0: a charged cast (0..1) - faster,
 * a tight cluster of three, a much bigger burst where it lands. */
static void rot_y(const float v[3], float a, float out[3]) { float c = cosf(a), sn = sinf(a); out[0] = v[0] * c + v[2] * sn; out[1] = v[1]; out[2] = v[2] * c - v[0] * sn; }
static int cast_now(u32 item, const float *P, int aimed, float power)
{
    float from[3], to[3];
    if (!red_staff_tip(from)) { from[0] = P[0]; from[1] = P[1] + 4; from[2] = P[2]; }
    if (aimed) red_aim_point(to);
    else {                                           /* straight ahead, level with his staff */
        float yaw = *(float *)(uintptr_t)(item + 0xdc);
        to[0] = from[0] - sinf(yaw) * 60; to[1] = from[1] - 1.0f; to[2] = from[2] - cosf(yaw) * 60;
    }
    static float spd = -1; if (spd < 0) spd = getenv("XBOX_CAST_SPEED") ? atof(getenv("XBOX_CAST_SPEED")) : 1.6f;
    u32 type = *(u32 *)(uintptr_t)0x43cd10u;
    float s = red_scale(), d[3] = { to[0] - from[0], to[1] - from[1], to[2] - from[2] };
    float sp = spd * s * (type == 4 ? 0.7f : type == 2 ? 1.3f : 1.0f);
    int ok = 0;
    g_cast_gen = 0; g_cast_big = 0;
    if (power > 0 && !red_magic_spend(15.0f + 20.0f * power)) return 0;   /* (not enough magic: a fizzle) */
    if (power > 0) {                                  /* charged: three side by side, all at once */
        g_cast_big = 1.6f + 1.6f * power;
        float r[3] = { -d[2], 0, d[0] }, rl = sqrtf(r[0] * r[0] + r[2] * r[2]); if (rl < 1e-3f) rl = 1;
        for (int k = -1; k <= 1; k++) {
            float o = 0.7f * s * k / rl, f2[3] = { from[0] + r[0] * o, from[1] + (k ? 0 : 0.5f * s), from[2] + r[2] * o }, t2[3] = { to[0] + r[0] * o, to[1], to[2] + r[2] * o };
            ok |= red_cast(f2, t2, sp * (1.2f + 0.5f * power), 0, 0);
        }
    } else if (type == 1) {                           /* fire: a spread of three */
        for (int k = -1; k <= 1; k++) {
            float dd[3]; rot_y(d, 0.11f * k, dd);
            float t2[3] = { from[0] + dd[0], from[1] + dd[1], from[2] + dd[2] };
            ok |= red_cast(from, t2, sp, 0, 0);
        }
    } else {
        if (type == 4) g_cast_big = 1.6f;             /* ice: one heavy shard */
        ok = red_cast(from, to, sp, 0, 0);
    }
    g_cast_big = 0;
    if (ok) {
        g_cast_recoil = power > 0 ? 1.6f : 1.0f;
        float fy = aimed ? g_aim_yaw : *(float *)(uintptr_t)(item + 0xdc);
        static float off = 99; if (off == 99) off = getenv("XBOX_FX_YAWOFF") ? atof(getenv("XBOX_FX_YAWOFF")) : 0.0f;
        if (type == 1) red_fx_at(0x040000b7u, from, fy + off);    /* his flame at the staff */
        else if (type == 4) red_fx_at(0x0400007au, from, fy + off);   /* his ice */
        float hz = type == 4 ? 900.0f : type == 2 ? 1400.0f : 420.0f;
        if (power > 0) { sfx_swoosh(hz * 0.5f, 0.35f, 0.8f, 0.0f); sfx_swoosh(90.0f, 0.3f, 0.7f, 0.0f); g_shake = 0.6f; }
        else sfx_swoosh(hz, 0.18f, 0.45f, 0.0f);
    }
    return ok;
}

/* a ring of short, unseen hits around him (ground pound, tail-spin) */
void red_ring_strike(u32 item, int n, float reach, float height, float a0)
{
    const float *P = (const float *)(uintptr_t)(item + 0xc8);
    float s = red_scale(), spd = 1.0f * s;
    for (int i = 0; i < n; i++) {
        float a = a0 + 6.2831853f * i / n;
        float st[3] = { P[0] - sinf(a) * 1.0f * s, P[1] + height * s, P[2] - cosf(a) * 1.0f * s };
        float to[3] = { st[0] - sinf(a) * 10, st[1] - 0.1f * s, st[2] - cosf(a) * 10 };
        red_cast(st, to, spd, reach * s / (spd * 60.0f), 1);
    }
}

/* the staff's hit: three short, unseen shots fanned across the swing, from his chest */
void red_melee_strike(u32 item, int step)
{
    const float *P = (const float *)(uintptr_t)(item + 0xc8);
    float s = red_scale(), yaw = *(float *)(uintptr_t)(item + 0xdc);
    static float reach = -1; if (reach < 0) reach = getenv("XBOX_MELEE_REACH") ? atof(getenv("XBOX_MELEE_REACH")) : 7.5f;
    float from[3] = { P[0], P[1] + 2.4f * s, P[2] };
    static const float fan[3] = { -0.55f, 0.0f, 0.55f };
    for (int i = 0; i < 3; i++) {
        float a = yaw + fan[i] * (step == 2 ? 1.3f : 1.0f), to[3] = { from[0] - sinf(a) * 10, from[1] - 0.15f * s, from[2] - cosf(a) * 10 };
        float st[3] = { from[0] - sinf(a) * 1.2f * s, from[1], from[2] - cosf(a) * 1.2f * s };
        float spd = 1.0f * s;
        extern int g_cast_launch; g_cast_launch = step == 2;           /* the finisher throws them up */
        red_cast(st, to, spd, reach * s / (spd * 60.0f), 1);         /* life: seconds to cover the reach */
        g_cast_launch = 0;
    }
}

int redcast_tick(u32 pl, float dt, int air, int blocked, int x_press, int x_down, float x_held, int classic)
{
    u32 item = *(u32 *)(uintptr_t)(pl + 4);
    const float *P = (const float *)(uintptr_t)(item + 0xc8);
    u32 md = *(u32 *)(uintptr_t)(pl + 0x7ec);
    int busy = md == 29 || md == 17 || md == 67 || md == 3 || md == 4;     /* charging, squashed, talking */
    int can = !blocked && !busy && dm_player_can_change();
    int aim = g_red_aim_in && can;
    float *yaw = (float *)(uintptr_t)(item + 0xdc);
    if (aim && !g_red_aiming) {                     /* start where the camera looks */
        u32 rc = *(u32 *)(uintptr_t)0x4703f0u;
        float A = *yaw;
        if (ok_ptr(rc)) { const float *cd = (const float *)(uintptr_t)(rc + 0x944); if (cd[0] * cd[0] + cd[2] * cd[2] > 1e-4f) A = atan2f(cd[0], cd[2]); }
        g_aim_yaw = A; g_aim_pitch = 0.08f; g_cast_cool = 0.12f;
        sfx_swoosh(260.0f, 0.12f, 0.25f, 0.0f);
    }
    if (!aim && g_red_aiming) g_cam_want_req = g_aim_yaw;          /* the follow camera stays where he aimed */
    g_red_aiming = aim;
    g_aim_w = appr(g_aim_w, aim ? 1.0f : 0.0f, aim ? 9 : 7, dt);
    if (g_aim_w < 0.01f && !aim) g_aim_w = 0;
    static float sens = -1; if (sens < 0) sens = getenv("XBOX_AIM_SENS") ? atof(getenv("XBOX_AIM_SENS")) : 1.0f;
    if (aim) {
        g_aim_yaw = wrapf(g_aim_yaw + g_aim_rx * fabsf(g_aim_rx) * 3.2f * sens * dt);
        g_aim_pitch += g_aim_ry * fabsf(g_aim_ry) * 2.0f * sens * dt;
        if (g_aim_pitch < -0.65f) g_aim_pitch = -0.65f;
        if (g_aim_pitch > 0.9f) g_aim_pitch = 0.9f;
        float d = wrapf(g_aim_yaw - *yaw);
        *yaw = wrapf(*yaw + d * (dt * 22 > 1 ? 1 : dt * 22)); *(u8 *)(uintptr_t)(item + 0x7e) = 0;
        g_cam_want_req = g_aim_yaw;
        /* walking while aiming: the left stick moves him without turning him (the game's own
         * movement would turn him to the stick); the collision sweep sees it as his movement */
        float sx = g_aim_lx, sy = g_aim_ly, mag = sqrtf(sx * sx + sy * sy);
        if (mag > 0.2f) {
            if (mag > 1) { sx /= mag; sy /= mag; }
            float A = g_aim_yaw, f[2] = { -sinf(A), -cosf(A) }, r[2] = { -cosf(A), sinf(A) };
            float sp = 11.0f * red_scale() * (air ? 0.8f : 1.0f) * dt / (g_move_scale * g_move_xz > 0.05f ? g_move_scale * g_move_xz : 1);
            float *wp = (float *)(uintptr_t)(item + 0xc8);
            wp[0] += (f[0] * sy + r[0] * sx) * sp; wp[2] += (f[1] * sy + r[1] * sx) * sp;
            *(u8 *)(uintptr_t)(item + 0x7e) = 0;
        }
    }
    aim_view(P, g_aim_eye, g_aim_dir);
    /* casting: aimed (square while aiming), or square held a moment without aiming. While aiming
     * a tap casts at once; held on past that, the staff charges (a second) and letting go casts
     * the charged spell */
    g_cast_cool -= dt;
    int want = 0, aimed = 0;
    static float rate = -1; if (rate < 0) rate = getenv("XBOX_CAST_RATE") ? atof(getenv("XBOX_CAST_RATE")) : 0.26f;
    if (aim) {
        if (x_press && g_cast_cool <= 0) { want = 1; aimed = 1; g_charge = 0; }
        if (x_down && x_held > 0.32f) {
            float c0 = g_charge; g_charge += dt / 1.0f; if (g_charge > 1) g_charge = 1;
            if (c0 < 1 && g_charge >= 1) sfx_swoosh(1800.0f, 0.15f, 0.4f, 0.0f);     /* full */
            static RS float hum; hum += dt; if (hum > 0.12f) { hum = 0; sfx_swoosh(200.0f + 500.0f * g_charge, 0.12f, 0.15f + 0.2f * g_charge, 0.0f); }
        }
        if (!x_down && g_charge > 0) {
            if (g_charge > 0.2f && cast_now(item, P, 1, g_charge)) g_cast_cool = rate * 1.5f;
            g_charge = 0;
        }
    } else {
        g_charge = 0;
        if (!classic && can && x_down && x_held > 0.3f) want = 1;
    }
    g_red_casting = want || g_charge > 0 || (g_red_casting && g_cast_cool > -0.25f);
    if (want && g_cast_cool <= 0) {
        if (cast_now(item, P, aimed, 0)) g_cast_cool = rate;
        else g_cast_cool = 0.1f;
    }
    /* electric forks (queued by a bolt's hit) */
    for (int i = 0; i < g_nforks; i++) {
        float to[3] = { g_forks[i].pos[0] + g_forks[i].dir[0] * 20, g_forks[i].pos[1] + g_forks[i].dir[1] * 20, g_forks[i].pos[2] + g_forks[i].dir[2] * 20 };
        g_cast_gen = g_forks[i].gen;
        red_cast(g_forks[i].pos, to, 1.6f * red_scale(), 0.3f, 0);
        g_cast_gen = 0;
    }
    if (g_nforks) sfx_swoosh(2200.0f, 0.12f, 0.35f, 0.0f);
    g_nforks = 0;
    /* aiming in the air: he hangs on his wings (sinking slowly, for a few seconds) to cast from above */
    {   static RS float hover;
        if (air && !g_red_was_air) hover = 0;
        if (aim && air) {
            u32 ph = *(u32 *)(uintptr_t)(item + 0x148);
            hover += dt;
            if (ok_ptr(ph) && hover < 3.0f) { float *vy = (float *)(uintptr_t)(ph + 0x7c); if (*vy < -2.0f) *vy = -2.0f; }
            g_red_hovering = hover < 3.0f;
        } else g_red_hovering = 0;
        g_red_was_air = air;
    }
    if (g_charge > 0) g_cast_recoil = appr(g_cast_recoil, -0.7f * g_charge, 10, dt);   /* the staff drawn back */
    else
    g_cast_recoil = appr(g_cast_recoil, 0, 9, dt);
    g_shake = appr(g_shake, 0, 12, dt);
    return aim || (!classic && x_down && x_held > 0.3f);
}

/* the camera hook (debugmenu.c size_camera): the aim view, blended in over the follow camera */
RS int g_cam_steady;                        /* staff combo / spin / pound: the camera is eased, not jerked */
void redcast_camera(u32 cam, float *eye, float *tgt)
{
    u32 pl = *(u32 *)(uintptr_t)PLAYER_OBJ; if (!ok_ptr(pl)) return;
    u32 item = *(u32 *)(uintptr_t)(pl + 4); if (!ok_ptr(item)) return;
    const float *P = (const float *)(uintptr_t)(item + 0xc8);
    float s = red_scale(); if (s < 0.5f) s = 0.5f;
    float tp = (tgt[0] - P[0]) * (tgt[0] - P[0]) + (tgt[2] - P[2]) * (tgt[2] - P[2]);
    {   /* research: XBOX_TESTCAM=dist,height,angle - a fixed view of him (angle from his front, radians) */
        static int tc = -1, tcf; static float cd = 12, chh = 3, ca = 1.5708f;
        if (tc < 0) { const char *e = getenv("XBOX_TESTCAM"); tc = e ? 1 : 0; if (e) sscanf(e, "%f,%f,%f,%d", &cd, &chh, &ca, &tcf); }
        if (tc && getenv("XBOX_TESTCAM_FROM")) { extern u32 debugmenu_play_frame(void); if (debugmenu_play_frame() < (u32)atoi(getenv("XBOX_TESTCAM_FROM"))) goto no_testcam; }
        if (tc) {
            static RS float yw0 = 99; if (yw0 == 99) yw0 = *(float *)(uintptr_t)(item + 0xdc);   /* (his yaw when it starts: the spin doesn't drag it) */
            if (tcf) yw0 = *(float *)(uintptr_t)(item + 0xdc);                              /* 4th value 1: turns with him (side-on tracking) */
            float a = yw0 + ca;
            eye[0] = P[0] - sinf(a) * cd * s; eye[1] = P[1] + chh * s; eye[2] = P[2] - cosf(a) * cd * s;
            tgt[0] = P[0]; tgt[1] = P[1] + 2.0f * s; tgt[2] = P[2];
            return;
        }
    }
no_testcam:
    if (tp > 9.0f * s * 9.0f * s) return;                               /* not a camera on him */
    {   /* steadying: the view's offset from him is eased while he fights (and for a moment after),
         * so the game's camera switching / pushing during swings doesn't jerk the picture */
        static RS struct { u32 cam; float e[3], t[3]; double lt; float w; } st[4]; int i;
        for (i = 0; i < 4 && st[i].cam != cam; i++) ;
        if (i == 4) { static RS int rr; i = rr++ & 3; st[i].cam = cam; st[i].lt = 0; }
        double now = now_s(); float dt = st[i].lt ? (float)(now - st[i].lt) : 1; st[i].lt = now;
        float oe[3] = { eye[0] - P[0], eye[1] - P[1], eye[2] - P[2] }, ot[3] = { tgt[0] - P[0], tgt[1] - P[1], tgt[2] - P[2] };
        if (dt > 0.3f) { memcpy(st[i].e, oe, 12); memcpy(st[i].t, ot, 12); st[i].w = 0; }
        if (dt > 1e-3f) {
            st[i].w = appr(st[i].w, g_cam_steady ? 1.0f : 0.0f, g_cam_steady ? 20 : 2.5f, dt);
            float k = 30.0f - 25.0f * st[i].w;            /* 30/s (follow) .. 5/s (steady) */
            for (int q = 0; q < 3; q++) { st[i].e[q] = appr(st[i].e[q], oe[q], k, dt); st[i].t[q] = appr(st[i].t[q], ot[q], k, dt); }
        }
        if (st[i].w > 0.01f) for (int q = 0; q < 3; q++) { eye[q] = P[q] + st[i].e[q]; tgt[q] = P[q] + st[i].t[q]; }
    }
    if (g_shake > 0.01f) {
        float a = g_shake * 0.35f * s; static u32 seed = 1; seed = seed * 1103515245u + 12345u;
        float j = ((seed >> 16) & 0xff) / 127.5f - 1, k = ((seed >> 8) & 0xff) / 127.5f - 1;
        eye[0] += j * a; eye[1] += k * a; tgt[0] += j * a * 0.5f; tgt[1] += k * a * 0.5f;
    }
    if (!g_redchar_anim || g_aim_w <= 0) return;
    /* no ray casts here: the game's own camera keeps clear of walls, so how close it has had to
     * come in to him is how much room there is behind him - the aim view comes in as far */
    {   float gd = sqrtf((eye[0] - P[0]) * (eye[0] - P[0]) + (eye[2] - P[2]) * (eye[2] - P[2]));
        float room = gd / (AIM_BACK * s); if (room > 1) room = 1; if (room < 0.35f) room = 0.35f;
        static RS double lt; double t = now_s(); float dt = lt ? (float)(t - lt) : 0; lt = t; if (dt > 0.1f) dt = 0.1f;
        if (dt > 1e-3f) g_aim_room = room < g_aim_room ? appr(g_aim_room, room, 20, dt) : appr(g_aim_room, room, 3, dt); }
    float e[3], d[3]; aim_view(P, e, d);
    float w = g_aim_w * g_aim_w * (3 - 2 * g_aim_w);
    for (int k = 0; k < 3; k++) {
        float t = e[k] + d[k] * 30.0f * s;
        eye[k] += (e[k] - eye[k]) * w; tgt[k] += (t - tgt[k]) * w;
    }
}

/* ---- drawn over the frame: the reticle while aiming, and a glow riding each of his shots ---- */
void ov_begin(int *w, int *h); void ov_rect(float x, float y, float w, float h, u32 rgba); void ov_glow(float x, float y, float r, u32 c); void ov_end(void);
extern int g_pres_rect[4];
static u32 elem_col(u32 type) { return type == 4 ? 0x60d8ff : type == 2 ? 0xfff070 : type == 8 ? 0x4090ff : 0xff7a20; }
static int to_screen(const float *VP, const float p[3], float *sx, float *sy, float *wv, int w, int h)
{
    float cx = p[0] * VP[0] + p[1] * VP[4] + p[2] * VP[8] + VP[12], cy = p[0] * VP[1] + p[1] * VP[5] + p[2] * VP[9] + VP[13];
    float cw = p[0] * VP[3] + p[1] * VP[7] + p[2] * VP[11] + VP[15];
    if (cw < 0.1f) return 0;
    float pw = g_pres_rect[2] ? g_pres_rect[2] : w, ph = g_pres_rect[3] ? g_pres_rect[3] : h;
    *sx = g_pres_rect[0] + (cx / cw * 0.5f + 0.5f) * pw; *sy = g_pres_rect[1] + (0.5f - cy / cw * 0.5f) * ph; *wv = cw;
    return 1;
}
void redcast_draw(void)
{
    if (!g_redchar_anim) return;
    u32 rc = *(u32 *)(uintptr_t)0x4703f0u; if (!ok_ptr(rc)) return;
    const float *VP = (const float *)(uintptr_t)(rc + 0x8f4);
    double now = now_s();
    int any = 1;                                       /* (the magic meter decides for itself) */
    for (int i = 0; i < 24 && !any; i++) if (g_rp[i].h && !g_rp[i].melee && !g_rp[i].hit && now - g_rp[i].seen < 0.1) any = 1;
    if (!any) return;
    int w, h; ov_begin(&w, &h);
    magic_draw(w, h, now);
    float ph = g_pres_rect[3] ? g_pres_rect[3] : h;
    for (int i = 0; i < 24; i++) {                    /* his shots: a bright core in his colours */
        rproj *r = &g_rp[i];
        if (!r->h || r->melee || r->hit || now - r->seen > 0.1) continue;
        float sx, sy, cw; if (!to_screen(VP, r->pos, &sx, &sy, &cw, w, h)) continue;
        float rad = ph * 1.6f / (cw > 1 ? cw : 1); if (rad > ph * 0.05f) rad = ph * 0.05f; if (rad < 3) rad = 3;
        u32 c = elem_col(r->type);
        if (r->big > 1) rad *= 1.0f + 0.25f * r->big;
        ov_glow(sx, sy, rad * 2.0f, 0x70000000u | c);
        ov_glow(sx, sy, rad * 0.8f, 0xe0000000u | 0xfff4e0);
    }
    if (g_aim_w > 0.4f) {                              /* the reticle */
        float p[3], sx, sy, cw; red_aim_point(p);
        if (to_screen(VP, p, &sx, &sy, &cw, w, h)) {
            u32 type = *(u32 *)(uintptr_t)0x43cd10u, c = elem_col(type);
            u32 a = (u32)(230 * (g_aim_w - 0.4f) / 0.6f) << 24;
            float g = ph * 0.016f + ph * 0.022f * (g_cast_cool > 0 ? g_cast_cool / 0.26f : 0), L = ph * 0.026f, t = ph * 0.005f + 1.5f, o = 1.5f;
            for (int pass = 0; pass < 2; pass++) {       /* a dark outline, then the colour */
                float e = pass ? 0 : o; u32 col = pass ? (a | c) : ((a >> 1) & 0xff000000u);
                ov_rect(sx - g - L - e, sy - t / 2 - e, L + 2 * e, t + 2 * e, col); ov_rect(sx + g - e, sy - t / 2 - e, L + 2 * e, t + 2 * e, col);
                ov_rect(sx - t / 2 - e, sy - g - L - e, t + 2 * e, L + 2 * e, col); ov_rect(sx - t / 2 - e, sy + g - e, t + 2 * e, L + 2 * e, col);
            }
            ov_glow(sx, sy, t * 1.6f, a | 0xffffff);
            if (g_charge > 0) {                       /* charging: the reticle swells and brightens */
                float pulse = 0.85f + 0.15f * sinf((float)now * (10 + 20 * g_charge));
                ov_glow(sx, sy, ph * (0.02f + 0.035f * g_charge) * pulse, ((u32)(60 + 150 * g_charge) << 24) | c);
            }
        }
    }
    ov_end();
}

/* ---- his magic meter: the charged casts, ground pound, tail-spin and fly-by fire draw on it; it
 * refills by itself a moment after the last use. Drawn bottom-left as a crystal gauge. ---- */
RS float g_magic = 100.0f;
static RS double g_magic_used = -10, g_magic_fizzle = -10;
void sfx_swoosh(float hz, float dur, float vol, float pan);
int red_magic_spend(float amt)
{
    if (getenv("XBOX_MAGIC_INF")) return 1;
    if (g_magic < amt) {
        double t = now_s(); if (t - g_magic_fizzle > 0.4) { g_magic_fizzle = t; sfx_swoosh(160.0f, 0.12f, 0.35f, 0.0f); }
        return 0;
    }
    g_magic -= amt; g_magic_used = now_s();
    return 1;
}
void red_magic_tick(float dt)
{
    if (now_s() - g_magic_used > 1.0) { g_magic += 16.0f * dt; if (g_magic > 100) g_magic = 100; }
}
static void magic_draw(int w, int h, double now)
{
    float ph = g_pres_rect[3] ? g_pres_rect[3] : h, pw = g_pres_rect[2] ? g_pres_rect[2] : w;
    float x0 = g_pres_rect[0] + pw * 0.035f, y1 = g_pres_rect[1] + ph * 0.93f;
    float bw = ph * 0.022f, bh = ph * 0.2f;
    float a = 1.0f;                                   /* full and unused for a while: fades away */
    double idle = now - g_magic_used; if (g_magic >= 100 && idle > 3) a = idle > 4 ? 0 : (float)(4 - idle);
    if (now - g_magic_fizzle < 0.5) a = 1;
    if (a <= 0.01f) return;
    u32 A = (u32)(a * 255) << 24, Ah = (u32)(a * 140) << 24;
    u32 type = *(u32 *)(uintptr_t)0x43cd10u, c = elem_col(type);
    int low = now - g_magic_fizzle < 0.5 && ((int)(now * 12) & 1);
    ov_rect(x0 - 2, y1 - bh - 2, bw + 4, bh + 4, Ah);                         /* the frame */
    float f = g_magic / 100.0f;
    ov_rect(x0, y1 - bh * f, bw, bh * f, A | (low ? 0xff3030 : c));             /* the charge in it */
    ov_glow(x0 + bw / 2, y1 - bh * f, bw * 1.3f, ((u32)(a * 200) << 24) | 0xffffff);   /* its glowing top */
    ov_glow(x0 + bw / 2, y1 - bh - bw * 0.9f, bw * 1.1f, ((u32)(a * 230) << 24) | 0xb070ff);   /* the crystal */
}

/* ---- fly-by fire: square held while flying pours the breath down ahead of him ---- */
int red_flyby_tick(u32 item, float dt, int x_down)
{
    static RS float t, fx_t;
    if (!x_down) { t = 0; return 0; }
    t -= dt; fx_t -= dt;
    if (t > 0) return 1;
    if (!red_magic_spend(18.0f * 0.07f)) { t = 0.2f; return 1; }
    t = 0.07f;
    const float *P = (const float *)(uintptr_t)(item + 0xc8);
    float from[3]; if (!red_staff_tip(from)) { from[0] = P[0]; from[1] = P[1] + 3; from[2] = P[2]; }
    float yaw = *(float *)(uintptr_t)(item + 0xdc), s = red_scale();
    float j = ((rand() % 100) / 100.0f - 0.5f) * 0.25f;
    float d[3] = { -sinf(yaw + j) * 0.6f, -0.8f, -cosf(yaw + j) * 0.6f };
    float to[3] = { from[0] + d[0] * 20, from[1] + d[1] * 20, from[2] + d[2] * 20 };
    g_cast_gen = 1;                                   /* (no forks; quiet if it finds nothing) */
    red_cast(from, to, 1.5f * s, 0.7f, 0);
    g_cast_gen = 0;
    if (fx_t <= 0) {
        fx_t = 0.18f;
        u32 type = *(u32 *)(uintptr_t)0x43cd10u;
        if (type == 1) red_fx_at(0x040000b7u, from, yaw); else if (type == 4) red_fx_at(0x0400007au, from, yaw);
        sfx_swoosh(type == 4 ? 800.0f : 350.0f, 0.2f, 0.35f, 0.0f);
    }
    return 1;
}

/* ---- the finisher's launch: the projectile class's "hit an item" (vtable +0x1e8, thiscall
 * proj, item, info, 0 -> hit) is wrapped like its update; a staff finisher shot that hits a
 * character throws it up into the air ---- */
typedef int (__attribute__((thiscall)) *hit_fn)(void *self, u32 item, void *info, u32 z);
static struct { u32 vt; hit_fn orig; } g_hvts[6]; static int g_nhvt;
static struct { u32 item; float t, vy; } g_launch[8];
int g_cast_launch;
static int __attribute__((thiscall)) proj_hit(void *self, u32 item, void *info, u32 z)
{
    u32 vt = *(u32 *)(uintptr_t)self; hit_fn orig = NULL;
    for (int i = 0; i < g_nhvt; i++) if (g_hvts[i].vt == vt) orig = g_hvts[i].orig;
    if (!orig) return 0;
    int r = orig(self, item, info, z);
    rproj *p = rp_find((u32)(uintptr_t)self);
    if (getenv("XBOX_PROJ_LOG")) klog("redcast: hit test %08x item %08x -> %d (ours %d melee %d)\n", (u32)(uintptr_t)self, item, r & 0xff, p != NULL, p ? p->melee : -1);
    if ((r & 0xff) && p && p->melee == 2 && ok_ptr(item)) {
        int k = 0; for (int i = 0; i < 8; i++) { if (g_launch[i].item == item) { k = -1; break; } if (g_launch[i].t <= 0) k = i; }
        if (k >= 0) { g_launch[k].item = item; g_launch[k].t = 0.7f; g_launch[k].vy = 26.0f; klog("redcast: launched %08x\n", item); }
    }
    return r;
}
void wrap_hit_vt(u32 vt)
{
    for (int i = 0; i < g_nhvt; i++) if (g_hvts[i].vt == vt) return;
    if (g_nhvt == 6) return;
    u32 *slot = (u32 *)(uintptr_t)(vt + 0x1e8);
    if (*slot < 0x10000 || *slot > 0x2f0000) return;
    g_hvts[g_nhvt].vt = vt; g_hvts[g_nhvt].orig = (hit_fn)(uintptr_t)*slot; g_nhvt++;
    klog("redcast: hit wrapped on vtable %08x\n", vt);
    *slot = (u32)(uintptr_t)proj_hit;
}
void red_launch_tick(float dt)
{
    { u32 gl_frame_number(void); static u32 lastf; u32 f = gl_frame_number(); if (f == lastf) return; lastf = f; }   /* (once a frame: both players may be Red) */
    for (int i = 0; i < 8; i++) {
        if (g_launch[i].t <= 0) continue;
        u32 it = g_launch[i].item;
        g_launch[i].t -= dt;
        if (!ok_ptr(it) || !(*(u8 *)(uintptr_t)(it + 0x10) & 1)) { g_launch[i].t = 0; continue; }
        float *p = (float *)(uintptr_t)(it + 0xc8);
        u32 ph = *(u32 *)(uintptr_t)(it + 0x148);
        int phys_ok = ok_ptr(ph) && *(u32 *)(uintptr_t)(ph + 4) == it;
        if (phys_ok && g_launch[i].vy > 25.0f) *(float *)(uintptr_t)(ph + 0x7c) = 22.0f;   /* its own physics, if it has a body */
        p[1] += g_launch[i].vy * dt; g_launch[i].vy -= 60.0f * dt;                        /* and a hop up we drive */
        if (g_launch[i].vy < 0) g_launch[i].t = 0;
        if (phys_ok) memcpy((void *)(uintptr_t)(ph + 0x140), p, 12);
        *(u8 *)(uintptr_t)(it + 0x7e) = 0;
    }
}
