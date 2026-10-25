/* Red's shrink bolt (Y / triangle): research + spell. */
#include "xbox.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
static unsigned SDL_GetTicks_shim(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (unsigned)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000); }

#define PLAYER_OBJ 0x44c704u
extern int kmem_committed(u32 a);
u32 debugmenu_play_frame(void);
static int okp(u32 p) { if (p >= 0x10000 && p < 0x4c0000) return 1; return p >= 0x10000 && p < 0x80000000u && kmem_committed(p & ~0xfffu) && kmem_committed((p + 0x400) & ~0xfffu); }
static u32 rd(u32 a) { return *(u32 *)(uintptr_t)a; }

/* class descriptor through vtable[0] = "mov eax, desc; ret"; desc +0 parent, +4 name */
static u32 class_desc(u32 obj)
{
    if (!okp(obj)) return 0;
    u32 vt = rd(obj); if (vt < 0x2e0000 || vt > 0x360000) return 0;
    u32 f = rd(vt); if (f < 0x10000 || f > 0x2e0000) return 0;
    const u8 *c = (const u8 *)(uintptr_t)f;
    if (c[0] != 0xb8 || c[5] != 0xc3) return 0;
    return *(u32 *)(c + 1);
}
static const char *class_name(u32 obj)
{
    u32 d = class_desc(obj); if (!d || !okp(rd(d + 4))) return "?";
    return (const char *)(uintptr_t)rd(d + 4);
}
static void class_chain(u32 obj, char *out, int n)
{
    out[0] = 0; u32 d = class_desc(obj);
    for (int i = 0; d && okp(d) && i < 6; i++) {
        u32 nm = rd(d + 4); if (!okp(nm)) break;
        int l = strlen(out); snprintf(out + l, n - l, "%s%s", i ? "<" : "", (const char *)(uintptr_t)nm);
        d = rd(d); if (d < 0x2e0000 || d > 0x360000) break;
    }
}


/* ---- what counts as an enemy: a handler (item +0x144, whose +4 is the item) descending from
 * EnemyBase (Gnorc-free enemies, fodder), XSEItemHandler_Grunt (Gnorcs, crabs, critters...) or
 * ChaseEnemyBase (egg and gem thieves); not the bosses ---- */
#define DESC_ENEMYBASE 0x2f2fa4u
#define DESC_GRUNT     0x30f88cu
#define DESC_CHASE     0x32b1acu
static int is_enemy(u32 it)
{
    u32 h = rd(it + 0x144); if (!okp(h) || rd(h + 4) != it) return 0;
    u32 d = class_desc(h);
    for (int i = 0; d && i < 8; i++) {
        if (d == DESC_ENEMYBASE || d == DESC_GRUNT || d == DESC_CHASE) return 1;
        d = rd(d); if (d < 0x2e0000 || d > 0x360000) break;
    }
    return 0;
}

/* the items the game is running now (its active list), refreshed each frame */
static u32 g_items[512]; static int g_nitems;
static void list_items(void)
{
    g_nitems = 0;
    u32 head = rd(0x446608u), n = okp(head) ? rd(head) : 0;
    for (int guard = 0; okp(n) && guard < 4000 && g_nitems < 512; guard++, n = rd(n + 4)) {
        u32 it = rd(n + 8); if (okp(it)) g_items[g_nitems++] = it;
    }
}
static int item_live(u32 it) { for (int i = 0; i < g_nitems; i++) if (g_items[i] == it) return *(u8 *)(uintptr_t)(it + 0x10) & 1; return 0; }

