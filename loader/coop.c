/* Co-op for up to four players: players 2..4 in the world, each on their own controller, with the
 * screen split (two players: left / right; three or four: four quarters, player 1 top left,
 * 2 top right, 3 bottom left, 4 bottom right).
 *
 * An extra player is a player handler made by the game's own item spawn (0x28e60, the call the
 * player factory 0xc2ee0 makes), of any of the eight player classes, or Red (a Spyro wearing Red,
 * as for player 1). A player's init writes itself to the "the player" globals ([0x44c704] handler,
 * [0x44c708] item) and its deinit (vtable slot 18, 0xdeba0 for Spyro) clears them. Each extra
 * player gets its own copy of its vtable with slot 16 (update) and 18 (deinit) replaced, and
 * whenever the game runs one of them (sw_in / sw_out) it is made "the player": the player globals,
 * Sparx ([0x44c70c], each player has their own), the controller port byte [0x44cc90] (which pad
 * the player code reads; the input manager [0x46ff98] polls all four ports itself, hle.c reports
 * them connected), the camera the player code steers by, and Red's per-player state (redctx.c:
 * every Red variable is in one section, swapped for that player's copy).
 * Where that happens: the item update 0x2b4d0 for the player's item and their Sparx's (handler
 * update, animators - Red's pose -, slot 4), their Red / size tick (coop_red_tick), their pad
 * (input_sdl.c), their camera's update (p_game_camera), their view's camera mods (coop_cam_begin)
 * and their part of the overlays (coop_overlay_begin).
 *
 * Views: the game's draw (0x1c0da0) is replaced and runs once per player each frame (game_draw);
 * each player has a copy of the game's follow camera's state, updated each frame with them as the
 * player, and on their draw the view camera that looks at player 1 gets their eye and look-at.
 * The renderer (gfx/gl.c) keeps each view (gl_coop_capture) and puts them side by side or in
 * quarters; with two views it doubles the horizontal projection of 3D draws (coop_apply) so the
 * half-width pictures keep their proportions. */
#include "xbox.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>

#define PLAYER_OBJ   0x44c704u
#define PLAYER_ITEM  0x44c708u
#define SPARX_OBJ    0x44c70cu          /* Sparx (of the player being run) */
#define PAD_PORT     0x44cc90u          /* byte: the port the player code reads */
#define RCTX_PTR     0x4703f0u
#define CAMCTL_PTR   0x42bb18u          /* [[0x42bb18]+0x4b8] = camera controller */
#define FN_SPAWN     0x28e60u           /* thiscall (0x4464e0, class, 0x42000001, pos*, rot*, 0) */
#define ITEM_MGR     0x4464e0u
#define FN_PL_FILES  0xc1a70u           /* cdecl (type) -> 0-terminated file list */
#define FN_DESTROY   0x2d770u           /* thiscall (handler): removes a handler and its item (ChangePlayer does this) */
#define FILE_MGR     0x4375a0u
#define FN_FL_LOAD   0x1f860u           /* thiscall (mgr, list, 1 load / 2 unload) */
#define NPL          4
#define CHAR_RED     9

/* player item classes by character type (the factory's jump table at 0xc311c) */
static const u32 g_pl_class[9] = { 0, 0x41000041, 0x41000042, 0x41000048, 0x41000087, 0x41000091, 0x41000054, 0x410000ec, 0x410000ed };
static u32 base_type(u32 t) { return t == CHAR_RED ? 1 : t >= 1 && t <= 8 ? t : 1; }   /* the game's player type it plays on */
int red_ctx(void); int red_ctx_set(int p); void red_ctx_reset(int p);

int g_coop;                      /* co-op wanted (menu / portcfg / XBOX_COOP) */
int g_coop_players = 2;          /* how many players then (2..4) */
int g_coop_char[NPL] = { 0, 1, 1, 1 };   /* players 2..4's characters: 1..8, 9 Red */
int g_coop_split;                /* views on screen (0 = no split, 2, 3 or 4) */
int g_coop_view;                 /* the view being drawn (= player index 0..3) */

#define CAM_STATE0 0x230u        /* the follow camera's own state (see p_game_camera) */
#define CAM_STATE1 0x3f4u
static struct cp {               /* [0] player 1 (only its view fields), [1..3] players 2..4 */
    u32 h, item, sparx;          /* handler, item, their Sparx (0 = none) */
    u32 type, ref;               /* character (1..9); the type whose files they hold a reference to */
    u32 loading; int bad;        /* waiting for files; spawn failed (not again this level) */
    float eye[3], tgt[3], yaw;   /* their camera: eye, look-at, yaw (+0x234) */
    int cam_ok; u32 cam_cc; u8 cam[CAM_STATE1 - CAM_STATE0];
    float rc[3][4]; int rc_ok;   /* the render context's view fields after their last draw */
    float vp[16]; int vp_ok;     /* ... and its view-projection */
} P[NPL];
static u32 g_cams_seen[8];       /* render cameras (EXCamera) the game has applied */
static struct { u32 cam; float eye[3], tgt[3], near; } g_cam_saved[4];
static int g_ncam_saved;         /* cameras the other players' draws retargeted, and what they had */
static u32 g_p1_seen;            /* player 1 handler at the last tick */
static int g_minigame;           /* player 1 is in a mini-game or a Ball Gadget run: the others sit it out */
static int g_scan_ours[NPL];     /* scan points we own (the item manager's 1, and our 3 and 4) */
static u8 g_p1_port;             /* player 1's pad port (the byte when nobody else is "the player") */

static int ok(u32 p) { return p >= 0x10000 && p < 0x80000000u; }
static u32 files_of(u32 type) { return type >= 1 && type <= 8 ? ((u32 (*)(u32))(uintptr_t)FN_PL_FILES)(type) : 0; }
u32 debugmenu_play_frame(void);
int dm_player_can_change(void);
static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
void coop_stack_dump(const char *tag);
static int coop_log(void) { static int v = -1; if (v < 0) v = getenv("XBOX_COOP_LOG") != NULL; return v; }
static int players(void) { return g_coop ? (g_coop_players < 2 ? 2 : g_coop_players > NPL ? NPL : g_coop_players) : 1; }
static int find_h(u32 h) { for (int k = 1; k < NPL; k++) if (h && P[k].h == h) return k; return 0; }
static int find_sparx(u32 s) { for (int k = 1; k < NPL; k++) if (s && P[k].sparx == s) return k; return 0; }
int coop_view_live(int v) { return v == 0 || (v > 0 && v < NPL && P[v].h); }   /* (gfx/gl.c) */
u32 coop_player(int k) { return k >= 1 && k < NPL ? P[k].h : 0; }      /* (debugmenu.c) */