/* ---- shrunk enemies ---- */
typedef struct { u32 item; int state; float k, t, flat; float base[3], wrote[3]; int hits; } shrunk;
#define NSHRUNK 12
static shrunk g_sh[NSHRUNK];
static const float SHRINK_TO = 0.3f, SHRINK_HOLD = 10.0f;
static shrunk *sh_find(u32 it) { for (int i = 0; i < NSHRUNK; i++) if (g_sh[i].item == it) return &g_sh[i]; return NULL; }
static void sh_write(shrunk *e)
{
    for (int k = 0; k < 3; k++) {
        float *f = (float *)(uintptr_t)(e->item + 0xe8 + 4 * k);
        if (*f != e->wrote[k]) e->base[k] = *f;               /* the game set it: that's its own size */
        float m = e->k * (k == 1 ? e->flat : 1.0f + (1.0f - e->flat) * 0.6f);
        e->wrote[k] = e->base[k] * m; *f = e->wrote[k];
    }
    *(u8 *)(uintptr_t)(e->item + 0x7e) = 0;
}
static void sh_release(shrunk *e)
{
    if (e->item && item_live(e->item)) for (int k = 0; k < 3; k++) {
        float *f = (float *)(uintptr_t)(e->item + 0xe8 + 4 * k); if (*f == e->wrote[k]) *f = e->base[k]; }
    memset(e, 0, sizeof *e);
}
static void sh_start(u32 it)
{
    shrunk *e = sh_find(it);
    if (!e) { for (int i = 0; i < NSHRUNK && !e; i++) if (!g_sh[i].item) e = &g_sh[i]; }
    if (!e) return;
    if (e->item != it) { memset(e, 0, sizeof *e); e->item = it; e->k = 1; e->flat = 1;
        for (int k = 0; k < 3; k++) e->base[k] = e->wrote[k] = *(float *)(uintptr_t)(it + 0xe8 + 4 * k); }
    e->state = 1; e->t = 0;
}

/* ---- the bolt: his own spell, a green spark that flies straight from the staff ---- */
typedef struct { int on; u32 target; float pos[3], dir[3], life, trail[8][3]; int nt; } sbolt;
static sbolt g_bolt[4];
static struct { float pos[3]; float t; } g_burst[6];       /* where a bolt struck (a flash) */
static void burst(const float p[3]) { int k = 0; for (int i = 1; i < 6; i++) if (g_burst[i].t < g_burst[k].t) k = i; memcpy(g_burst[k].pos, p, 12); g_burst[k].t = 0.45f; }

extern u32 g_redchar_anim;
extern float g_size_cur;
void sfx_swoosh(float hz, float dur, float vol, float pan);
void red_sound_at(u32 id, u32 item, float vol, float pan);
int red_magic_spend(float amt);
int red_staff_tip(float out[3]);
void red_aim_point(float out[3]);
int red_cast(const float from[3], const float to[3], float spd, float life, int melee);
static float rscale(void) { return g_size_cur > 0.2f ? g_size_cur : 1.0f; }
static void centre(u32 it, float c[3])
{
    const float *Q = (const float *)(uintptr_t)(it + 0xc8);
    float sy = *(float *)(uintptr_t)(it + 0xec); if (!(sy > 0.05f && sy < 20)) sy = 1;
    c[0] = Q[0]; c[1] = Q[1] + 1.2f * sy; c[2] = Q[2];
}

RS float g_shrink_pose;                  /* > 0: he thrusts the staff (the cast pose, debugmenu.c) */
int g_shrink_casts, g_shrink_hits, g_shrink_kills;   /* (tests) */
static void cast_bolt(u32 item, int aimed)
{
    const float *P = (const float *)(uintptr_t)(item + 0xc8);
    float from[3], to[3], s = rscale(); u32 tgt = 0;
    if (!red_staff_tip(from)) { from[0] = P[0]; from[1] = P[1] + 3.5f * s; from[2] = P[2]; }
    float yaw = *(float *)(uintptr_t)(item + 0xdc), fx = -sinf(yaw), fz = -cosf(yaw);
    if (aimed) red_aim_point(to);
    else {      /* not aiming: it bends toward the enemy most nearly ahead of him (within ~45 degrees) */
        to[0] = from[0] + fx * 50; to[1] = from[1] - 1.5f * s; to[2] = from[2] + fz * 50;
        float best = -9; tgt = 0;
        for (int i = 0; i < g_nitems; i++) {
            u32 it = g_items[i]; if (it == item || !(*(u8 *)(uintptr_t)(it + 0x10) & 1) || !is_enemy(it)) continue;
            shrunk *e = sh_find(it); if (e && e->state) continue;
            float c[3]; centre(it, c);
            float d[3] = { c[0] - P[0], c[1] - P[1], c[2] - P[2] }, l = sqrtf(d[0] * d[0] + d[2] * d[2]);
            if (l < 0.5f || l > 45 * s || fabsf(d[1]) > 12 * s) continue;
            float dot = (d[0] * fx + d[2] * fz) / l, score = dot - l / (150 * s);   /* (nearer ones first) */
            if (dot > 0.72f && score > best) { best = score; memcpy(to, c, 12); tgt = it; }
        }
    }
    sbolt *b = &g_bolt[0]; for (int i = 0; i < 4; i++) if (!g_bolt[i].on) { b = &g_bolt[i]; break; }
    float d[3] = { to[0] - from[0], to[1] - from[1], to[2] - from[2] }, l = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (l < 1e-3f) { d[0] = fx; d[1] = 0; d[2] = fz; l = 1; }
    memset(b, 0, sizeof *b); b->on = 1; b->life = 1.2f; b->target = tgt;
    for (int k = 0; k < 3; k++) { b->pos[k] = from[k]; b->dir[k] = d[k] / l; }
    extern float g_cast_recoil; g_cast_recoil = 1.3f; g_shrink_pose = 0.45f;
    sfx_swoosh(1600.0f, 0.22f, 0.5f, 0.0f); sfx_swoosh(700.0f, 0.3f, 0.35f, 0.0f);
    if (rand() % 3 == 0) red_sound_at(0x233, item, 0.7f, 0.0f);
    g_shrink_casts++;
    klog("shrink: bolt from %.1f %.1f %.1f (feet %.1f %.1f %.1f) dir %.2f %.2f %.2f%s target %08x\n", from[0], from[1], from[2], P[0], P[1], P[2], b->dir[0], b->dir[1], b->dir[2], aimed ? " (aimed)" : "", tgt);
}

/* finishing a squashed one: its handler's own "hit" (vtable slot 49, +0xc4: thiscall handler,
 * info) with a hit that hurts it. info +0x18 = the push (x, y, z), +0x40 = the kind of hit: Gnorcs
 * and the other grunts take only the kinds in their mask (+0x3d4: one "who" bit in 0xf000 plus a
 * "what" bit) and have health at +0x59c (one left, so this hit is the last); fodder (sheep and
 * such) go down to any hit */
static u8 g_hitinfo[0x100] __attribute__((aligned(16)));
static int has_desc(u32 h, u32 want) { u32 d = class_desc(h); for (int i = 0; d && i < 8; i++) { if (d == want) return 1; d = rd(d); if (d < 0x2e0000 || d > 0x360000) break; } return 0; }
static int finish(u32 it, u32 me)
{
    u32 h = rd(it + 0x144); if (!okp(h) || rd(h + 4) != it) return 0;
    u32 vt = rd(h); if (vt < 0x2e0000 || vt > 0x360000) return 0;
    u32 fn = rd(vt + 0xc4); if (fn < 0x10000 || fn > 0x2e0000 || fn == 0x24de0u) { klog("shrink: %08x has no hit handler\n", it); return 0; }
    u32 flags = 0x1011;
    if (has_desc(h, DESC_GRUNT)) {
        u32 mask = rd(h + 0x3d4), who = mask & 0xf000, what = mask & 0x11 ? mask & 0x11 : mask & 0x8ff;
        if (!who || !what) { klog("shrink: %08x can't be hurt (mask %08x)\n", it, mask); return 0; }
        flags = (who & -who) | (what & -what);
        float *hp = (float *)(uintptr_t)(h + 0x59c); if (*hp > 1.0f && *hp < 1000.0f) *hp = 1.0f;
        *(u32 *)(uintptr_t)(h + 0x3a8) = 0;                       /* (no "just hit" grace) */
    }
    memset(g_hitinfo, 0, sizeof g_hitinfo);
    float *d = (float *)(g_hitinfo + 0x18);
    if (me) { const float *P = (const float *)(uintptr_t)(me + 0xc8), *Q = (const float *)(uintptr_t)(it + 0xc8);
        float dx = Q[0] - P[0], dz = Q[2] - P[2], l = sqrtf(dx * dx + dz * dz); if (l > 1e-3f) { d[0] = dx / l; d[2] = dz / l; } else d[2] = 1; }
    d[1] = -1.0f;
    *(u32 *)(g_hitinfo + 0x40) = flags;
    int r = ((int (__attribute__((thiscall)) *)(void *, void *))(uintptr_t)fn)((void *)(uintptr_t)h, g_hitinfo);
    klog("shrink: finished %08x (hit %08x flags %x -> %d)\n", it, fn, flags, r);
    return 1;
}