typedef int (__attribute__((thiscall)) *upd_fn)(void *self);
typedef void (__attribute__((thiscall)) *vfn)(void *self);
typedef u32 (__attribute__((thiscall)) *spawn_fn)(void *mgr, u32 cls, u32 kind, const float *pos, const float *rot, u32 z);
typedef u32 (__attribute__((thiscall)) *fl_load_fn)(void *mgr, u32 list, u32 mode);

/* Our copies of the players' vtables (update and deinit wrapped). A removed player is deleted
 * later in the frame through its vtable, so a copy is never refilled while its old owner may still
 * be around: they are used in turn. */
#define NVTC 24
static struct vtc { u32 vt[512]; u32 orig; } g_vtc[NVTC];
static int g_vtc_next;
static struct vtc *vtc_of(void *self)
{
    u32 v = *(u32 *)self;
    for (int i = 0; i < NVTC; i++) if (v == (u32)(uintptr_t)g_vtc[i].vt) return &g_vtc[i];
    return NULL;
}
static u32 vt_orig(void *self, int slot) { struct vtc *c = vtc_of(self); return *(u32 *)(uintptr_t)(c->orig + slot * 4); }
static struct vtc *vtc_wrap(u32 h)
{
    struct vtc *c = &g_vtc[g_vtc_next++ % NVTC];
    c->orig = *(u32 *)(uintptr_t)h;
    memcpy(c->vt, (void *)(uintptr_t)c->orig, sizeof c->vt);
    *(u32 *)(uintptr_t)h = (u32)(uintptr_t)c->vt;
    return c;
}

/* pad ports: player 1 has theirs; players 2..4 the others in order */
u8 coop_port_of(int k)
{
    if (k <= 0) return g_p1_port;
    int n = 0;
    for (u8 p = 0; p < 4; p++) { if (p == g_p1_port) continue; if (++n == k) return p; }
    return (u8)k;
}
u8 coop_p2_port(void) { return coop_port_of(1); }

/* the rctx fields the player code may steer by: +0x934 position, +0x944 back vector, +0x8f4 view-proj row 0 */
static void rc_save(float o[3][4]) { u32 rc = *(u32 *)(uintptr_t)RCTX_PTR; if (!ok(rc)) return;
    memcpy(o[0], (void *)(uintptr_t)(rc + 0x934), 16); memcpy(o[1], (void *)(uintptr_t)(rc + 0x944), 16); memcpy(o[2], (void *)(uintptr_t)(rc + 0x8f4), 16); }
static void rc_load(float o[3][4]) { u32 rc = *(u32 *)(uintptr_t)RCTX_PTR; if (!ok(rc)) return;
    memcpy((void *)(uintptr_t)(rc + 0x934), o[0], 16); memcpy((void *)(uintptr_t)(rc + 0x944), o[1], 16); memcpy((void *)(uintptr_t)(rc + 0x8f4), o[2], 16); }
static u32 camctl(void) { u32 a = *(u32 *)(uintptr_t)CAMCTL_PTR; return ok(a) ? *(u32 *)(uintptr_t)(a + 0x4b8) : 0; }

/* ---- making player k "the player" ---- */
struct sw { int k, ctx, cam; u32 h, i, sp; u8 port; float rc[3][4], cc[2][3], yaw; };
static int g_sw_depth;
static void sw_in(int k, struct sw *s, int cam)     /* cam: also the camera they steer by (not mid-draw) */
{
    if (!g_sw_depth++) g_p1_port = *(u8 *)(uintptr_t)PAD_PORT;
    s->k = k; s->cam = 0;
    s->sp = *(u32 *)(uintptr_t)SPARX_OBJ; s->h = *(u32 *)(uintptr_t)PLAYER_OBJ; s->i = *(u32 *)(uintptr_t)PLAYER_ITEM; s->port = *(u8 *)(uintptr_t)PAD_PORT;
    *(u32 *)(uintptr_t)SPARX_OBJ = P[k].sparx;
    *(u32 *)(uintptr_t)PLAYER_OBJ = P[k].h; *(u32 *)(uintptr_t)PLAYER_ITEM = P[k].item;
    *(u8 *)(uintptr_t)PAD_PORT = coop_port_of(k);
    s->ctx = red_ctx_set(k);
    if (!cam || !P[k].cam_ok) return;
    if (P[k].rc_ok) { rc_save(s->rc); rc_load(P[k].rc); s->cam |= 1; }
    u32 cc = camctl();
    if (ok(cc) && P[k].cam_ok == 1 && *(u32 *)(uintptr_t)cc == 0x3202a0u) {
        memcpy(s->cc[0], (void *)(uintptr_t)(cc + 0x284), 12); memcpy(s->cc[1], (void *)(uintptr_t)(cc + 0x294), 12);
        memcpy((void *)(uintptr_t)(cc + 0x284), P[k].eye, 12); memcpy((void *)(uintptr_t)(cc + 0x294), P[k].tgt, 12);
        /* +0x234: the camera's yaw, which the player code adds to the stick angle to steer (0xc24a6) */
        s->yaw = *(float *)(uintptr_t)(cc + 0x234); *(float *)(uintptr_t)(cc + 0x234) = P[k].yaw;
        s->cam |= 2;
    }
}
static void sw_out(struct sw *s)
{
    int k = s->k;
    if (P[k].h && *(u32 *)(uintptr_t)SPARX_OBJ != P[k].sparx) P[k].sparx = *(u32 *)(uintptr_t)SPARX_OBJ;   /* (made or cleared meanwhile) */
    *(u32 *)(uintptr_t)SPARX_OBJ = s->sp;
    *(u32 *)(uintptr_t)PLAYER_OBJ = s->h; *(u32 *)(uintptr_t)PLAYER_ITEM = s->i; *(u8 *)(uintptr_t)PAD_PORT = s->port;
    red_ctx_set(s->ctx);
    if (s->cam & 1) rc_load(s->rc);
    u32 cc = camctl();
    if ((s->cam & 2) && ok(cc)) { memcpy((void *)(uintptr_t)(cc + 0x284), s->cc[0], 12); memcpy((void *)(uintptr_t)(cc + 0x294), s->cc[1], 12);
                                  *(float *)(uintptr_t)(cc + 0x234) = s->yaw; }
    g_sw_depth--;
}