/* Red's part (debugmenu.c's Red tick): Y / triangle casts */
static RS int g_y_prev;
void redshrink_red_tick(u32 pl, float dt, int blocked)
{
    extern int g_raw_y_pub, g_red_aiming;
    u32 item = rd(pl + 4); if (!okp(item)) return;
    u32 md = rd(pl + 0x7ec);
    int busy = md == 29 || md == 17 || md == 67 || md == 3 || md == 4;
    extern int dm_player_can_change(void);
    static RS float cool; cool -= dt;
    if (g_shrink_pose > 0) g_shrink_pose -= dt;           /* (his pose: each player's own) */
    int y = g_raw_y_pub;
    {   const char *ev = getenv("XBOX_SHRINK_AT"); u32 pf = debugmenu_play_frame();      /* tests: +frame,.. */
        if (ev && pf) { char buf[256]; snprintf(buf, sizeof buf, "%s", ev); char *sv;
            for (char *t = strtok_r(buf, ",", &sv); t; t = strtok_r(NULL, ",", &sv)) if ((u32)strtoul(t + (t[0] == '+'), NULL, 10) == pf) { y = 1; g_y_prev = 0; cool = 0; } } }
    if (y && !g_y_prev && !blocked && !busy && cool <= 0 && dm_player_can_change()) {
        if (red_magic_spend(25.0f)) { cast_bolt(item, g_red_aiming); cool = 0.5f; }
        else cool = 0.3f;
    }
    g_y_prev = y;
}

/* the enemy nearest him is a shrunk one (and no full-size one is near): its hits don't count */
int redshrink_only_tiny_near(u32 me)
{
    int any = 0; for (int i = 0; i < NSHRUNK; i++) any |= g_sh[i].item != 0; if (!any || !okp(me)) return 0;
    list_items();
    const float *P = (const float *)(uintptr_t)(me + 0xc8); float s = rscale(), reach = 9.0f * s;
    int tiny = 0;
    for (int i = 0; i < g_nitems; i++) {
        u32 it = g_items[i]; if (it == me || !(*(u8 *)(uintptr_t)(it + 0x10) & 1) || !is_enemy(it)) continue;
        const float *Q = (const float *)(uintptr_t)(it + 0xc8);
        float d2 = (Q[0] - P[0]) * (Q[0] - P[0]) + (Q[1] - P[1]) * (Q[1] - P[1]) + (Q[2] - P[2]) * (Q[2] - P[2]);
        if (d2 > reach * reach) continue;
        shrunk *e = sh_find(it);
        if (e && (e->state == 1 || e->state == 3) && e->k < 0.6f) tiny = 1; else return 0;
    }
    return tiny;
}