static struct { u32 h, sparx; } g_dying[8];   /* players removed (deleted later, Sparx with them) */
static int g_ndying;
static int __attribute__((thiscall)) px_update(void *self)
{
    upd_fn orig = (upd_fn)(uintptr_t)vt_orig(self, 16);
    int k = find_h((u32)(uintptr_t)self);
    if (!k) return orig(self);
    struct sw s; sw_in(k, &s, 1);
    int r = orig(self);
    sw_out(&s);
    return r;
}
static void __attribute__((thiscall)) px_deinit(void *self)
{
    vfn orig = (vfn)(uintptr_t)vt_orig(self, 18);
    u32 me = (u32)(uintptr_t)self;
    int k = find_h(me);
    /* the deinit also removes Sparx ([0x44c70c]) and clears the player globals: here they are its own */
    u32 h = *(u32 *)(uintptr_t)PLAYER_OBJ, i = *(u32 *)(uintptr_t)PLAYER_ITEM, sp = *(u32 *)(uintptr_t)SPARX_OBJ;
    klog("coop: player %d %08x going away\n", k + 1, me);
    if (coop_log()) { u32 it = *(u32 *)(uintptr_t)(me + 4), ph = ok(it) ? *(u32 *)(uintptr_t)(it + 0x148) : 0;
        u32 i1 = *(u32 *)(uintptr_t)PLAYER_ITEM, ph1 = ok(i1) ? *(u32 *)(uintptr_t)(i1 + 0x148) : 0;
        klog("coop:   item %08x phys %08x (+160 %08x) / player 1 item %08x phys %08x (+160 %08x)\n", it, ph, ok(ph) ? *(u32 *)(uintptr_t)(ph + 0x160) : 0,
             i1, ph1, ok(ph1) ? *(u32 *)(uintptr_t)(ph1 + 0x160) : 0); }
    u32 msp = k ? P[k].sparx : 0;
    for (int j = 0; j < 8; j++) if (g_dying[j].h == me) { if (!k) msp = g_dying[j].sparx; g_dying[j].h = 0; }
    *(u32 *)(uintptr_t)SPARX_OBJ = msp;
    *(u32 *)(uintptr_t)PLAYER_OBJ = me; *(u32 *)(uintptr_t)PLAYER_ITEM = *(u32 *)(uintptr_t)(me + 4);
    orig(self);                       /* (unloads its character files: its reference) */
    if (k) { P[k].h = P[k].item = P[k].sparx = 0; P[k].ref = 0; }
    *(u32 *)(uintptr_t)PLAYER_OBJ = h; *(u32 *)(uintptr_t)PLAYER_ITEM = i; *(u32 *)(uintptr_t)SPARX_OBJ = sp;
}

/* an extra player's Sparx: his update and deinit run with his player as "the player" and himself as "Sparx" */
static int __attribute__((thiscall)) sx_update(void *self)
{
    upd_fn orig = (upd_fn)(uintptr_t)vt_orig(self, 16);
    int k = find_sparx((u32)(uintptr_t)self);
    if (!k || !P[k].h) return orig(self);
    struct sw s; sw_in(k, &s, 1);
    int r = orig(self);
    sw_out(&s);
    return r;
}
static void __attribute__((thiscall)) sx_deinit(void *self)
{
    vfn orig = (vfn)(uintptr_t)vt_orig(self, 18);
    u32 me = (u32)(uintptr_t)self, sp = *(u32 *)(uintptr_t)SPARX_OBJ;
    int k = find_sparx(me);
    klog("coop: player %d's Sparx %08x going away\n", k + 1, me);
    *(u32 *)(uintptr_t)SPARX_OBJ = me;
    orig(self);
    *(u32 *)(uintptr_t)SPARX_OBJ = sp == me ? 0 : sp;
    if (k) P[k].sparx = 0;
}
static void sparx_wrap(int k)
{
    if (!ok(P[k].sparx) || vtc_of((void *)(uintptr_t)P[k].sparx)) return;
    struct vtc *c = vtc_wrap(P[k].sparx);
    c->vt[16] = (u32)(uintptr_t)sx_update;
    c->vt[18] = (u32)(uintptr_t)sx_deinit;
}

/* player 1's own update gets player 1's camera in the rctx (the last view drawn was another's) */
static int __attribute__((thiscall)) p1_update(void *self)
{
    upd_fn orig = (upd_fn)(uintptr_t)vt_orig(self, 16);
    int sw = g_coop_split && P[0].rc_ok;
    float sv[3][4];
    if (sw) { rc_save(sv); rc_load(P[0].rc); }
    int r = orig(self);
    if (sw) rc_load(sv);
    return r;
}

/* Character files. The file manager keeps a load job per file with a reference count (job +0x18):
 * Load (0x1f860 mode 1 -> 0x1f080) adds a reference, or makes the job; Unload (mode 2 -> 0x1f120) drops
 * one; a job at 0 is purged some time later, which frees the data (the resource's +0x24 keeps pointing
 * at it until then). A player's deinit unloads GetPlayersFileList(its type) (0xc4510, type at +0x548),
 * so each extra player holds exactly one reference to its whole list: loaded before the spawn
 * (P[k].ref), dropped by its own deinit. Player 1's references are never touched. (Files that belong
 * to the level have no job: Load and Unload both skip them, 0x1e790.) */
static u32 res_of(u32 file) { return ((u32 (*)(u32))(uintptr_t)0x1c3fa0u)(file); }
static void files_ref(int k, u32 type, int load)
{
    u32 list = files_of(type); if (!ok(list)) return;
    klog("coop: %s player %d's character %u files\n", load ? "loading" : "releasing", k + 1, type);
    ((fl_load_fn)(uintptr_t)FN_FL_LOAD)((void *)(uintptr_t)FILE_MGR, list, load ? 1 : 2);
    P[k].ref = load ? type : 0;
}
static int files_loaded(u32 type)
{
    u32 list = files_of(type); if (!ok(list)) return 0;
    for (const u32 *l = (const u32 *)(uintptr_t)list; *l; l++) { u32 r = res_of(*l); if (!r || !*(u32 *)(uintptr_t)(r + 0x24)) return 0; }
    return 1;
}

static void spawn(int k, u32 want)
{
    u32 h1 = *(u32 *)(uintptr_t)PLAYER_OBJ, i1 = *(u32 *)(uintptr_t)PLAYER_ITEM;
    if (!ok(h1) || !ok(i1)) return;
    const float *p = (const float *)(uintptr_t)(i1 + 0xc8), *r = (const float *)(uintptr_t)(i1 + 0xd8);
    float yaw = r[1] + (k == 1 ? 0.0f : k == 2 ? (float)M_PI : (float)M_PI / 2);   /* beside player 1: right, left, behind */
    float pos[4] = { p[0] + cosf(yaw) * 2.5f, p[1] + 0.5f, p[2] - sinf(yaw) * 2.5f, p[3] }, rot[4] = { r[0], r[1], r[2], r[3] };
    u32 type = base_type(want);
    klog("coop: spawning player %d (%s type %u, class %08x) at %.1f %.1f %.1f\n", k + 1, want == CHAR_RED ? "Red on" : "", type, g_pl_class[type], pos[0], pos[1], pos[2]);
    /* the player's init makes Sparx when there is none ([0x44c70c] = 0) and puts him next to the player */
    u32 sp1 = *(u32 *)(uintptr_t)SPARX_OBJ;
    *(u32 *)(uintptr_t)SPARX_OBJ = 0;
    u32 ret = ((spawn_fn)(uintptr_t)FN_SPAWN)((void *)(uintptr_t)ITEM_MGR, g_pl_class[type], 0x42000001u, pos, rot, 0);
    u32 h2 = *(u32 *)(uintptr_t)PLAYER_OBJ, i2 = *(u32 *)(uintptr_t)PLAYER_ITEM, sp2 = *(u32 *)(uintptr_t)SPARX_OBJ;
    *(u32 *)(uintptr_t)PLAYER_OBJ = h1; *(u32 *)(uintptr_t)PLAYER_ITEM = i1; *(u32 *)(uintptr_t)SPARX_OBJ = sp1;
    klog("coop: spawn returned %08x, new handler %08x item %08x Sparx %08x (player 1 %08x / %08x)\n", ret, h2, i2, sp2 != sp1 ? sp2 : 0, h1, i1);
    if (h2 == h1 || !ok(h2) || !ok(i2) || find_h(h2)) { klog("coop: player %d did not appear\n", k + 1); P[k].bad = 1; return; }
    P[k].h = h2; P[k].item = i2; P[k].sparx = sp2 != sp1 ? sp2 : 0; P[k].type = want; P[k].cam_ok = 0;
    {   /* their Red state starts afresh; Red wanted: his tick (debugmenu.c) dresses them */
        extern int g_redchar_want; red_ctx_reset(k);
        int was = red_ctx_set(k); g_redchar_want = want == CHAR_RED; red_ctx_set(was); }
    sparx_wrap(k);
    struct vtc *c = vtc_wrap(h2);
    c->vt[16] = (u32)(uintptr_t)px_update;
    c->vt[18] = (u32)(uintptr_t)px_deinit;
    *(u32 *)(uintptr_t)(h2 + 0x548) = type;
}

/* ---- each extra player's camera: the game's own follow camera, run once more for them ----
 * The follow camera is an item handler (vtable 0x3202a0, size 0x3f4, update slot 16 = 0x144940);
 * its state is +0x230..+0x3f4 (eye +0x284, look-at +0x294, yaw +0x234, mode +0x3dc, followed item
 * +0x384). Each extra player has a copy of that state: before the frame is drawn the camera's state
 * is swapped for theirs, its update runs with them as "the player" (their pad, so their right stick
 * turns it), and the result is kept and player 1's state put back. Walls, the game's camera modes
 * and its stick handling all come with it. */
#define EXCAMERA_VT 0x33993cu        /* (the vtable pointer the camera objects hold) */
static int cam_live(u32 c) { return ok(c) && *(u32 *)(uintptr_t)c == EXCAMERA_VT; }
/* Another camera is in charge (the Ball Gadget's, a cut-scene's...): the player sees themselves as
 * player 1's view sees player 1 - same offset from the player, same direction. */