/* everyone's part (every frame): bolts in flight, shrunk enemies, stomps */
void redshrink_tick(void)
{
    static double lt; struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    double now = ts.tv_sec + ts.tv_nsec * 1e-9; float dt = lt ? (float)(now - lt) : 1.0f / 60; lt = now;
    if (dt > 0.1f) dt = 0.1f;
    for (int i = 0; i < 6; i++) if (g_burst[i].t > 0) g_burst[i].t -= dt;
    int any = 0; for (int i = 0; i < NSHRUNK; i++) any |= g_sh[i].item != 0;
    for (int i = 0; i < 4; i++) any |= g_bolt[i].on;
    if (!any) return;
    list_items();
    u32 pl = rd(PLAYER_OBJ), me = okp(pl) ? rd(pl + 4) : 0;
    if (!okp(me)) me = 0;
    float s = rscale();
    /* bolts */
    for (int bi = 0; bi < 4; bi++) {
        sbolt *b = &g_bolt[bi]; if (!b->on) continue;
        float step = 45.0f * s * dt; int sub = (int)(step / 0.8f) + 1;
        for (int si = 0; si < sub && b->on; si++) {
            if (b->target && item_live(b->target)) {       /* it homes in a little on the one it was sent at */
                float c[3]; centre(b->target, c);
                float d[3] = { c[0] - b->pos[0], c[1] - b->pos[1], c[2] - b->pos[2] }, l = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                if (l > 0.5f) { float a = 8.0f * dt / sub; if (a > 1) a = 1; float n = 0;
                    for (int k = 0; k < 3; k++) { b->dir[k] += (d[k] / l - b->dir[k]) * a; n += b->dir[k] * b->dir[k]; }
                    n = sqrtf(n); if (n > 1e-3f) for (int k = 0; k < 3; k++) b->dir[k] /= n; }
            }
            for (int k = 0; k < 3; k++) b->pos[k] += b->dir[k] * step / sub;
            for (int i = 0; i < g_nitems; i++) {
                u32 it = g_items[i]; if (it == me || !(*(u8 *)(uintptr_t)(it + 0x10) & 1) || !is_enemy(it)) continue;
                float c[3]; centre(it, c);
                float sy = *(float *)(uintptr_t)(it + 0xec); if (!(sy > 0.05f && sy < 20)) sy = 1;
                float d2 = 0; for (int k = 0; k < 3; k++) d2 += (c[k] - b->pos[k]) * (c[k] - b->pos[k]);
                float r = 1.6f + 1.4f * sy;
                if (d2 < r * r) {
                    shrunk *e = sh_find(it);
                    if (!e || e->state != 1) { sh_start(it); g_shrink_hits++; }
                    else e->t = 0;                                   /* again: the clock starts over */
                    burst(b->pos); b->on = 0;
                    sfx_swoosh(2400.0f, 0.15f, 0.55f, 0.0f); sfx_swoosh(1200.0f, 0.4f, 0.4f, 0.0f);
                    klog("shrink: hit %08x\n", it);
                    break;
                }
            }
        }
        if (b->on) {
            memmove(b->trail[1], b->trail[0], sizeof b->trail[0] * 7); memcpy(b->trail[0], b->pos, 12); if (b->nt < 8) b->nt++;
            b->life -= dt; if (b->life <= 0) b->on = 0;
        }
    }
    /* Red over a shrunk enemy, coming down: stomped */
    const float *P = me ? (const float *)(uintptr_t)(me + 0xc8) : NULL;
    static float py; float vy = P && dt > 0 ? (P[1] - py) / dt : 0; if (P) py = P[1];
    extern int g_red_pounding;
    for (int i = 0; i < NSHRUNK; i++) {
        shrunk *e = &g_sh[i]; if (!e->item) continue;
        if (!item_live(e->item)) { klog("shrink: %08x gone%s\n", e->item, e->state == 3 ? " (stomped)" : ""); if (e->state == 3) g_shrink_kills++; memset(e, 0, sizeof *e); continue; }
        e->t += dt;
        if (e->state == 1) {                                   /* shrinking, then small for a while */
            e->k += (SHRINK_TO - e->k) * (dt * 10 > 1 ? 1 : dt * 10);
            if (e->t > SHRINK_HOLD) { e->state = 2; e->t = 0; sfx_swoosh(500.0f, 0.35f, 0.4f, 0.0f); }
        } else if (e->state == 2) {                            /* growing back */
            e->k += (1.0f - e->k) * (dt * 6 > 1 ? 1 : dt * 6);
            if (e->k > 0.995f) { sh_release(e); continue; }
        } else if (e->state == 3) {                            /* squashed flat, and finished */
            e->flat += (0.12f - e->flat) * (dt * 25 > 1 ? 1 : dt * 25);
            if (!e->hits && e->t > 0.2f) { e->hits = 1; if (finish(e->item, me)) e->hits = 2; }
            if (e->t > (e->hits == 2 ? 5.0f : 1.6f)) {
                if (e->hits == 2) { sh_release(e); continue; }       /* (dying takes its time: let go) */
                e->state = 1; e->t = SHRINK_HOLD * 0.5f; e->flat = 1; e->hits = 0;   /* it shrugged it off: up again, still small */
            }
        }
        if (e->state && e->state != 3 && P && e->k < 0.6f) {
            const float *Q = (const float *)(uintptr_t)(e->item + 0xc8);
            float dx = P[0] - Q[0], dz = P[2] - Q[2], dy = P[1] - Q[1], reach = 1.3f * s + 1.6f * e->k * (e->base[0] > 0 ? e->base[0] : 1);
            if (dx * dx + dz * dz < reach * reach && dy > -0.8f && dy < 3.0f * s && (vy < -2.0f || g_red_pounding)) {
                e->state = 3; e->t = 0; e->hits = 0;
                burst(Q);
                sfx_swoosh(80.0f, 0.3f, 1.0f, 0.0f); sfx_swoosh(260.0f, 0.15f, 0.7f, 0.0f);
                if (rand() % 2) red_sound_at(0x236, me, 0.8f, 0.0f);
                u32 ph = rd(me + 0x148);                      /* he bounces off it */
                if (okp(ph) && rd(ph + 4) == me && !g_red_pounding) *(float *)(uintptr_t)(ph + 0x7c) = 14.0f;
                klog("shrink: stomped %08x\n", e->item);
            }
        }
        sh_write(e);
    }
}

/* drawn with his other effects */
void ov_begin(int *w, int *h); void ov_glow(float x, float y, float r, u32 c); void ov_end(void);
extern int g_pres_rect[4];
void redshrink_draw(void)
{
    int any = 0; for (int i = 0; i < 4; i++) any |= g_bolt[i].on; for (int i = 0; i < 6; i++) any |= g_burst[i].t > 0;
    for (int i = 0; i < NSHRUNK; i++) any |= g_sh[i].item && g_sh[i].state == 1;
    if (!any) return;
    u32 rc = rd(0x4703f0u); if (!okp(rc)) return;
    const float *VP = (const float *)(uintptr_t)(rc + 0x8f4), *cam = (const float *)(uintptr_t)(rc + 0x934);
    int w, h; ov_begin(&w, &h);
    float pw = g_pres_rect[2] ? g_pres_rect[2] : w, ph = g_pres_rect[3] ? g_pres_rect[3] : h;
#define PROJ(p, sx, sy, R, base) ({ float cx_ = (p)[0] * VP[0] + (p)[1] * VP[4] + (p)[2] * VP[8] + VP[12], cy_ = (p)[0] * VP[1] + (p)[1] * VP[5] + (p)[2] * VP[9] + VP[13], \
        cw_ = (p)[0] * VP[3] + (p)[1] * VP[7] + (p)[2] * VP[11] + VP[15]; int ok_ = cw_ > 0.1f; if (ok_) { sx = g_pres_rect[0] + (cx_ / cw_ * 0.5f + 0.5f) * pw; sy = g_pres_rect[1] + (0.5f - cy_ / cw_ * 0.5f) * ph; \
        float dd_ = sqrtf(((p)[0] - cam[0]) * ((p)[0] - cam[0]) + ((p)[1] - cam[1]) * ((p)[1] - cam[1]) + ((p)[2] - cam[2]) * ((p)[2] - cam[2])); R = ph * (base) / (dd_ > 1 ? dd_ : 1); if (R > ph * 0.1f) R = ph * 0.1f; } ok_; })
    for (int bi = 0; bi < 4; bi++) {
        sbolt *b = &g_bolt[bi]; if (!b->on) continue;
        for (int i = b->nt - 1; i >= 0; i--) {            /* the trail, then the spark */
            float sx, sy, r; if (!PROJ(b->trail[i], sx, sy, r, 2.4f)) continue;
            r *= 1 - i * 0.09f; ov_glow(sx, sy, r, (u32)(200 * (1 - i * 0.11f)) << 24 | 0x50ff60);
        }
        float sx, sy, r; if (PROJ(b->pos, sx, sy, r, 3.0f)) { ov_glow(sx, sy, r, 0xe050ff60u); ov_glow(sx, sy, r * 0.45f, 0xf0f0ffe0u);
            if (getenv("XBOX_SHRINK_LOG")) klog("shrink: bolt drawn at %.0f %.0f r %.1f (screen %dx%d)\n", sx, sy, r, w, h); }
    }
    for (int i = 0; i < 6; i++) {
        if (g_burst[i].t <= 0) continue;
        float u = 1 - g_burst[i].t / 0.45f, sx, sy, r;
        if (PROJ(g_burst[i].pos, sx, sy, r, 4.0f + 8.0f * u)) ov_glow(sx, sy, r, (u32)(220 * (1 - u)) << 24 | 0x80ff90);
    }
    double t = (double)SDL_GetTicks_shim() / 1000.0;
    for (int i = 0; i < NSHRUNK; i++) {                    /* shrunk ones shimmer faintly green */
        shrunk *e = &g_sh[i]; if (!e->item || e->state != 1) continue;
        float c[3]; centre(e->item, c);
        float sx, sy, r; if (!PROJ(c, sx, sy, r, 1.6f)) continue;
        float pulse = 0.6f + 0.4f * sinf((float)t * 6 + i);
        float fade = e->t > SHRINK_HOLD - 2 ? ((int)(t * 8) & 1 ? 0.3f : 1.0f) : 1.0f;   /* blinks before it grows back */
        ov_glow(sx, sy, r, (u32)(90 * pulse * fade) << 24 | 0x60ff70);
    }