static void p_follow_p1_view(int k)
{
    u32 i1 = *(u32 *)(uintptr_t)PLAYER_ITEM;
    if (!ok(i1)) return;
    const float *Q = (const float *)(uintptr_t)(i1 + 0xc8), *Pk = (const float *)(uintptr_t)(P[k].item + 0xc8);
    for (int j = 0; j < 8 && g_cams_seen[j]; j++) {
        u32 c = g_cams_seen[j]; if (!cam_live(c)) continue;
        const float *e = (const float *)(uintptr_t)(c + 0x1c), *t = (const float *)(uintptr_t)(c + 0x2c);
        float d2 = 0; for (int a = 0; a < 3; a++) d2 += (t[a] - Q[a]) * (t[a] - Q[a]);
        if (d2 > 30.0f * 30.0f) continue;
        for (int a = 0; a < 3; a++) { P[k].eye[a] = Pk[a] + e[a] - Q[a]; P[k].tgt[a] = Pk[a] + t[a] - Q[a]; }
        P[k].cam_ok = 2; return;
    }
}
static void p_game_camera(int k)
{
    u32 cc = camctl();
    if (!P[k].h || !ok(P[k].item)) return;
    if (!ok(cc) || *(u32 *)(uintptr_t)cc != 0x3202a0u) { p_follow_p1_view(k); return; }
    if (P[k].cam_ok == 2) P[k].cam_ok = 0;            /* (back from another camera: start over behind them) */
    u8 *st = (u8 *)(uintptr_t)(cc + CAM_STATE0);
    if (P[k].cam_cc != cc || !P[k].cam_ok) {          /* start from player 1's camera, moved behind them */
        memcpy(P[k].cam, st, sizeof P[k].cam);
        P[k].cam_cc = cc;
        u32 i1 = *(u32 *)(uintptr_t)PLAYER_ITEM;
        if (ok(i1)) {                                  /* (positions: the update lifts all five together, 0x144a0d) */
            const float *Pk = (const float *)(uintptr_t)(P[k].item + 0xc8), *Q = (const float *)(uintptr_t)(i1 + 0xc8);
            static const u32 pos[5] = { 0x254, 0x284, 0x294, 0x2a4, 0x2b4 };
            for (int j = 0; j < 5; j++) for (int a = 0; a < 3; a++) *(float *)(P[k].cam + pos[j] + 4 * a - CAM_STATE0) += Pk[a] - Q[a];
        }
    }
    *(u32 *)(P[k].cam + 0x384 - CAM_STATE0) = P[k].item;
    static u8 p1[CAM_STATE1 - CAM_STATE0];
    memcpy(p1, st, sizeof p1);
    memcpy(st, P[k].cam, sizeof P[k].cam);
    static u8 cams[8][0x50];             /* the update also sets the render camera: player 1's is put back */
    for (int j = 0; j < 8 && g_cams_seen[j]; j++) if (cam_live(g_cams_seen[j])) memcpy(cams[j], (void *)(uintptr_t)(g_cams_seen[j] + 0x10), 0x50);
    struct sw s; sw_in(k, &s, 0);
    ((upd_fn)(uintptr_t)*(u32 *)(uintptr_t)(*(u32 *)(uintptr_t)cc + 16 * 4))((void *)(uintptr_t)cc);
    sw_out(&s);
    for (int j = 0; j < 8 && g_cams_seen[j]; j++) if (cam_live(g_cams_seen[j])) memcpy((void *)(uintptr_t)(g_cams_seen[j] + 0x10), cams[j], 0x50);
    memcpy(P[k].cam, st, sizeof P[k].cam);
    memcpy(P[k].eye, st + 0x284 - CAM_STATE0, 12); memcpy(P[k].tgt, st + 0x294 - CAM_STATE0, 12);
    P[k].yaw = *(float *)(st + 0x234 - CAM_STATE0);
    memcpy(st, p1, sizeof p1);
    if (coop_log()) { static u32 n[NPL]; if (n[k]++ % 60 == 0) klog("coop: p%d game camera eye %.1f %.1f %.1f tgt %.1f %.1f %.1f yaw %.2f mode %u\n", k + 1,
        P[k].eye[0], P[k].eye[1], P[k].eye[2], P[k].tgt[0], P[k].tgt[1], P[k].tgt[2], P[k].yaw, *(u32 *)(P[k].cam + 0x3dc - CAM_STATE0)); }
    P[k].cam_ok = 1;
}

/* camera hook (debugmenu.c size_camera): on another player's view the view camera (the one looking
 * at player 1) gets that player's eye and look-at, and the camera mods after this run as that player
 * (their item and pad as "the player", their Red state) until coop_cam_end. What the camera had is
 * put back after all the views are drawn (game_draw). */
static struct sw g_cam_sw; static int g_cam_in;
int coop_cam_begin(u32 cam)
{
    { int k; for (k = 0; k < 8 && g_cams_seen[k] && g_cams_seen[k] != cam; k++) {}
      if (k < 8 && !g_cams_seen[k] && coop_log()) klog("coop: render camera %08x vt %08x\n", cam, *(u32 *)(uintptr_t)cam);
      if (k < 8) g_cams_seen[k] = cam; }
    coop_stack_dump("camera");
    int v = g_coop_view;
    if (!g_coop_split || v < 1 || v >= NPL || !P[v].cam_ok || !P[v].h || g_cam_in) return 0;
    u32 i1 = *(u32 *)(uintptr_t)PLAYER_ITEM;
    if (!ok(i1)) return 0;
    const float *Q = (const float *)(uintptr_t)(i1 + 0xc8), *tgt = (const float *)(uintptr_t)(cam + 0x2c);
    float d2 = 0; for (int a = 0; a < 3; a++) d2 += (tgt[a] - Q[a]) * (tgt[a] - Q[a]);
    int j = 0; while (j < g_ncam_saved && g_cam_saved[j].cam != cam) j++;
    if (j == g_ncam_saved) {                       /* (the first retarget this frame: what player 1's view had) */
        if (d2 > 30.0f * 30.0f || j == 4) return 0;
        g_ncam_saved++; g_cam_saved[j].cam = cam;
        memcpy(g_cam_saved[j].eye, (void *)(uintptr_t)(cam + 0x1c), 12); memcpy(g_cam_saved[j].tgt, (void *)(uintptr_t)(cam + 0x2c), 12);
        g_cam_saved[j].near = *(float *)(uintptr_t)(cam + 0x4c);
    }
    memcpy((void *)(uintptr_t)(cam + 0x1c), P[v].eye, 12); memcpy((void *)(uintptr_t)(cam + 0x2c), P[v].tgt, 12);
    *(float *)(uintptr_t)(cam + 0x4c) = g_cam_saved[j].near;
    sw_in(v, &g_cam_sw, 0); g_cam_in = 1;           /* (mid-draw: the render context stays as it is) */
    return 1;
}
void coop_cam_end(void)
{
    if (!g_cam_in) return;
    g_cam_in = 0; sw_out(&g_cam_sw);
}

/* The game's draw (0x1c0da0, slot 0x70 of the game object, after its update 0x6c): the render
 * context's begin (vt 0x24), the scene 0x1c17f0, its end (vt 0x28). In split screen it runs once per
 * player each frame; gl_coop_capture keeps each one. */