#undef PROJ
    ov_end();
}

static int due_at(const char *list, u32 pf)
{
    if (!list || !pf) return 0;
    char buf[256]; snprintf(buf, sizeof buf, "%s", list); char *sv;
    for (char *t = strtok_r(buf, ",", &sv); t; t = strtok_r(NULL, ",", &sv)) if ((u32)strtoul(t + (t[0] == '+'), NULL, 10) == pf) return 1;
    return 0;
}
/* tests: XBOX_GOTO_ENEMY=+frame stands him 10 from the nearest enemy, facing it;
 * XBOX_STOMP_AT=+frame drops him onto the nearest shrunk one from 4 up */
static void research_moves(void)
{
    u32 pf = debugmenu_play_frame(); if (!pf) return;
    int go = due_at(getenv("XBOX_GOTO_ENEMY"), pf), st = due_at(getenv("XBOX_STOMP_AT"), pf), side = due_at(getenv("XBOX_GOTO_ENEMY_SIDE"), pf);
    go |= side;
    if (!go && !st) return;
    u32 pl = rd(PLAYER_OBJ); if (!okp(pl)) return; u32 me = rd(pl + 4); if (!okp(me)) return;
    float *W = (float *)(uintptr_t)(me + 0xc8);
    list_items();
    u32 best = 0; float bd = 1e9f;
    for (int i = 0; i < g_nitems; i++) {
        u32 it = g_items[i]; if (it == me || !(*(u8 *)(uintptr_t)(it + 0x10) & 1) || !is_enemy(it)) continue;
        if (st) { shrunk *e = sh_find(it); if (!e || e->state != 1) continue; }
        if (getenv("XBOX_GOTO_CLASS")) { char c2[256]; class_chain(rd(it + 0x144), c2, sizeof c2); if (!strstr(c2, getenv("XBOX_GOTO_CLASS"))) continue; }
        const float *Q = (const float *)(uintptr_t)(it + 0xc8);
        float d = (Q[0] - W[0]) * (Q[0] - W[0]) + (Q[2] - W[2]) * (Q[2] - W[2]);
        if (d < bd) { bd = d; best = it; }
    }
    if (!best) { klog("shrinktest: no %s enemy\n", st ? "shrunk" : ""); return; }
    const float *Q = (const float *)(uintptr_t)(best + 0xc8);
    char ch[256]; class_chain(rd(best + 0x144), ch, sizeof ch);
    if (go) {
        float dx = W[0] - Q[0], dz = W[2] - Q[2], l = sqrtf(dx * dx + dz * dz); if (l < 1e-3f) { dx = 1; dz = 0; l = 1; }
        W[0] = Q[0] + dx / l * 10; W[1] = Q[1] + 1; W[2] = Q[2] + dz / l * 10;
        if (side) { float sd = atof(getenv("XBOX_SIDE_OFS") ? getenv("XBOX_SIDE_OFS") : "5"); W[0] = Q[0] + dx / l * 9 - dz / l * sd; W[2] = Q[2] + dz / l * 9 + dx / l * sd; }   /* (off to one side: in view) */
        *(float *)(uintptr_t)(me + 0xdc) = atan2f(dx, dz);
    } else { W[0] = Q[0]; W[1] = Q[1] + 4; W[2] = Q[2]; }
    { u32 ph = rd(me + 0x148); if (okp(ph)) memcpy((void *)(uintptr_t)(ph + 0x140), W, 12); }
    *(u8 *)(uintptr_t)(me + 0x7e) = 0;
    klog("shrinktest: %s %08x %s at %.1f %.1f %.1f (was %.1f away)\n", go ? "goto" : "stomp", best, ch, Q[0], Q[1], Q[2], sqrtf(bd));
}