static void game_draw_once(void)
{
    u32 rc = *(u32 *)(uintptr_t)RCTX_PTR; u32 vt = *(u32 *)(uintptr_t)rc;
    ((vfn)(uintptr_t)*(u32 *)(uintptr_t)(vt + 0x24))((void *)(uintptr_t)rc);
    ((vfn)(uintptr_t)0x1c17f0u)((void *)(uintptr_t)rc);
    ((vfn)(uintptr_t)*(u32 *)(uintptr_t)(vt + 0x28))((void *)(uintptr_t)rc);
}
static void game_draw(void)
{
    extern void gl_coop_capture(int v);
    extern void gl_set_view_fraction(float fx, float fy);
    for (int k = 1; k < NPL; k++) if (P[k].h) p_game_camera(k);
    if (!g_coop_split) { g_coop_view = 0; gl_set_view_fraction(1, 1); game_draw_once(); return; }
    /* each view drawn at the size it is shown (XBOX_COOP_FULLRES=1: every view at full size, as coop4) */
    static int full = -1; if (full < 0) full = getenv("XBOX_COOP_FULLRES") != NULL;
    if (!full) gl_set_view_fraction(0.5f, g_coop_split == 2 ? 1.0f : 0.5f);
    for (int v = 0; v < g_coop_split; v++) {
        if (v && !P[v].h) continue;                    /* (an empty quarter stays black) */
        g_coop_view = v;
        game_draw_once();
        gl_coop_capture(v);
        rc_save(P[v].rc); P[v].rc_ok = 1;
        u32 rc = *(u32 *)(uintptr_t)RCTX_PTR; if (ok(rc)) { memcpy(P[v].vp, (void *)(uintptr_t)(rc + 0x8f4), 64); P[v].vp_ok = 1; }
    }
    /* the rest of the game (camera controller, sound, the next update) sees player 1's camera again,
     * and the cameras the other views retargeted get back what they had */
    g_coop_view = 0;
    gl_set_view_fraction(1, 1);
    rc_load(P[0].rc);
    for (int j = 0; j < g_ncam_saved; j++) { memcpy((void *)(uintptr_t)(g_cam_saved[j].cam + 0x1c), g_cam_saved[j].eye, 12);
                                             memcpy((void *)(uintptr_t)(g_cam_saved[j].cam + 0x2c), g_cam_saved[j].tgt, 12);
                                             *(float *)(uintptr_t)(g_cam_saved[j].cam + 0x4c) = g_cam_saved[j].near; }
    g_ncam_saved = 0;
}
static void draw_hook_install(void)
{
    static int done; if (done) return; done = 1;
    static const u8 want[] = { 0x56, 0x8b, 0x35, 0xf0, 0x03, 0x47, 0x00, 0x8b, 0x06, 0x8b, 0xce, 0xff, 0x50, 0x24 };
    extern void hle_patch_jmp(u32 at, void *to);
    if (!memcmp((void *)(uintptr_t)0x1c0da0u, want, sizeof want)) hle_patch_jmp(0x1c0da0u, (void *)game_draw);
    else klog("coop: draw hook: unexpected bytes\n");
}
/* renderer: a frame just finished */
void coop_frame_done(void) { g_ncam_saved = 0; g_coop_view = 0; }

/* ---- the item update: players 2..4 and their Sparx run as themselves ----
 * 0x2b4d0 (thiscall item) runs an item's handler update, then its animators (Red's pose is made
 * there, redpose.c), then its handler's slot 4. */
void __attribute__((thiscall)) item_upd_orig(void *item);
__asm__(".globl item_upd_orig\n"
        "item_upd_orig:\n"
        "  pushl %esi\n"
        "  movl %ecx, %esi\n"
        "  testb $2, 0x10(%esi)\n"
        "  pushl $0x2b4d7\n"               /* (push and ret keep the flags for its je) */
        "  ret\n");
static void __attribute__((thiscall)) item_update_hook(void *item)
{
    u32 it = (u32)(uintptr_t)item; int k = 0;
    for (int j = 1; j < NPL && !k; j++)
        if (P[j].h && (it == P[j].item || (ok(P[j].sparx) && it == *(u32 *)(uintptr_t)(P[j].sparx + 4)))) k = j;
    if (!k) { item_upd_orig(item); return; }
    struct sw s; sw_in(k, &s, 1);
    item_upd_orig(item);
    sw_out(&s);
}
static void item_hook_install(void)
{
    static int done; if (done) return; done = 1;
    static const u8 want[] = { 0x56, 0x8b, 0xf1, 0xf6, 0x46, 0x10, 0x02, 0x74 };
    extern void hle_patch_jmp(u32 at, void *to);
    if (!memcmp((void *)(uintptr_t)0x2b4d0u, want, sizeof want)) hle_patch_jmp(0x2b4d0u, (void *)item_update_hook);
    else klog("coop: item update hook: unexpected bytes\n");
}

/* ---- waking the world near every player ----
 * Objects exist only while a scan point (the item manager's table at 0x4464e0: 3 entries of 0x18,
 * flags, position, zone +0x14) is in range; the range test 0x290e0 walks them. Player 1 is entry 0;
 * player 2 gets the spare entry 1. For players 3 and 4 the test is pointed at a table of five of
 * ours (mov edi,0x4464e0 at 0x290ea; cmp ebp,3 at 0x29156) that copies the game's three each frame. */
static u8 g_scan5[5 * 0x18];
static int g_scan5_on;
static void scan5_install(void)
{
    if (g_scan5_on) return;
    if (getenv("XBOX_COOP_NOSCAN5")) { g_scan5_on = -1; return; }
    u8 *a = (u8 *)(uintptr_t)0x290eau, *b = (u8 *)(uintptr_t)0x29156u;
    if (a[0] != 0xbf || *(u32 *)(a + 1) != ITEM_MGR || b[0] != 0x83 || b[1] != 0xfd || b[2] != 3) { klog("coop: scan table: unexpected bytes\n"); g_scan5_on = -1; return; }
    memcpy(g_scan5, (void *)(uintptr_t)ITEM_MGR, 3 * 0x18);
    *(u32 *)(a + 1) = (u32)(uintptr_t)g_scan5; b[2] = 5;
    g_scan5_on = 1;
}
static void scan_tick(void)
{
    u32 *e0 = (u32 *)(uintptr_t)ITEM_MGR;
    for (int k = 1; k < NPL; k++) {
        u32 *e = k == 1 ? (u32 *)(uintptr_t)(ITEM_MGR + 0x18) : (u32 *)(g_scan5 + (k + 1) * 0x18);
        if (P[k].h && ok(P[k].item)) {
            if (k >= 2) scan5_install();
            if (k == 1 && (e[0] & 1) && !g_scan_ours[k]) continue;      /* (the game's own use of it) */
            e[0] = e0[0]; memcpy(e + 1, (void *)(uintptr_t)(P[k].item + 0xc8), 12); ((float *)e)[4] = 0;
            *(u16 *)((u8 *)e + 0x14) = *(u16 *)(uintptr_t)(ITEM_MGR + 0x14);
            g_scan_ours[k] = 1;
        } else if (g_scan_ours[k]) { e[0] = 0; g_scan_ours[k] = 0; }
    }
    if (g_scan5_on == 1) memcpy(g_scan5, (void *)(uintptr_t)ITEM_MGR, 3 * 0x18);
}

/* debugmenu.c's game tick, after player 1's Red and size: the other players' */
void coop_red_tick(void)
{
    void dm_red_player_tick(void);
    for (int k = 1; k < NPL; k++) {
        if (!P[k].h || !ok(P[k].item)) continue;
        struct sw s; sw_in(k, &s, 1);
        dm_red_player_tick();
        sw_out(&s);
    }
}