void redshrink_research(void)
{
    research_moves();
    const char *ev = getenv("XBOX_ITEM_DUMP"); if (!ev) return;
    u32 pf = debugmenu_play_frame(); if (!pf) return;
    unsigned from = 0, every = 600; sscanf(ev, "%u:%u", &from, &every);
    if (pf < from || (pf - from) % every) return;
    u32 pl = rd(PLAYER_OBJ); if (!okp(pl)) return;
    u32 me = rd(pl + 4); if (!okp(me)) return;
    const float *P = (const float *)(uintptr_t)(me + 0xc8);
    klog("itemdump f%u player %08x (%s) handler %08x (%s) at %.1f %.1f %.1f\n", pf, me, class_name(me), pl, class_name(pl), P[0], P[1], P[2]);
    { int k; for (k = 0; k < 0x400; k += 4) if (rd(me + k) == pl) klog("  player item +%x -> handler\n", k); }
    u32 head = rd(0x446608u), n = okp(head) ? rd(head) : 0;
    int cnt = 0;
    for (int guard = 0; okp(n) && guard < 4000; guard++, n = rd(n + 4)) {
        u32 it = rd(n + 8); if (!okp(it) || it == me) continue;
        const float *Q = (const float *)(uintptr_t)(it + 0xc8);
        float d = sqrtf((Q[0] - P[0]) * (Q[0] - P[0]) + (Q[1] - P[1]) * (Q[1] - P[1]) + (Q[2] - P[2]) * (Q[2] - P[2]));
        cnt++;
        if (d > 80 && !getenv("XBOX_ITEM_DUMP_ALL")) continue;
        /* the handler: an object whose +4 points back at the item */
        u32 h = 0; int hoff = -1;
        for (int k = 0x10; k < 0x300 && !h; k += 4) { u32 v = rd(it + k); if (v != it && okp(v) && rd(v + 4) == it) { h = v; hoff = k; } }
        const float *sc = (const float *)(uintptr_t)(it + 0xe8);
        char ch[256]; class_chain(h, ch, sizeof ch);
        klog("  item %08x %-28s vt %08x f10 %04x d %5.1f pos %.1f %.1f %.1f sc %.2f %.2f %.2f an %08x h+%x %08x %s st3bc %x f260 %x\n", it, class_name(it), rd(it), *(u16 *)(uintptr_t)(it + 0x10), d,
             Q[0], Q[1], Q[2], sc[0], sc[1], sc[2], rd(it + 0x13c), hoff, h, ch, h ? rd(h + 0x3bc) : 0, h ? rd(h + 0x260) & 0xffff : 0);
    }
    klog("itemdump: %d items\n", cnt);
}