/* debugmenu.c's overlays (Red's effects, meter, reticle): in split screen once per view, each with
 * its part of the window, its view-projection and its player. 0 = no (more) views: draw once as
 * usual when it says 0 for view 0. */
static int g_ov_rect[4]; static float g_ov_vp[16]; static struct sw g_ov_sw; static int g_ov_in;
int coop_overlay_begin(int v)
{
    extern int g_pres_rect[4];
    u32 rc = *(u32 *)(uintptr_t)RCTX_PTR;
    while (v < g_coop_split && v && !P[v].h) v++;
    if (!g_coop_split || v >= g_coop_split || !P[v].vp_ok || !ok(rc)) return 0;
    memcpy(g_ov_rect, g_pres_rect, sizeof g_ov_rect);
    int quad = g_coop_split > 2, hw = g_pres_rect[2] / 2, hh = g_pres_rect[3] / 2;
    if (quad) { g_pres_rect[0] += (v & 1) * hw; g_pres_rect[1] += (v >> 1) * hh; g_pres_rect[2] = hw; g_pres_rect[3] = hh; }
    else { g_pres_rect[0] += v * hw; g_pres_rect[2] = hw; }
    memcpy(g_ov_vp, (void *)(uintptr_t)(rc + 0x8f4), 64);
    float *VP = (float *)(uintptr_t)(rc + 0x8f4);
    memcpy(VP, P[v].vp, 64);
    if (!quad) for (int a = 0; a < 4; a++) VP[a * 4] *= 2.0f;   /* each half shows the middle of its view, squeezed (coop_apply) */
    g_ov_in = v;
    if (v) sw_in(v, &g_ov_sw, 0);
    return v + 1;
}
void coop_overlay_end(void)
{
    extern int g_pres_rect[4];
    if (g_ov_in) sw_out(&g_ov_sw);
    g_ov_in = 0;
    memcpy(g_pres_rect, g_ov_rect, sizeof g_ov_rect);
    u32 rc = *(u32 *)(uintptr_t)RCTX_PTR; if (ok(rc)) memcpy((void *)(uintptr_t)(rc + 0x8f4), g_ov_vp, 64);
}

/* research: the game's return addresses on the stack (XBOX_COOP_STACK=+frame) */
void coop_stack_dump(const char *tag)
{
    const char *e = getenv("XBOX_COOP_STACK"); if (!e || !debugmenu_play_frame()) return;
    static char seen[8][32]; static int ns;
    for (int i = 0; i < ns; i++) if (!strcmp(seen[i], tag)) return;
    if (debugmenu_play_frame() < parse_u32(e + (e[0] == '+'), 10) || ns >= 8) return;
    snprintf(seen[ns++], 32, "%s", tag);
    u32 *sp = (u32 *)__builtin_frame_address(0); char line[2048]; int n = 0;
    for (int i = 0; i < 2048 && n < 2000; i++) {
        u32 v = sp[i]; if (v < 0x11000 || v > 0x290000) continue;
        const u8 *c = (const u8 *)(uintptr_t)v;
        if (c[-5] == 0xe8 || (c[-6] == 0xff && (c[-5] & 0x38) == 0x10) || (c[-2] == 0xff && (c[-1] & 0x38) == 0x10) || (c[-3] == 0xff && (c[-2] & 0x38) == 0x10))
            n += snprintf(line + n, sizeof line - n, " %x", v);
    }
    klog("coop: stack at %s:%s\n", tag, line);
}

/* tests: VAR=+frame:value[,..] (frames after gameplay began), each fired once */
static void test_list(const char *var, char done[8], void (*fire)(unsigned a, unsigned b), int two)
{
    const char *s = getenv(var); if (!s || !debugmenu_play_frame()) return;
    char buf[128]; snprintf(buf, sizeof buf, "%s", s); char *sv = NULL; int n = 0;
    for (char *tok = strtok_r(buf, ",", &sv); tok && n < 8; tok = strtok_r(NULL, ",", &sv), n++) {
        unsigned f = 0, b = 0;
        if (sscanf(tok + (tok[0] == '+'), two ? "%u:%u" : "%u", &f, &b) < (two ? 2 : 1) || done[n] || debugmenu_play_frame() < f) continue;
        done[n] = 1; fire(f, b);
    }
}
static void t_char2(unsigned f, unsigned ty) { g_coop_char[1] = (int)ty; klog("coop: test: player 2 -> type %u\n", ty); }
static void t_players(unsigned f, unsigned n) { g_coop = 1; g_coop_players = (int)n; klog("coop: test: %u players\n", n); }
static int g_test_kill;
static void t_hurt(unsigned f, unsigned b)
{
    if (!P[1].h || !ok(P[1].item)) return;
    if (g_test_kill) *(u32 *)(uintptr_t)0x43cd14u = 0;
    const float *Q = (const float *)(uintptr_t)(P[1].item + 0xc8);
    klog("coop: test hit on player 2 (health %u)\n", *(u32 *)(uintptr_t)0x43cd14u);
    typedef void (__attribute__((thiscall)) *hurt_fn)(void *, u32, u32, u32, float, float, float, float, float, float, float, float, float, u32, u32);
    ((hurt_fn)(uintptr_t)0xca670u)((void *)(uintptr_t)P[1].h, 0x20, 0, 3, Q[0] + 1, Q[1], Q[2], 1, Q[0], Q[1] + 1, Q[2], 1, 10.0f, 0, 0);
}

void coop_tick(void)
{
    static int inited;
    coop_stack_dump("swap");
    if (!inited && debugmenu_play_frame()) {       /* (after portcfg.txt is read: the environment wins) */
        inited = 1; if (getenv("XBOX_COOP")) g_coop = atoi(getenv("XBOX_COOP"));
        if (getenv("XBOX_COOP_PLAYERS")) g_coop_players = atoi(getenv("XBOX_COOP_PLAYERS"));
        static const char *cv[NPL] = { NULL, "XBOX_COOP_CHAR", "XBOX_COOP_CHAR3", "XBOX_COOP_CHAR4" };
        for (int k = 1; k < NPL; k++) if (getenv(cv[k])) g_coop_char[k] = atoi(getenv(cv[k]));
    }
    { static char d1[8], d2[8], d3[8], d4[8];
      test_list("XBOX_COOP_CHAR_AT", d1, t_char2, 1);          /* +frame:type: player 2's character */
      test_list("XBOX_COOP_PLAYERS_AT", d2, t_players, 1);     /* +frame:n: n players */
      g_test_kill = 0; test_list("XBOX_COOP_HURT", d3, t_hurt, 0);
      g_test_kill = 1; test_list("XBOX_COOP_KILL", d4, t_hurt, 0); }
    {   static int done; const char *s = getenv("XBOX_COOP_AT");   /* tests: XBOX_COOP_AT=+frame turns co-op on then */
        if (s && !done && debugmenu_play_frame() && debugmenu_play_frame() >= parse_u32(s + (s[0] == '+'), 10)) { done = 1; g_coop = 1; } }
    if (!g_sw_depth) g_p1_port = *(u8 *)(uintptr_t)PAD_PORT;
    u32 h1 = *(u32 *)(uintptr_t)PLAYER_OBJ;
    static int p1_age;               /* ticks since player 1 last changed (a level load makes a new one) */
    if (h1 != g_p1_seen) { g_p1_seen = h1; for (int k = 1; k < NPL; k++) P[k].bad = 0; p1_age = 0;
                           memset(g_cams_seen, 0, sizeof g_cams_seen); }      /* (a new level: those cameras are gone) */ else if (p1_age < 1000000) p1_age++;
    extern int g_coop_back[NPL]; int n = players();
    /* the mini-games (Sparx's shooter, Blink's, Sgt. Byrd's...) and the Ball Gadget runs are
     * one-player games built around "the player": the others sit them out (Sparx's crashed with
     * extra players in it; an extra Ball Gadget is never let onto the track by the start pad and
     * drops off the level) and come back next to player 1 afterwards. */
    const char *dm_map_class(void);
    int minigame = !strncmp(dm_map_class(), "Map_MiniGame_", 13) || (ok(h1) && *(u32 *)(uintptr_t)(h1 + 0x548) == 6);
    if (minigame != g_minigame) { g_minigame = minigame; klog("coop: %s a mini-game / Ball Gadget run\n", minigame ? "in" : "out of"); }

    for (int k = 1; k < NPL; k++) {
        /* a player goes away: co-op switched off, fewer players, another character, or Back on their
         * pad (they come straight back next to player 1) */
        int back = g_coop_back[k]; g_coop_back[k] = 0;
        int want = g_coop_char[k] >= 1 && g_coop_char[k] <= CHAR_RED ? g_coop_char[k] : 1;
        if (P[k].h && ok(h1) && (k >= n || (u32)want != P[k].type || back) && dm_player_can_change()) {
            klog("coop: removing player %d (%s)\n", k + 1, k >= n ? "co-op off / fewer players" : back ? "rejoin" : "new character");
            g_dying[g_ndying % 8].h = P[k].h; g_dying[g_ndying % 8].sparx = P[k].sparx; g_ndying++;
            ((vfn)(uintptr_t)FN_DESTROY)((void *)(uintptr_t)P[k].h);
            P[k].h = P[k].item = P[k].sparx = 0; P[k].cam_ok = 0;
        }
        if (k >= n && !P[k].h && P[k].ref) files_ref(k, P[k].ref, 0);      /* switched off before they appeared */
        if (k < n && !minigame && !P[k].h && !P[k].bad && ok(h1) && p1_age > 90 && debugmenu_play_frame() && dm_player_can_change()) {
            u32 type = base_type((u32)want);
            draw_hook_install(); item_hook_install();
            if (P[k].ref != type) {                    /* one reference to the character's files, then wait for them */
                if (P[k].ref) files_ref(k, P[k].ref, 0);
                files_ref(k, type, 1); P[k].loading = 0;
            }
            if (!files_loaded(type)) { if (++P[k].loading > 3000) { klog("coop: player %d's files didn't load\n", k + 1); P[k].bad = 1; } }
            else { P[k].loading = 0; spawn(k, (u32)want); }
        }
        if (P[k].h) sparx_wrap(k);
    }
    int any = 0; for (int k = 1; k < NPL; k++) any |= P[k].h != 0;
    if (any && ok(h1) && !vtc_of((void *)(uintptr_t)h1)) {     /* (a new player 1 can reuse an address) */
        struct vtc *c = vtc_wrap(h1);
        c->vt[16] = (u32)(uintptr_t)p1_update;
    }
    scan_tick();
    for (int k = 1; k < NPL; k++) if (!P[k].h) P[k].cam_ok = 0;
    int last = 0; for (int k = 1; k < NPL; k++) if (P[k].h && P[k].cam_ok) last = k;
    g_coop_split = !last || getenv("XBOX_COOP_NOSPLIT") ? 0 : n > 2 || last > 1 ? 4 : 2;
    if (coop_log()) { static u32 c; if (c++ % 30 == 0) for (int k = 1; k < NPL; k++) if (P[k].h && ok(P[k].item)) {
        const float *Q = (const float *)(uintptr_t)(P[k].item + 0xc8);
        klog("coop: p%d pos %.2f %.2f %.2f yaw %.2f mode %u port %u split %d hp %u zone %u/%u\n", k + 1, Q[0], Q[1], Q[2], *(float *)(uintptr_t)(P[k].item + 0xdc),
             *(u32 *)(uintptr_t)(P[k].h + 0x7ec), coop_port_of(k), g_coop_split, *(u32 *)(uintptr_t)0x43cd14u, *(u16 *)(uintptr_t)(P[k].item + 0x94), *(u16 *)(uintptr_t)(ITEM_MGR + 0x14)); } }
}

/* renderer (gfx/gl.c): with two views 3D draws on the back buffer get twice the horizontal
 * projection, so the frame (shown at half width) keeps its proportions; a quarter keeps the
 * screen's shape as it is. Perspective transforms only, as freecam. */
int coop_apply(float (*c)[4])
{
    if (g_coop_split != 2) return 0;
    float w2 = c[96][3] * c[96][3] + c[97][3] * c[97][3] + c[98][3] * c[98][3];
    float x2 = c[96][0] * c[96][0] + c[97][0] * c[97][0] + c[98][0] * c[98][0];
    if (!(w2 > 1e-6f * x2)) return 0;
    for (int j = 96; j < 100; j++) c[j][0] *= 2.0f;
    return 1;
}
