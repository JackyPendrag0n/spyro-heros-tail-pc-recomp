/* Port debug menu (Spyro: A Hero's Tail, NTSC XBE built 2004-09-21).
 *
 * Opened with F1 (keyboard) or by clicking both sticks (L3 + R3) on a controller.
 *   - Warp: every map class the game registered, with a start point; the warp is the
 *     game's own map change (the function its teleporters and cheats call).
 *   - Free camera, frame-rate and widescreen settings.
 *
 * Game addresses (found by reverse engineering, see NOTES.md):
 *   0x43a9e8        world/map manager
 *   0x1fd60         thiscall GoToMap(this=0x43a9e8, u32 mapHash, s32 startPoint (-1 = default), void *ctx)
 *   0x446000/8      registered map count / array of Map objects (+0xdc = map .edb hash, +0xe0 = sub id)
 *   vtable[0]       Map::GetClassDesc -> desc (+4 = class name, e.g. "Map_DragonVillage")
 *   0x42bb20        player-ish object; GoToMap callers pass (p ? p + 0x270 : 0) as ctx
 *   start points    0x4a000000 START, 0x4a000001..6 Restart1..6 */
#include "xbox.h"
#include <SDL2/SDL.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>
#include "edb_hashes.h"

int g_freecam_enabled;      /* gfx/freecam.c */
static char g_status[128];

#define MAP_COUNT_ADDR  0x446000u
#define MAP_ARRAY_ADDR  0x446008u
#define MAP_MANAGER     0x43a9e8u
#define FN_GOTOMAP      0x1fd60u
#define PLAYER_PTR      0x42bb20u

typedef u32 (__attribute__((thiscall)) *gotomap_fn)(void *self, u32 hash, s32 start, u32 ctx);
/* 0x1fce0: thiscall QueueMap(this=0x43a9e8, Map *map, ctx) -> 1 if accepted;
 * 0x22cf0: thiscall SetStart(this=map->[+0x120], s32 start, 0) */
typedef u32 (__attribute__((thiscall)) *queuemap_fn)(void *self, u32 map, u32 ctx);
typedef u32 (__attribute__((thiscall)) *setstart_fn)(void *self, s32 start, u32 zero);
#define FN_QUEUEMAP 0x1fce0u
#define FN_SETSTART 0x22cf0u
typedef u32 (*getdesc_fn)(void);

static int edb_shipped(u32 h)
{
    int lo = 0, hi = (int)(sizeof g_edb_hashes / sizeof g_edb_hashes[0]) - 1;
    while (lo <= hi) { int m = (lo + hi) / 2; if (g_edb_hashes[m] == h) return 1; if (g_edb_hashes[m] < h) lo = m + 1; else hi = m - 1; }
    return 0;
}
static const char *edb_file(u32 h)
{
    for (size_t i = 0; i < sizeof g_edb_names / sizeof g_edb_names[0]; i++) if (g_edb_names[i].hash == h) return g_edb_names[i].file;
    return NULL;
}

static int guest_ptr_ok(u32 p) { return p >= 0x10000 && p < 0x80000000u; }

/* ---- map list ---- */
struct mapent { u32 obj, hash, sub; char name[64]; char file[24]; int shipped, section; };
static struct mapent g_maps[160];
static int g_nmaps;

/* "Map_DragonVillage" -> "Dragon Village", "Map_MiniGame_MR1_Blk" -> "MiniGame MR1 Blk" */
static void pretty(const char *cls, char *out, size_t n)
{
    if (!strncmp(cls, "Map_", 4)) cls += 4;
    size_t k = 0;
    for (size_t i = 0; cls[i] && k + 2 < n; i++) {
        char c = cls[i];
        if (c == '_') { out[k++] = ' '; continue; }
        if (i && c >= 'A' && c <= 'Z' && cls[i - 1] >= 'a' && cls[i - 1] <= 'z') out[k++] = ' ';
        out[k++] = c;
    }
    out[k] = 0;
}

/* friendly names for the game's map classes; anything else is a developer/test map */
static const struct { const char *cls, *name; } g_names[] = {   /* names as in the game's own level table */
    { "Map_DragonVillage", "1A Dragon Village (hub)" }, { "Map_DragonSwamp", "1B Dragon Swamp" },
    { "Map_DragonShores", "1C Dragon Shores" }, { "Map_GnastyGnorc", "1Z Gnasty Gnorc (boss)" },
    { "Map_R1LinkAB", "1 link A-B" }, { "Map_R1LinkAC", "1 link A-C" },
    { "Map_Beach", "2A The Beach (hub)" }, { "Map_SunkenCity", "2B Sunken City" }, { "Map_CloudCity", "2C Cloud City" },
    { "Map_WateryTomb", "2Z Watery Tomb (boss)" }, { "Map_R2LinkAB", "2 link A-B" }, { "Map_R2LinkAC", "2 link A-C" },
    { "Map_FrostbiteVillage", "3A Frostbite Village (hub)" }, { "Map_MountainPass", "3B Mountain Pass" },
    { "Map_IceCitadel", "3C Ice Citadel" }, { "Map_RedsChamber", "3Z Red's Chamber (boss)" },
    { "Map_StormyBeach", "4A Stormy Beach (hub)" }, { "Map_VolcanoAscent", "4B Volcano Ascent" },
    { "Map_VolcanoDescent", "4C Volcano Descent" }, { "Map_Foundry", "4D Mine & Foundry" },
    { "Map_ProfessorsLab", "4E Professor's Laboratory" }, { "Map_MechaRed", "4Z MechaRed (boss)" },
    { "Map_R4LinkBC", "4 link B-C" }, { "Map_R4LinkCD", "4 link C-D" }, { "Map_R4LinkDE", "4 link D-E" },
    { "Map_Titles", "Title screen" }, { "Map_Credits", "Credits" },
};
static int cmp_maps(const void *a, const void *b)
{
    const struct mapent *x = a, *y = b;
    if (x->section != y->section) return x->section - y->section;
    return strcmp(x->name, y->name);
}


static void scan_maps(void)
{
    g_nmaps = 0;
    u32 n = *(volatile u32 *)(uintptr_t)MAP_COUNT_ADDR;
    if (n > 160) n = 160;
    for (u32 i = 0; i < n; i++) {
        u32 obj = *(u32 *)(uintptr_t)(MAP_ARRAY_ADDR + 4 * i);
        if (!guest_ptr_ok(obj)) continue;
        u32 vt = *(u32 *)(uintptr_t)obj;
        if (!guest_ptr_ok(vt)) continue;
        u32 f0 = *(u32 *)(uintptr_t)vt;
        const u8 *code = (const u8 *)(uintptr_t)f0;
        if (!guest_ptr_ok(f0) || code[0] != 0xB8 || code[5] != 0xC3) continue;   /* mov eax, desc; ret */
        u32 desc = *(const u32 *)(code + 1);
        const char *cls = (const char *)(uintptr_t)*(u32 *)(uintptr_t)(desc + 4);
        u32 hash = *(u32 *)(uintptr_t)(obj + 0xdc);
        if (hash == 0xFFFFFFFFu) continue;                  /* internal (loading loop) */
        struct mapent *m = &g_maps[g_nmaps++];
        m->obj = obj;
        m->hash = hash;
        m->sub = *(u32 *)(uintptr_t)(obj + 0xe0);
        const char *cn = guest_ptr_ok((u32)(uintptr_t)cls) ? cls : "?";
        m->section = 2;                                      /* developer / test */
        for (size_t k = 0; k < sizeof g_names / sizeof g_names[0]; k++)
            if (!strcmp(cn, g_names[k].cls)) { snprintf(m->name, sizeof m->name, "%s", g_names[k].name); m->section = 0; }
        if (!strncmp(cn, "Map_MiniGame_", 13)) {           /* MR<realm>_<Blk|Sgt|Spx|Spy> */
            const char *w = cn + 13, *who = strstr(w, "Blk") ? "Blink" : strstr(w, "Sgt") ? "Sgt. Byrd" :
                            strstr(w, "Spx") ? "Sparx" : strstr(w, "Spy") ? "Spyro" : "?";
            snprintf(m->name, sizeof m->name, "%c  mini-game: %s", w[2], who);
            m->section = 1;
        }
        if (m->section == 2) { char t[56]; pretty(cn, t, sizeof t); snprintf(m->name, sizeof m->name, "[dev] %s", t); }
        const char *f = edb_file(m->hash);
        snprintf(m->file, sizeof m->file, "%s", f ? f : "-");
        m->shipped = edb_shipped(m->hash);
    }
    for (int i = 0; i < g_nmaps; i++) {                       /* same class twice: number them */
        int dup = 0, idx = 0;
        for (int j = 0; j < g_nmaps; j++) if (j != i && !strcmp(g_maps[j].name, g_maps[i].name)) { dup = 1; if (j < i) idx++; }
        if (dup) { size_t L = strlen(g_maps[i].name); snprintf(g_maps[i].name + L, sizeof g_maps[i].name - L, " #%d", idx + 1); }
    }
    qsort(g_maps, g_nmaps, sizeof g_maps[0], cmp_maps);
}

/* the class name of the map being played ([[map manager +0x78] +0x88] = its Map object), or "" */
const char *dm_map_class(void)
{
    u32 st = *(u32 *)(uintptr_t)(MAP_MANAGER + 0x78);
    u32 m = guest_ptr_ok(st) ? *(u32 *)(uintptr_t)(st + 0x88) : 0;
    u32 vt = guest_ptr_ok(m) ? *(u32 *)(uintptr_t)m : 0;
    u32 f0 = guest_ptr_ok(vt) ? *(u32 *)(uintptr_t)vt : 0;
    if (!guest_ptr_ok(f0)) return "";
    const u8 *code = (const u8 *)(uintptr_t)f0;
    if (code[0] != 0xB8 || code[5] != 0xC3) return "";
    u32 desc = *(const u32 *)(code + 1), cls = guest_ptr_ok(desc) ? *(u32 *)(uintptr_t)(desc + 4) : 0;
    return guest_ptr_ok(cls) ? (const char *)(uintptr_t)cls : "";
}

void debugmenu_dump_maps(void)
{
    scan_maps();

    klog("debug: %d maps registered\n", g_nmaps);
    for (int i = 0; i < g_nmaps; i++)
        klog("  map %2d: %-28s hash %08x sub %08x file %-14s %s\n", i, g_maps[i].name, g_maps[i].hash, g_maps[i].sub,
             g_maps[i].file, g_maps[i].shipped ? "" : "(not on disc)");
}

/* ---- camera research: XBOX_FIND_CAM=frame scans guest memory for the camera position ---- */
float g_dbg_world_m[16]; u32 g_dbg_world_frame;
/* M = P*V*W (c96..c99 = columns). The eye e satisfies rows x, y, w of M*[e,1] = 0. */
static int eye_from_matrix(const float *m, float e[3])
{
    double a[3][4];
    const int rows[3] = { 0, 1, 3 };
    /* the program computes x*c96 + y*c97 + z*c98 + w*c99, so c96..c99 are the columns */
    for (int i = 0; i < 3; i++) for (int j = 0; j < 4; j++) a[i][j] = m[j * 4 + rows[i]];
    for (int c = 0; c < 3; c++) {                       /* Gaussian elimination, partial pivoting */
        int p = c; for (int r = c + 1; r < 3; r++) if (fabs(a[r][c]) > fabs(a[p][c])) p = r;
        if (fabs(a[p][c]) < 1e-12) return -1;
        for (int j = 0; j < 4; j++) { double t = a[c][j]; a[c][j] = a[p][j]; a[p][j] = t; }
        for (int r = 0; r < 3; r++) if (r != c) { double f = a[r][c] / a[c][c]; for (int j = 0; j < 4; j++) a[r][j] -= f * a[c][j]; }
    }
    for (int i = 0; i < 3; i++) e[i] = (float)(-a[i][3] / a[i][i]);
    return 0;
}
static void find_camera(void)
{
    {   /* render context dump: [0x4703f0] + 0x800 .. + 0xa00 */
        u32 rc = *(u32 *)(uintptr_t)0x4703f0u;
        if (guest_ptr_ok(rc)) for (u32 o = 0x800; o < 0xa00; o += 16) {
            const float *v = (const float *)(uintptr_t)(rc + o);
            klog("rctx +%03x: %12g %12g %12g %12g\n", o, v[0], v[1], v[2], v[3]);
        }
    }
    float e[3];
    if (eye_from_matrix(g_dbg_world_m, e)) { klog("camfind: singular matrix\n"); return; }
    klog("camfind: frame %u eye (%g %g %g)\n", g_dbg_world_frame, e[0], e[1], e[2]);
    FILE *f = fopen("/proc/self/maps", "r");
    char line[256]; int hits = 0;
    while (f && fgets(line, sizeof line, f)) {
        unsigned lo, hi; char perm[8];
        if (sscanf(line, "%x-%x %7s", &lo, &hi, perm) != 3 || perm[0] != 'r') continue;
        if (lo < 0x10000 || hi > 0x80000000u) continue;
        for (u32 a = lo; a + 12 <= hi; a += 4) {
            const float *v = (const float *)(uintptr_t)a;
            int ok = 1;
            for (int k = 0; k < 3 && ok; k++) { float d = v[k] - e[k]; if (!(fabsf(d) <= 1e-3f * (1 + fabsf(e[k])))) ok = 0; }
            if (ok && hits++ < 64) klog("camfind: %08x: %g %g %g\n", a, v[0], v[1], v[2]);
        }
    }
    if (f) fclose(f);
    klog("camfind: %d hits\n", hits);
    /* the world transform itself (rows c96..c99), or its transpose, as stored by the game */
    const float *m = g_dbg_world_m;
    klog("camfind: M row0 %g %g %g %g row3 %g %g %g %g\n", m[0], m[1], m[2], m[3], m[12], m[13], m[14], m[15]);
    for (int tr = 0; tr < 2; tr++) {
        float q[4];
        for (int k = 0; k < 4; k++) q[k] = tr ? m[k * 4] : m[k];      /* first row, or first column */
        f = fopen("/proc/self/maps", "r"); int mh = 0;
        while (f && fgets(line, sizeof line, f)) {
            unsigned lo, hi; char perm[8];
            if (sscanf(line, "%x-%x %7s", &lo, &hi, perm) != 3 || perm[0] != 'r') continue;
            if (lo < 0x10000 || hi > 0x80000000u) continue;
            for (u32 a = lo; a + 16 <= hi; a += 4) {
                const float *v = (const float *)(uintptr_t)a;
                int ok = 1;
                for (int k = 0; k < 4 && ok; k++) { float d = v[k] - q[k]; if (!(fabsf(d) <= 1e-4f * (1 + fabsf(q[k])))) ok = 0; }
                if (ok && mh++ < 32) klog("camfind: %s match at %08x\n", tr ? "column" : "row", a);
            }
        }
        if (f) fclose(f);
    }
}

/* ---- cheats ----
 * Inventory/progress block at 0x43cd10: +4 health (0xa0 = full), +8/+0xc gems (the game's own
 * "Lots of Gems" debug command sets both to 10000), +0x28 = 0x43cd38 ability/item bits.
 * Game progress flags live at 0x43ce2c; SetFlag 0xcc370 (thiscall, id 0x44xxxxxx) mirrors five of
 * them into ability bits: 0x44000001 -> 0x1, 0x44000003 -> 0x10, 0x44000025 -> 0x4000,
 * 0x4400009d -> 0x8000, 0x4400009c -> 0x10000. The pause-menu inventory table (0x34e910) lists the
 * other item bits: 0x4, 0x20, 0x40, 0x80 (breaths), 0x200, 0x20000, 0x40000.
 * Bits 0x800/0x1000/0x2000 are timed power-ups and are left alone. */
#define INV_BASE   0x43cd10u
#define FN_SETFLAG 0xcc370u
#define FN_SETHEALTH 0xcf2a0u
#define FLAGS_OBJ  0x43ce2cu
#define PLAYER_OBJ 0x44c704u
typedef u32 (__attribute__((thiscall)) *setflag_fn)(void *self, u32 id);
typedef u32 (__attribute__((thiscall)) *sethealth_fn)(void *self, u32 hp);
static volatile int g_cheat_req;          /* bit 0 abilities, bit 1 gems, bit 2 health */
int g_invincible, g_noclip, g_unlock;
#define UNLOCK_MASK (0x1u | 0x4 | 0x10 | 0x20 | 0x40 | 0x80 | 0x200 | 0x4000 | 0x8000 | 0x10000 | 0x20000 | 0x40000)

u32 debugmenu_play_frame(void);
static void cheats_tick(void)
{
    int req = g_cheat_req; g_cheat_req = 0;
    static int unlocked; static u32 saved_bits; static u8 saved_cnt[20];
    /* five item counters at 0x43cd20 (lives, then the breath-bomb ammo): {count, max, max the shop
     * can raise it to, upgrades bought}; the shop refuses to sell once max == cap */
    u8 *cnt = (u8 *)(uintptr_t)(INV_BASE + 0x10);
    volatile u32 *bits = (u32 *)(uintptr_t)(INV_BASE + 0x28);
    if (*(u32 *)(uintptr_t)PLAYER_OBJ == 0) return;          /* no game running (title, loading) */
    if (g_unlock && !unlocked) { saved_bits = *bits; memcpy(saved_cnt, cnt, 20); unlocked = 1; req |= 1; }
    else if (!g_unlock && unlocked) {
        unlocked = 0;
        *bits = (*bits & ~UNLOCK_MASK) | (saved_bits & UNLOCK_MASK);
        for (int g = 0; g < 20; g += 4) {                     /* back to the save's upgrades, counts clamped */
            cnt[g + 1] = saved_cnt[g + 1]; cnt[g + 3] = saved_cnt[g + 3];
            if ((signed char)cnt[g] > (signed char)cnt[g + 1]) cnt[g] = cnt[g + 1];
        }
        snprintf(g_status, sizeof g_status, "Abilities & items restored to your save's");
    } else if (unlocked) *bits |= UNLOCK_MASK;                /* keep them through map loads */
    if (unlocked)                                             /* shop upgrades maxed, ammo/lives kept full */
        for (int g = 0; g < 20; g += 4) {
            if ((signed char)cnt[g + 2] > (signed char)cnt[g + 1]) cnt[g + 1] = cnt[g + 2];
            if ((signed char)cnt[g] < (signed char)cnt[g + 1]) cnt[g] = cnt[g + 1];
        }
    if (req & 1) {
        static const u32 flags[] = { 0x44000001, 0x44000003, 0x44000025, 0x4400009c, 0x4400009d };
        for (unsigned i = 0; i < sizeof flags / sizeof flags[0]; i++)
            ((setflag_fn)(uintptr_t)FN_SETFLAG)((void *)(uintptr_t)FLAGS_OBJ, flags[i]);
        *bits |= UNLOCK_MASK;
        snprintf(g_status, sizeof g_status, "All abilities and items unlocked");
        char hx[64]; for (int i = 0; i < 20; i++) sprintf(hx + i * 3, " %02x", saved_cnt[i]);
        klog("debug: abilities unlocked, bits now %08x; counters were%s\n", *bits, hx);
    }
    if (req & 2) {
        *(u32 *)(uintptr_t)(INV_BASE + 0x8) = 10000; *(u32 *)(uintptr_t)(INV_BASE + 0xc) = 10000;
        snprintf(g_status, sizeof g_status, "10,000 gems");
    }
    {   /* tests: XBOX_HURT_AT=n (frames after gameplay began) takes some health, XBOX_KILL_AT=n all of it */
        static int hurt_done, kill_done; u32 pf = debugmenu_play_frame();
        const char *e = getenv("XBOX_HURT_AT");
        if (e && !hurt_done && pf >= parse_u32(e, 10)) { hurt_done = 1; u32 hp = *(u32 *)(uintptr_t)(INV_BASE + 0x4);
            ((sethealth_fn)(uintptr_t)FN_SETHEALTH)((void *)(uintptr_t)INV_BASE, hp > 0x30 ? hp - 0x28 : hp); }
        e = getenv("XBOX_KILL_AT");
        if (e && !kill_done && pf >= parse_u32(e, 10)) { kill_done = 1; ((sethealth_fn)(uintptr_t)FN_SETHEALTH)((void *)(uintptr_t)INV_BASE, 0); }
    }
    if ((req & 4) || g_invincible) {
        u32 hp = *(u32 *)(uintptr_t)(INV_BASE + 0x4);
        if (hp < 0xa0) ((sethealth_fn)(uintptr_t)FN_SETHEALTH)((void *)(uintptr_t)INV_BASE, 0xa0);
        if (req & 4) snprintf(g_status, sizeof g_status, "Health restored");
    }
}

/* noclip: the player's position lives in its world matrix (+0x118) and two copies used by the
 * physics (+0x128, +0x4b4); all three are driven directly while the mode is on */
static void noclip_tick(float dt)
{
    static int was; static double pos[3]; static float off4b4[3]; static u32 last_pl;
    u32 pl = *(u32 *)(uintptr_t)PLAYER_OBJ;
    if (pl != last_pl) { last_pl = pl; was = 0; }        /* new player object (map change, character change) */
    if (!g_noclip || !guest_ptr_ok(pl)) { was = 0; return; }
    float *m = (float *)(uintptr_t)(pl + 0x118);
    if (!was) {                    /* +0x4b4 sits a little above the feet (collision centre): keep its offset */
        for (int i = 0; i < 3; i++) { pos[i] = m[i]; off4b4[i] = ((float *)(uintptr_t)(pl + 0x4b4))[i] - m[i]; }
        for (int i = 0; i < 3; i++) if (fabsf(off4b4[i]) > 4.0f) off4b4[i] = 0;
        was = 1;
    }
    extern void freecam_read_controls(float *mx, float *my, float *mz, float *lx, float *ly, int *fast);
    extern int freecam_ground_basis(double fwd[3], double right[3], double up[3]);
    float mx, my, mz, lx, ly; int fast;
    freecam_read_controls(&mx, &my, &mz, &lx, &ly, &fast);
    double f[3], r[3], u[3];
    if (dt > 0.1f) dt = 0.1f;
    if (!freecam_ground_basis(f, r, u)) {
        double sp = (fast ? 30.0 : 8.0) * dt;
        for (int i = 0; i < 3; i++) pos[i] += (f[i] * mz + r[i] * mx + u[i] * my) * sp;
    }
    if (getenv("XBOX_NOCLIP_PROBE")) {           /* research: which fields change between frames */
        static float prev[0x800 / 4]; static u32 pf; extern u32 gl_frame_number(void);
        u32 f0 = parse_u32(getenv("XBOX_NOCLIP_PROBE"), 10), fn = gl_frame_number();
        const float *o = (const float *)(uintptr_t)pl;
        if (fn == f0) {
            klog("probe-dump f%u:", fn);
            for (int i = 0; i < 0x800 / 4; i++)
                if (isfinite(o[i]) && fabsf(o[i]) > 0.3f && fabsf(o[i]) < 200.f) klog(" +%03x=%.3f", i * 4, o[i]);
            klog("\n");
        }
        if (fn >= f0 && fn < f0 + 4 && pf) {
            for (int i = 0; i < 0x800 / 4; i++) {
                float d = o[i] - prev[i];
                if (isfinite(o[i]) && fabsf(o[i]) < 1e6f && fabsf(d) > 1e-4f && fabsf(d) < 1e5f)
                    klog("probe f%u +%03x %.4f (d %.4f)\n", fn, i * 4, o[i], d);
            }
        }
        memcpy(prev, o, sizeof prev); pf = fn;
    }
    if (getenv("XBOX_NOCLIP_LOG")) {
        static int n; extern u32 gl_frame_number(void);
        if (n++ % 30 == 0) klog("noclip f%u pos %.2f %.2f %.2f (before write %.2f %.2f %.2f) v134 %.2f\n", gl_frame_number(),
                                pos[0], pos[1], pos[2], m[0], m[1], m[2], *(float *)(uintptr_t)(pl + 0x134));
    }
    for (int i = 0; i < 3; i++) {
        m[i] = (float)pos[i];
        ((float *)(uintptr_t)(pl + 0x128))[i] = (float)pos[i];
        ((float *)(uintptr_t)(pl + 0x4b4))[i] = (float)pos[i] + off4b4[i];
    }
}

/* ---- character change ----
 * Same route as the GameCube "CharacterChanger" mod (BrinchEbsen), with the Xbox addresses:
 *   gPlayerLoader 0x3498ac {m_LastLoad, m_SoundBank}; PreLoad 0xc42a0 / IsLoaded 0xc1ad0 (thiscall),
 *   XSEItemhandler_Player::ChangePlayer(player, inCutscene) 0xc4c40 (cdecl).
 *   Player handler [0x44c704]: +0x548 type, +0x54c move flags, +0x550 state flags, +0x7ec mode.
 * ChangePlayer resets the setup's map-list index / mini-game id (0x43cd98 / 0x43cd94); the mod
 * restores them afterwards, so do we. */
#define PLAYER_LOADER   0x3498acu
#define FN_PL_PRELOAD   0xc42a0u
#define FN_PL_ISLOADED  0xc1ad0u
#define FN_CHANGEPLAYER 0xc4c40u
#define FN_SETMODE      0xc1be0u
typedef void (__attribute__((thiscall)) *preload_fn)(void *self, u32 player);
typedef int (__attribute__((thiscall)) *isloaded_fn)(void *self, u32 player);
typedef void (*changeplayer_fn)(u32 player, u32 incutscene);
typedef void (__attribute__((thiscall)) *setmode_fn)(void *self, u32 mode, u32 flags, u32 arg);
static const char *g_char_names[] = { "?", "Spyro", "Hunter", "Sparx", "Blink", "Sgt. Byrd", "Ball Gadget", "Ember", "Flame", "Red" };
#define NCHARS 9
extern int g_redchar_want, g_redchar_base; extern u32 g_redchar_anim;
static void redchar_detach(void);
static int g_char_sel = 1;
static volatile int g_char_req;            /* character to switch to, 0 = none */
static u32 g_char_loading, g_char_wait;

static u32 player_type(void)
{
    u32 pl = *(u32 *)(uintptr_t)PLAYER_OBJ;
    u32 t = guest_ptr_ok(pl) ? *(u32 *)(uintptr_t)(pl + 0x548) : 0;
    if (g_redchar_anim && t == (u32)g_redchar_base) return 9;                  /* Red rides on it */
    return t;
}
static int player_can_change(void);
int dm_player_can_change(void) { return player_can_change(); }
static int player_can_change(void)
{
    u32 pl = *(u32 *)(uintptr_t)PLAYER_OBJ;
    if (!guest_ptr_ok(pl)) return 0;
    u32 mode = *(u32 *)(uintptr_t)(pl + 0x7ec);
    if (mode == 3 || mode == 4) return 0;                          /* talking */
    if (*(u32 *)(uintptr_t)(pl + 0x550) & 0x200) return 0;          /* ps_Locked */
    if (*(u32 *)(uintptr_t)(pl + 0x54c) & 0x10) return 0;           /* mf_IgnoreInput (cutscene) */
    return 1;
}
extern int g_start_red; static void load_portcfg(void); u32 debugmenu_play_frame(void);
static void character_tick(void)
{
    {   /* "Start as Red": once the game is running, switch to Red (once per run) */
        static int loaded, done;
        if (!loaded) { loaded = 1; load_portcfg(); if (getenv("XBOX_START_RED")) g_start_red = atoi(getenv("XBOX_START_RED")); }
        if (g_start_red && !done && debugmenu_play_frame() > 30 && !g_char_req && !g_char_loading && player_can_change()) {
            done = 1; if (!g_redchar_want) g_char_req = 9;
        }
    }
    if (g_char_req && !g_char_loading) {
        u32 want = g_char_req; g_char_req = 0;
        if (want == 9) {                                    /* Red: a Spyro-class player wearing Red */
            if (!player_can_change()) { snprintf(g_status, sizeof g_status, "Can't change character right now (in a cutscene or talking?)"); return; }
            int base = 1;
            if (g_redchar_anim && g_redchar_base != base) redchar_detach();
            g_redchar_base = base; g_redchar_want = 1;
            if (*(u32 *)(uintptr_t)(*(u32 *)(uintptr_t)PLAYER_OBJ + 0x548) == (u32)base) return;
            want = base;
        } else {
            g_redchar_want = 0;
            if (want == (u32)g_redchar_base && g_redchar_anim) { snprintf(g_status, sizeof g_status, "Now playing as %s", g_char_names[want]); return; }
        }
        if (!player_can_change()) { snprintf(g_status, sizeof g_status, "Can't change character right now (in a cutscene or talking?)"); return; }
        if (player_type() == want) { snprintf(g_status, sizeof g_status, "Already playing as %s", g_char_names[want]); return; }
        ((preload_fn)(uintptr_t)FN_PL_PRELOAD)((void *)(uintptr_t)PLAYER_LOADER, want);
        g_char_loading = want; g_char_wait = 0;
        snprintf(g_status, sizeof g_status, "Loading %s...", g_char_names[want]);
        klog("debug: preloading player %u\n", want);
        return;
    }
    if (!g_char_loading) return;
    if (!((isloaded_fn)(uintptr_t)FN_PL_ISLOADED)((void *)(uintptr_t)PLAYER_LOADER, g_char_loading)) {
        if (++g_char_wait > 3000) { snprintf(g_status, sizeof g_status, "Loading the character timed out"); g_char_loading = 0; }
        return;
    }
    if (!player_can_change()) return;                               /* wait for a moment it is allowed */
    u32 mi = *(u32 *)(uintptr_t)0x43cd94u, ml = *(u32 *)(uintptr_t)0x43cd98u;
    klog("debug: change player %u -> %u (after %u frames)\n", player_type(), g_char_loading, g_char_wait);
    ((changeplayer_fn)(uintptr_t)FN_CHANGEPLAYER)(g_char_loading, 0);
    *(u32 *)(uintptr_t)0x43cd94u = mi; *(u32 *)(uintptr_t)0x43cd98u = ml;
    snprintf(g_status, sizeof g_status, "Now playing as %s", g_char_names[g_char_loading]);
    g_char_loading = 0;
}

/* ---- squashed ----
 * Player mode 67 "squashed" exists for Spyro / Ember / Flame (handler 0xebc90), Hunter (0x104520)
 * and Blink (0x122130); Sparx, Sgt. Byrd and the Ball Gadget have none. The handler plays anim
 * 0x9000124 and eases the model scale (+0x6d0/6d4/6d8 -> item +0xec/+0xe8/+0xf0) to 0.25 high,
 * 1.25 wide. Animation events: bit 8 in +0x7a8/+0x7ac -> the "peel off" (scale back to 1);
 * bit 1 = animation end -> mode 1 (idle).
 * Animator = [item+0x13c]-4: +0xf8 playback speed (the track advance 0x217680 multiplies dt by it;
 * NPC code sets it to 0 to stop an animation), +0x11c -> current track node (track = node-4,
 * track +0x80 = normalised progress 0..1). The squash animation lies flat from ~0.1 to ~0.5
 * and gets up after that.
 * Hold: once the progress reaches SQUASH_HOLD the speed goes to 0, and events 1|8 are cleared
 * every frame. Off: speed back, bit 8 set -> the game's own peel-off and get-up play. */
#define SQUASH_HOLD 0.30f
int g_squash;
static int has_squash_state(u32 type) { return type == 1 || type == 2 || type == 4 || type == 7 || type == 8; }

static void squash_tick(void)
{
    static int was, held, armed; static float sc[3] = { 1, 1, 1 }, saved_speed = 1.0f; static u32 last_pl, held_anim;
    u32 pl = *(u32 *)(uintptr_t)PLAYER_OBJ;
    if (pl != last_pl) { last_pl = pl; was = 0; held = 0; held_anim = 0; }     /* new player object */
    if (!guest_ptr_ok(pl)) { was = 0; sc[0] = sc[1] = sc[2] = 1; return; }
    u32 item = *(u32 *)(uintptr_t)(pl + 4);
    if (!guest_ptr_ok(item)) return;
    u32 *mode = (u32 *)(uintptr_t)(pl + 0x7ec);
    u32 *ev = (u32 *)(uintptr_t)(pl + 0x7a8), *evacc = (u32 *)(uintptr_t)(pl + 0x7ac);
    u32 type = *(u32 *)(uintptr_t)(pl + 0x548);
    u32 animp = *(u32 *)(uintptr_t)(item + 0x13c);
    u32 anim = guest_ptr_ok(animp) ? animp - 4 : 0;
    u32 node = anim ? *(u32 *)(uintptr_t)(anim + 0x11c) : 0;
    float *progress = guest_ptr_ok(node) ? (float *)(uintptr_t)(node - 4 + 0x80) : NULL;
    float *speed = anim ? (float *)(uintptr_t)(anim + 0xf8) : NULL;
    int raw = getenv("XBOX_SQUASH_RAW") != NULL;
    float hold = getenv("XBOX_SQUASH_HOLD") ? (float)atof(getenv("XBOX_SQUASH_HOLD")) : SQUASH_HOLD;

    if (has_squash_state(type)) {
        if (g_squash) {
            /* (re)enter when standing around (also after a map change or a fall) */
            if (*mode != 0x43 && (!was || *mode == 1 || *mode == 0 || *mode == 16)) {
                if (held && held_anim && held_anim == anim) *speed = saved_speed;
                held = 0; armed = 0;
                ((setmode_fn)(uintptr_t)FN_SETMODE)((void *)(uintptr_t)pl, 0x43, 1, 0);
                klog("debug: squash on, mode now %u\n", *mode);
            }
            was = 1;
            if (*mode == 0x43 && !raw) {
                *ev &= ~9u; *evacc &= ~9u;                     /* no peel-off, no end */
                /* the new animation's track may only start next frame: wait until its progress
                 * is seen below the hold point before holding */
                if (progress && *progress < hold && *(u32 *)(uintptr_t)(item + 0x178) == 0x9000124) armed = 1;
                if (!held && armed && progress && speed && *progress >= hold) {
                    held = 1; held_anim = anim; saved_speed = *speed > 0 ? *speed : 1.0f;
                    klog("debug: squash held at %.3f\n", *progress);
                }
                if (held && speed) *speed = 0;
            }
        } else if (was) {
            was = 0;
            if (held && held_anim == anim && speed) *speed = saved_speed;
            held = 0; held_anim = 0;
            if (*mode == 0x43) *evacc |= 8;                    /* the game's own peel-off and get-up */
        }
        return;
    }
    /* no squashed state (Sparx, Sgt. Byrd, Ball Gadget): just flatten the model */
    if (!g_squash && sc[1] == 1.0f && sc[0] == 1.0f) { was = 0; return; }
    was = g_squash;
    const float tgt[3] = { g_squash ? 1.25f : 1.0f, g_squash ? 0.25f : 1.0f, g_squash ? 1.25f : 1.0f };
    for (int i = 0; i < 3; i++) { sc[i] += (tgt[i] - sc[i]) / 3.0f; if (fabsf(sc[i] - tgt[i]) < 0.002f) sc[i] = tgt[i]; }
    *(float *)(uintptr_t)(item + 0xe8) = sc[0]; *(float *)(uintptr_t)(item + 0xec) = sc[1]; *(float *)(uintptr_t)(item + 0xf0) = sc[2];
    *(u8 *)(uintptr_t)(item + 0x7e) = 0;                     /* matrix needs rebuilding */
}

/* ---- character size ----
 * Model: item +0xe8/+0xec/+0xf0 scale (+0x7e = 0 rebuilds the matrix).
 * Hitbox: physics object [item+0x148] (XItemPhysicsPlayer): collision shape from the model's
 * 0x10000004 data, +0x19c = 0 sphere {+0x160 centre, +0x170 radius + 0.1} (Spyro: (0,1,0), 1.1),
 * 1 capsule {+0x160 point, +0x170 point, +0x180 radius + 0.1}. The 0.1 is kept unscaled (the
 * ground test relies on the sphere reaching 0.1 below the feet), and the size eases in over
 * ~2 s: a sudden jump of the sphere loses the ground.
 * Movement: scaled inside the collision sweep (collide_stub below), so walking, jumping and
 * falling shrink/grow with the hitbox and the collision still checks the real path.
 * Camera: in the camera hook, eye (+0x1c) and look-at (+0x2c) of a camera looking at the player
 * are scaled about the player's feet; near plane (+0x4c) shrinks with him. */
extern u32 g_cam_calls;
#define SIZE_MIN_R 0.15f
float g_size = 1.0f;
RS float g_size_cur = 1.0f, g_size_mult = 1.0f;   /* per player (g_size_mult: Red mode) - shared, Red's 1.3 resized a Ball Gadget player 2's physics */
int g_hide_player;     /* target / current (eased) */
RS u32 g_size_item;
RS float g_move_scale = 1.0f;
RS float g_move_xz = 1.0f;
float g_size_rate = 1.03f;       /* largest size change per frame */
RS float g_flat = 1.0f;             /* the player's height factor (stepped on: 0.15) */
RS int g_freeze_player;             /* input_sdl: no control (flattened) */          /* extra ground speed (Red: a big dragon covers more ground) */
/* XItemPhysicsPlayer collision 0xccd80 (vtable slot, shared by every player class) sweeps the
 * sphere from the last resolved position (phys +0x140) to the item's new position (+0xc8). The
 * frame's whole movement (walking, jumps, gravity, pushes) is scaled right there, before the
 * collision sees it, so the hitbox and the motion stay in proportion. */
void size_collide_c(u32 phys)
{
    if (!g_size_item || (g_move_scale == 1.0f && g_move_xz == 1.0f) || *(u32 *)(uintptr_t)(phys + 4) != g_size_item) return;
    float *pos = (float *)(uintptr_t)(g_size_item + 0xc8), *prev = (float *)(uintptr_t)(phys + 0x140);
    float d[3], d2 = 0;
    for (int k = 0; k < 3; k++) { d[k] = pos[k] - prev[k]; d2 += d[k] * d[k]; }
    if (!(d2 < 4.0f * 4.0f)) return;                   /* a teleport / warp: leave it */
    /* a slow frame (loading hitch, low frame rate) moves the character a long way relative to
     * its hitbox and it can sink through the floor: cap one frame's step at half the sphere */
    float len = sqrtf(d2) * g_move_scale, cap = (g_move_scale + 0.1f) * 0.5f, k2 = g_move_scale;
    if (len > cap) k2 *= cap / len;
    for (int k = 0; k < 3; k++) pos[k] = prev[k] + d[k] * k2 * (k == 1 ? 1.0f : g_move_xz);
}
__asm__(".globl collide_stub\n"
        "collide_stub:\n"
        "  pushal\n"
        "  pushl %ecx\n"
        "  call size_collide_c\n"
        "  addl $4, %esp\n"
        "  popal\n"
        "  subl $0x1d4, %esp\n"
        "  pushl $0xccd86\n"
        "  ret\n");
void collide_stub(void);
static const float g_size_choices[] = { 0.001f, 0.01f, 0.05f, 0.1f, 0.25f, 0.5f, 0.75f, 1.0f, 1.25f, 1.5f, 1.75f, 2.0f };
#define NSIZE (int)(sizeof g_size_choices / sizeof g_size_choices[0])
static void cycle_size(int d)
{
    int i = 0; while (i < NSIZE && g_size_choices[i] != g_size) i++;
    if (i == NSIZE) i = 7;
    i = (i + d + NSIZE) % NSIZE; g_size = g_size_choices[i];
}
static const char *size_name(void)
{
    static char b[32];
    if (g_size < 0.01f) snprintf(b, sizeof b, "%.1f%%", g_size * 100);
    else snprintf(b, sizeof b, g_size == 1.0f ? "%.0f%% (original)" : "%.0f%%", g_size * 100);
    return b;
}
static void size_hooks_install(void)
{
    static int done; if (done) return; done = 1;
    static const u8 want[] = { 0x81, 0xec, 0xd4, 0x01, 0x00, 0x00 };   /* sub esp,0x1d4 */
    extern void hle_patch_jmp(u32 at, void *to);
    if (!memcmp((void *)(uintptr_t)0xccd80, want, sizeof want)) hle_patch_jmp(0xccd80, (void *)collide_stub);
    else klog("debug: collision hook: unexpected bytes\n");
    const char *e = getenv("XBOX_SIZE");
    if (e) { float v = (float)atof(e) / 100.0f; if (v >= 0.001f && v <= 2.0f) g_size = v; }
}
/* per-object "the game's value" tracking: a field holding something other than what we last
 * wrote was set by the game, so it becomes the new original */
struct scaled { u32 obj; float base[8], wrote[8]; };
static const float *g_skin;                 /* per-field constant part kept unscaled (or NULL) */
static const float *g_fmul;                 /* per-field extra factor (or NULL) */
static void scale_fields(struct scaled *t, u32 obj, const u16 *offs, int n, float sc)
{
    const float *skin = g_skin; g_skin = NULL;
    const float *fm = g_fmul; g_fmul = NULL;
    if (t->obj != obj) { t->obj = obj; for (int i = 0; i < n; i++) { t->base[i] = *(float *)(uintptr_t)(obj + offs[i]); t->wrote[i] = t->base[i]; } }
    for (int i = 0; i < n; i++) {
        float *f = (float *)(uintptr_t)(obj + offs[i]);
        if (*f != t->wrote[i]) t->base[i] = *f;
        float k = skin ? skin[i] : 0.0f;
        t->wrote[i] = ((t->base[i] - k) * sc + k) * (fm ? fm[i] : 1.0f);
        *f = t->wrote[i];
    }
}
static void size_tick(void)
{
    size_hooks_install();
    static RS struct scaled model, shape; static RS float applied = 1.0f;
    u32 pl = *(u32 *)(uintptr_t)PLAYER_OBJ;
    u32 item = guest_ptr_ok(pl) ? *(u32 *)(uintptr_t)(pl + 4) : 0;
    if (!guest_ptr_ok(item)) { g_size_item = 0; model.obj = shape.obj = 0; return; }
    if (model.obj != item) g_size_cur = 1.0f;          /* a new player object (level, character): ease in again */
    /* grow / shrink smoothly (a sudden jump of the collision sphere loses the ground) */
    float target = g_size * g_size_mult;
    if (g_size_cur != target) {
        float lim = g_size_rate;                       /* <= 3% a frame (faster for the boss's shrink bolt) */
        float r = target / g_size_cur, step = r > lim ? lim : r < 1 / lim ? 1 / lim : r;
        if ((r > 1 && g_size_cur * step >= target) || (r < 1 && g_size_cur * step <= target) || fabsf(r - 1) < 1e-4f) g_size_cur = target;
        else g_size_cur *= step;
    }
    static RS int was_hidden;
    if (g_size_cur == 1.0f && applied == 1.0f && model.obj == item && !g_hide_player && !was_hidden && g_move_xz == 1.0f && g_flat == 1.0f) { g_size_item = 0; return; }
    was_hidden = g_hide_player;
    applied = g_size_cur;
    g_size_item = item; g_move_scale = getenv("XBOX_SIZE_NOMOVE") ? 1.0f : g_size_cur / g_size_mult;   /* Red moves himself */
    static const u16 m_offs[] = { 0xe8, 0xec, 0xf0 };
    float fmul[3] = { 1.0f + (1.0f - g_flat) * 0.35f, g_flat, 1.0f + (1.0f - g_flat) * 0.35f };   /* flattened: wider */
    g_fmul = fmul;
    scale_fields(&model, item, m_offs, 3, g_hide_player ? 0.0001f : getenv("XBOX_SIZE_NOMODEL") ? 1.0f : g_size_cur / g_size_mult);
    *(u8 *)(uintptr_t)(item + 0x7e) = 0;
    u32 phys = *(u32 *)(uintptr_t)(item + 0x148);
    /* (the Ball Gadget's physics is another class: +0x160 there is a pointer, freed when the level ends) */
    if (guest_ptr_ok(phys) && !getenv("XBOX_SIZE_NOSHAPE") && *(u32 *)(uintptr_t)(pl + 0x548) != 6) {
        static const u16 sph[] = { 0x160, 0x164, 0x168, 0x170 };
        static const u16 cap[] = { 0x160, 0x164, 0x168, 0x170, 0x174, 0x178, 0x180 };
        /* the stored radius is the shape's radius + 0.1, and the ground test relies on the sphere
         * reaching 0.1 below the feet: keep that 0.1 unscaled */
        static const float sph_skin[] = { 0, 0, 0, 0.1f }, cap_skin[] = { 0, 0, 0, 0, 0, 0, 0.1f };
        if (getenv("XBOX_SIZE_ONLYR")) { static const u16 o[] = { 0x170 }; static const float k[] = { 0.1f }; g_skin = k; scale_fields(&shape, phys, o, 1, g_size_cur); }
        else if (getenv("XBOX_SIZE_ONLYC")) { static const u16 o[] = { 0x164 }; scale_fields(&shape, phys, o, 1, g_size_cur); }
        else if (*(u32 *)(uintptr_t)(phys + 0x19c) == 1) { g_skin = cap_skin; scale_fields(&shape, phys, cap, 7, g_size_cur); }
        else {
            /* sphere: scaled, but never thinner than SIZE_MIN_R - a very small ball slips through
             * seams in the level mesh. The centre is lifted to match, so the sphere still ends
             * 0.1 below the feet. */
            g_skin = sph_skin; scale_fields(&shape, phys, sph, 4, g_size_cur);
            float *cy = (float *)(uintptr_t)(phys + 0x164), *rr = (float *)(uintptr_t)(phys + 0x170);
            float body = *rr - 0.1f;
            if (body < SIZE_MIN_R) {
                *cy += SIZE_MIN_R - body; *rr = SIZE_MIN_R + 0.1f;
                shape.wrote[1] = *cy; shape.wrote[3] = *rr;      /* ours, not the game's */
            }
        }
    }
    /* safety net: a resized character can slip through a seam in the floor (seen on slow frames);
     * if it falls far below where it last stood, put it back there */
    {
        static RS u32 gitem; static RS float gpos[3]; static RS int have;
        float *pos = (float *)(uintptr_t)(item + 0xc8);
        u32 md = *(u32 *)(uintptr_t)(pl + 0x7ec);
        if (gitem != item) { gitem = item; have = 0; }
        if (md == 1 || md == 16 || md == 22 || md == 23 || md == 43 || md == 47) { for (int k = 0; k < 3; k++) gpos[k] = pos[k]; have = 1; }
        else if (have && md == 5 && g_size_cur != 1.0f && gpos[1] - pos[1] > (g_size_cur > 1 ? 8.0f * g_size_cur : 8.0f)) {
            float corr[3]; for (int k = 0; k < 3; k++) corr[k] = gpos[k] - pos[k];
            corr[1] += 0.2f;
            float *mtx = (float *)(uintptr_t)(item + 0xb8);
            for (int k = 0; k < 3; k++) { pos[k] += corr[k]; mtx[k] += corr[k]; }
            if (guest_ptr_ok(phys)) {
                static const u16 po[] = { 0x4c, 0x130, 0x140 };
                for (int j = 0; j < 3; j++) for (int k = 0; k < 3; k++) ((float *)(uintptr_t)(phys + po[j]))[k] += corr[k];
                *(float *)(uintptr_t)(phys + 0x7c) = 0;            /* vertical speed */
            }
            klog("debug: size: fell through the floor, put back at %.2f %.2f %.2f\n", gpos[0], gpos[1], gpos[2]);
        }
    }
    if (getenv("XBOX_SIZE_LOG")) {
        static RS u32 n; extern u32 gl_frame_number(void);
        if (n++ % 10 == 0) {
            const float *ps = (const float *)(uintptr_t)(item + 0xc8);
            klog("size f%u pos %.3f %.3f %.3f mode %u shape %g %g r %g type %u\n", gl_frame_number(), ps[0], ps[1], ps[2],
                 *(u32 *)(uintptr_t)(pl + 0x7ec), guest_ptr_ok(phys) ? *(float *)(uintptr_t)(phys + 0x164) : 0,
                 guest_ptr_ok(phys) ? *(float *)(uintptr_t)(phys + 0x168) : 0, guest_ptr_ok(phys) ? *(float *)(uintptr_t)(phys + 0x170) : 0,
                 guest_ptr_ok(phys) ? *(u32 *)(uintptr_t)(phys + 0x19c) : 9);
        }
    }
}
/* ---- modern camera: the right stick turns the view all the way round and up/down, and the view
 * stays where you put it ----
 * The game's follow camera turns only left/right and swings back behind the player by itself. Its
 * own camera keeps doing the work (following, keeping clear of walls): the right stick's left/right
 * sets the yaw we want and the game's camera is steered to it (a right-stick x handed to the game,
 * proportional to the difference), so it stays there instead of swinging back. Up/down tilts the
 * eye about the look-at point (pulled in a little when looking up from low down). After the stick
 * has been left alone for a while and the player is running, the game's own following takes over
 * again, as modern cameras drift in behind. */
int g_modern_cam = 1;
int g_modern_cam_active;                 /* read by input_sdl: take the right stick */
float g_cam_rx, g_cam_ry;                /* the player's right stick -1..1 (from input_sdl) */
float g_cam_rx_out;                      /* right stick x handed to the game (steering its yaw) */
extern int g_noclip;
static void modern_camera(u32 cam, float *eye, float *tgt)
{
    /* more than one camera looks at the player each frame (the view, and a level one at eye
     * height); the view is the one tilted down at the player - only that one is changed */
    static u32 view_cam; static double view_seen;
    static float want_yaw, pitch_off; static int valid, holding; static double lt, last_seen; static float idle_t, calib = 0;
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    double now = ts.tv_sec + ts.tv_nsec * 1e-9;
    if (now - last_seen > 0.3) { g_modern_cam_active = 0; valid = 0; g_cam_rx_out = 0; }
    u32 pl = *(u32 *)(uintptr_t)PLAYER_OBJ;
    if (!g_modern_cam || g_freecam_enabled || g_noclip || !guest_ptr_ok(pl)) return;
    u32 item = *(u32 *)(uintptr_t)(pl + 4);
    if (!guest_ptr_ok(item) || !player_can_change()) return;      /* cutscenes, talking: the game's view */
    float *P = (float *)(uintptr_t)(item + 0xc8);
    float sc = g_size_cur > 1 ? g_size_cur : 1;
    float d[3] = { eye[0] - tgt[0], eye[1] - tgt[1], eye[2] - tgt[2] };
    float dist = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    float tp = (tgt[0] - P[0]) * (tgt[0] - P[0]) + (tgt[2] - P[2]) * (tgt[2] - P[2]);
    if (tp > 8.0f * sc * 8.0f * sc || dist < 0.5f) return;       /* not looking at the player */
    float gyaw = atan2f(d[0], d[2]), gpitch = asinf(d[1] / dist);
    if (cam != view_cam) {
        if (now - view_seen < 0.5 || gpitch < 0.03f) return;
        view_cam = cam; valid = 0;
    }
    view_seen = now;
    float dt = lt ? (float)(now - lt) : 1.0f / 60;
    int upd = dt >= 1e-3f;                                        /* (applied more than once a frame: update once) */
    if (upd) lt = now; else dt = 0;
    if (dt > 0.1f) dt = 0.1f;
    if (!valid) { want_yaw = gyaw; pitch_off = 0; valid = 1; idle_t = 0; holding = 0; }
    last_seen = now; g_modern_cam_active = 1;
    {   extern float g_cam_want_req;               /* Red aimed: settle where he was aiming */
        if (!isnan(g_cam_want_req)) { want_yaw = g_cam_want_req; holding = 1; idle_t = 0; g_cam_want_req = NAN; } }
    if (!calib) calib = getenv("XBOX_CAM_SIGN") ? (float)atof(getenv("XBOX_CAM_SIGN")) : -1.0f;   /* +x turns the game camera to lower yaw */
    /* yaw: the stick sets where we want it; the game's camera is steered there */
    int touching = fabsf(g_cam_rx) > 0.05f;
    if (touching) { want_yaw = gyaw; holding = 1; idle_t = 0; }   /* turning: the game turns it, at its own pace */
    else if (fabsf(g_cam_ry) > 0.05f) { holding = 1; idle_t = 0; }  /* tilting also holds the view */
    else idle_t += dt;
    {
        static float lp[3], v;
        if (upd) { v = sqrtf((P[0] - lp[0]) * (P[0] - lp[0]) + (P[2] - lp[2]) * (P[2] - lp[2])) / dt; memcpy(lp, P, 12); }
        if (holding && idle_t > 1.5f && v > 3.0f * sc && v < 80.0f * sc) holding = 0;   /* running: the game follows again */
    }
    if (!holding) want_yaw = gyaw;
    float dy = want_yaw - gyaw;
    while (dy > 3.14159f) dy -= 6.28318f;
    while (dy < -3.14159f) dy += 6.28318f;
    while (want_yaw > 3.14159f) want_yaw -= 6.28318f;
    while (want_yaw < -3.14159f) want_yaw += 6.28318f;
    float out = touching ? g_cam_rx : holding ? dy * 5.0f * calib : 0;   /* let go: hold it there */
    g_cam_rx_out = out > 1 ? 1 : out < -1 ? -1 : out;
    /* pitch: tilt about the look-at point */
    pitch_off -= g_cam_ry * 1.3f * dt;
    if (!touching && fabsf(g_cam_ry) < 0.05f && !holding && pitch_off != 0) {   /* running: the tilt eases back */
        float k = 0.35f * dt; pitch_off = fabsf(pitch_off) < k ? 0 : pitch_off - (pitch_off > 0 ? k : -k);
    }
    float p = gpitch + pitch_off;
    if (p < -0.25f) { pitch_off += -0.25f - p; p = -0.25f; }
    if (p > 0.95f) { pitch_off -= p - 0.95f; p = 0.95f; }
    if (getenv("XBOX_CAM_LOG") && getenv("XBOX_CAM_LOG")[0] == '2') klog("modcam2 cam %08x rx %.2f gyaw %.2f want %.2f out %.2f eye %.1f %.1f %.1f tgt %.1f %.1f %.1f\n", cam, g_cam_rx, gyaw, want_yaw, g_cam_rx_out, eye[0], eye[1], eye[2], tgt[0], tgt[1], tgt[2]);
    if (fabsf(pitch_off) < 1e-4f) return;
    float r = dist;
    float yaw = gyaw;
    eye[0] = tgt[0] + r * cosf(p) * sinf(yaw);
    eye[1] = tgt[1] + r * sinf(p);
    eye[2] = tgt[2] + r * cosf(p) * cosf(yaw);
    if (getenv("XBOX_CAM_LOG")) { static u32 n; if (n++ % (getenv("XBOX_CAM_LOG")[0] == '2' ? 1 : 20) == 0) klog("modcam gyaw %.2f want %.2f out %.2f pitch %.2f+%.2f hold %d\n", gyaw, want_yaw, g_cam_rx_out, gpitch, pitch_off, holding); }
}

/* camera: called from the camera hook */
static RS float g_fly_cam_w; RS int g_red_flycam;
static float approach(float v, float to, float rate, float dt);
static void size_camera_for(u32 cam, int p2v)
{
    static RS struct { u32 cam; float eye[3], tgt[3], near, weye[3], wtgt[3], wnear; } t[8];
    int i;
    for (i = 0; i < 8 && t[i].cam && t[i].cam != cam; i++) ;
    if (i == 8) i = (int)(g_cam_calls % 8);
    float *eye = (float *)(uintptr_t)(cam + 0x1c), *tgt = (float *)(uintptr_t)(cam + 0x2c), *nr = (float *)(uintptr_t)(cam + 0x4c);
    if (t[i].cam != cam) { t[i].cam = cam; t[i].wnear = -1; t[i].weye[0] = NAN; }
    if (*nr != t[i].wnear) t[i].near = *nr;
    int same = 1;
    for (int k = 0; k < 3; k++) if (eye[k] != t[i].weye[k] || tgt[k] != t[i].wtgt[k]) same = 0;
    if (!same) for (int k = 0; k < 3; k++) { t[i].eye[k] = eye[k]; t[i].tgt[k] = tgt[k]; }
    float s = g_size_item ? g_size_cur : 1.0f;
    float pos[3] = { 0, 0, 0 };
    int follow = 0;
    if (g_size_item && s != 1.0f) {
        for (int k = 0; k < 3; k++) pos[k] = ((float *)(uintptr_t)(g_size_item + 0xc8))[k];
        float d2 = 0; for (int k = 0; k < 3; k++) d2 += (t[i].tgt[k] - pos[k]) * (t[i].tgt[k] - pos[k]);
        follow = d2 < 12.0f * 12.0f;               /* a camera looking at the player (the charge camera looks well ahead of him) */
    }
    extern float g_cam_lift;                       /* Red's high jumps: the view rises with him */
    {   /* flying: the view comes in close behind him (the levels are built round Spyro's size) */
        static RS double lt; struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
        double tn = ts.tv_sec + ts.tv_nsec * 1e-9; float dt = lt ? (float)(tn - lt) : 0; lt = tn; if (dt > 0.1f) dt = 0.1f;
        g_fly_cam_w = approach(g_fly_cam_w, g_red_flycam ? 1.0f : 0.0f, g_red_flycam ? 2.5f : 1.5f, dt); }
    for (int k = 0; k < 3; k++) {
        /* Red: a little further out and higher, so the view looks past his wings and staff */
        extern u32 g_redchar_anim; float kd = g_redchar_anim ? 1.18f : 1.0f, up = g_redchar_anim && k == 1 ? 0.8f * s : 0.0f;
        eye[k] = follow ? pos[k] + (t[i].eye[k] - pos[k]) * s * kd + up * 1.3f : t[i].eye[k];
        tgt[k] = follow ? pos[k] + (t[i].tgt[k] - pos[k]) * s + up : t[i].tgt[k];
        if (follow && k == 1) { eye[k] += g_cam_lift; tgt[k] += g_cam_lift; }
    }
    if (!p2v) modern_camera(cam, eye, tgt);         /* (player 2's half: the game's camera is theirs, turned by their stick) */
    int looks_at_him = 0;                          /* (not the game's other views, e.g. the far one it also runs) */
    if (g_size_item) { const float *Pp = (const float *)(uintptr_t)(g_size_item + 0xc8); float d2 = 0;
        for (int k = 0; k < 3; k++) d2 += (t[i].tgt[k] - Pp[k]) * (t[i].tgt[k] - Pp[k]); looks_at_him = d2 < 25.0f * 25.0f; }
    if (g_fly_cam_w > 0.001f && g_size_item && looks_at_him) {    /* flying: our own chase view, close behind and a little above him */
        static RS float ce[3], ct[3]; static RS int init;
        const float *P = (const float *)(uintptr_t)(g_size_item + 0xc8); float yaw = *(float *)(uintptr_t)(g_size_item + 0xdc);
        float rs = g_size_cur > 0.2f ? g_size_cur : 1.0f, fx = -sinf(yaw), fz = -cosf(yaw);
        extern float g_fly_pitch; float back = 5.5f * rs, high = 2.3f * rs - g_fly_pitch * 1.2f * rs;
        float wt[3] = { P[0] + fx * 2.0f * rs, P[1] + 2.4f * rs, P[2] + fz * 2.0f * rs };
        float we[3] = { P[0] - fx * back, P[1] + high, P[2] - fz * back };
        static RS double lt; struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
        double tn = ts.tv_sec + ts.tv_nsec * 1e-9; float dt = lt ? (float)(tn - lt) : 0; lt = tn; if (dt > 0.1f) dt = 0.1f;
        if (!init || g_fly_cam_w < 0.02f) { memcpy(ce, eye, 12); memcpy(ct, tgt, 12); init = 1; }
        for (int k = 0; k < 3; k++) { ce[k] = approach(ce[k], we[k], 5.0f, dt); ct[k] = approach(ct[k], wt[k], 9.0f, dt); }
        float w = g_fly_cam_w * g_fly_cam_w * (3 - 2 * g_fly_cam_w);
        for (int k = 0; k < 3; k++) { eye[k] += (ce[k] - eye[k]) * w; tgt[k] += (ct[k] - tgt[k]) * w; }
        if (getenv("XBOX_FLY_LOG")) { static RS u32 n; if (n++ % 20 == 0) klog("flycam cam %08x w %.2f P %.1f %.1f %.1f yaw %.2f eye %.1f %.1f %.1f tgt %.1f %.1f %.1f\n", cam, w, P[0], P[1], P[2], yaw, eye[0], eye[1], eye[2], tgt[0], tgt[1], tgt[2]); }
    }
    { void redcast_camera(u32 cam, float *eye, float *tgt); redcast_camera(cam, eye, tgt); }   /* Red aiming: over his shoulder */
    for (int k = 0; k < 3; k++) { t[i].weye[k] = eye[k]; t[i].wtgt[k] = tgt[k]; }
    *nr = t[i].near * (follow && s < 1.0f ? s : 1.0f);
    t[i].wnear = *nr;
}
/* co-op: on player 2's half the view camera starts from player 2's own camera and everything
 * above runs for player 2 (their size, Red's flight and aim views - with their Red state) */
static void size_camera(u32 cam)
{
    int coop_cam_begin(u32 cam); void coop_cam_end(void);
    int p2v = coop_cam_begin(cam);
    size_camera_for(cam, p2v);
    if (p2v) coop_cam_end();
}

/* ---- Red as a playable character (character select) ----
 * Red becomes the player himself: the player stays the game's own Spyro-class player (its
 * physics, collision, camera, moves), and Red's body from bossred.edb is attached to the player
 * item as a second animator - the same thing his boss init (0xbe850, via 0x77260) does to his own
 * item. Spyro's animator stays first in the item's animator list (the player code plays its
 * clips and reads their events there) but is not drawn. Red's animator is driven from the
 * player's state; his boss in 3Z is left alone.
 *   item animator list: head [item+0x13c], node = animator+4, next = [animator+8];
 *   Attach 0x1e5d80 / Detach 0x1e5db0 / Destroy 0x1e5dd0 (thiscall item, animator);
 *   resource for a file 0x1c3fa0(file) (cdecl), animator for an entity 0x1fdfb0(res, entity);
 *   Red's body = entity 0x0300005e in bossred.edb (0x010000db). */
#define RED_FILE      0x010000dbu
#define RED_ENTITY    0x0300005eu
#define CHAR_RED      9            /* "Red": rides a Spyro-class player (glide, charge) */
int g_redchar_base = 1;            /* player type Red rides on: 1 Spyro, 2 Hunter */
#define RED_SIZE      2.2f   /* hitbox / camera vs. the base */
RS int g_redchar_want;                /* character select picked Red */
RS u32 g_redchar_anim, g_redchar_item;
static RS u32 g_redchar_spyro;        /* Spyro's own animator on that item */
static RS float g_redchar_spyro_scale[3];
extern u32 g_red_animator; extern int g_pose_mirror, g_pose_synth;
extern float g_pose_synth_t, g_pose_flap, g_pose_look, g_pose_time;
extern float g_pose_gait_phase, g_pose_gait, g_pose_lean, g_pose_bank, g_pose_crouch, g_pose_breathe;
void redpose_install(void);
enum { SYN_NONE, SYN_TAUNT, SYN_HOVER, SYN_STAFFSPIN };
typedef u32 (*res_get_fn)(u32 file);
typedef u32 (*anim_create_fn)(u32 res, u32 entity);
typedef void (__attribute__((thiscall)) *th1_fn)(void *self, u32 a);
typedef void (__attribute__((thiscall)) *th3_fn)(void *self, u32 a, u32 b, u32 c);
typedef void (__attribute__((thiscall)) *thf_fn)(void *self, float a);
typedef u32 (__attribute__((thiscall)) *fl_load_fn)(void *mgr, u32 *list, u32 mode);
typedef u32 (__attribute__((thiscall)) *fl_isloaded_fn)(void *mgr, u32 *list);
typedef u32 (__attribute__((thiscall)) *animplay_fn)(void *animator, u32 cur, u32 next, u32 fallback);
static u32 g_red_files[3] = { RED_FILE, 0x08000001u, 0 };   /* the file and its section 1 (the animations) */
extern u32 gl_frame_number(void);
u32 debugmenu_play_frame(void);

static float anim_progress(u32 an)
{
    u32 node = *(u32 *)(uintptr_t)(an + 0x120);   /* newest track (list tail) */
    return guest_ptr_ok(node) ? *(float *)(uintptr_t)(node - 4 + 0x80) : 0.0f;
}
static u32 anim_track(u32 an)         /* the newest track (list tail) - 0 if none */
{
    u32 node = *(u32 *)(uintptr_t)(an + 0x120);
    return guest_ptr_ok(node) ? node - 4 : 0;
}
/* a track's play position: +0x80 is the clip position 0..1 that the advance (0x217680) moves on by
 * speed * dt each frame and the sampler reads, so writing it scrubs the clip. +0x20 flags: bit 0
 * loops at the end; otherwise reaching 1 runs the clip's end (back to his idle). */
static void anim_set_progress(u32 an, float p)
{
    u32 tr = anim_track(an);
    if (getenv("XBOX_SCRUB_TRACE")) { static RS u32 last; if (tr != last) { last = tr; klog("scrub write track %08x (node %08x head %08x)\n", tr,
        *(u32 *)(uintptr_t)(an + 0x120), *(u32 *)(uintptr_t)(an + 0x11c)); } }
    /* only while the clip is the animator's single track: just after a PlayAnim the list tail is a
     * short-lived blend entry, smaller than a track, and +0x80 there is another object's memory */
    if (tr && *(u32 *)(uintptr_t)(an + 0x120) == *(u32 *)(uintptr_t)(an + 0x11c)) *(float *)(uintptr_t)(tr + 0x80) = p;
}
static RS u32 g_redchar_cur;          /* clip playing on Red's animator */
RS u32 g_redchar_cur_pub; RS float g_red_progress_pub;   /* (for the sound log) */
static void redchar_play(u32 anim, int restart)
{
    u32 an = g_redchar_anim;
    if (!an) return;
    if (restart && g_redchar_cur == anim) {
        ((animplay_fn)(uintptr_t)0x1fd240u)((void *)(uintptr_t)an, g_redchar_cur, 0x09000004u, 0x9000004u);
        g_redchar_cur = 0x09000004u;
    }
    if (g_redchar_cur == anim) return;
    /* his clips go back to his idle by themselves when they end (their anim-mode scripts);
     * asking for the idle part-way through one leaves it frozen on its last frame instead */
    if (anim == 0x09000004u && g_redchar_cur) { g_redchar_cur = anim; return; }
    ((animplay_fn)(uintptr_t)0x1fd240u)((void *)(uintptr_t)an, g_redchar_cur, anim, 0x9000004u);
    g_redchar_cur = anim;
}
/* Spyro's animator stays (the player's moves run on it) but isn't drawn: animators draw only with
 * bit 0 of +0x10 set (item draw loop 0x1e6f60). It is scaled up so that whatever comes out of his
 * bones (breath, sparks) comes from about Red's head rather than his feet. */
#define SPYRO_UNDER_RED_SCALE 3.5f
static void spyro_visible(int on)
{
    u32 a = g_redchar_spyro;
    if (!a) return;
    float *sc = (float *)(uintptr_t)(a + 0xe8);
    if (on) { for (int k = 0; k < 3; k++) sc[k] = g_redchar_spyro_scale[k]; *(u8 *)(uintptr_t)(a + 0x10) |= 1; }
    else    { for (int k = 0; k < 3; k++) sc[k] = g_redchar_spyro_scale[k] * SPYRO_UNDER_RED_SCALE; *(u8 *)(uintptr_t)(a + 0x10) &= ~1; }
    *(u8 *)(uintptr_t)(a + 0x7e) = 0;
}
extern float g_move_xz;
static void redchar_detach(void)
{
    g_move_xz = 1.0f;
    u32 pl = *(u32 *)(uintptr_t)PLAYER_OBJ;
    if (g_redchar_anim && guest_ptr_ok(pl) && *(u32 *)(uintptr_t)(pl + 4) == g_redchar_item) {
        spyro_visible(1);
        ((th1_fn)(uintptr_t)0x1e5dd0u)((void *)(uintptr_t)g_redchar_item, g_redchar_anim);
        klog("redchar: Red's body removed\n");
    }
    g_redchar_anim = g_redchar_item = g_redchar_spyro = 0; g_red_animator = 0; g_redchar_cur = 0;
    g_size_mult = 1.0f;
}
static int redchar_attach(u32 pl)
{
    static RS u32 asked_for;
    if (asked_for != pl) {
        asked_for = pl;
        u32 r = ((fl_load_fn)(uintptr_t)0x1f860u)((void *)(uintptr_t)0x4375a0u, g_red_files, 1);
        klog("redchar: load bossred.edb -> %u\n", r);
    }
    if (!((fl_isloaded_fn)(uintptr_t)0x1f990u)((void *)(uintptr_t)0x4375a0u, g_red_files)) return 0;
    u32 item = *(u32 *)(uintptr_t)(pl + 4);
    u32 head = *(u32 *)(uintptr_t)(item + 0x13c);
    if (!guest_ptr_ok(head)) return 0;
    u32 res = ((res_get_fn)(uintptr_t)0x1c3fa0u)(RED_FILE);
    if (!res) { klog("redchar: no resource for bossred.edb\n"); return 0; }
    u32 an = ((anim_create_fn)(uintptr_t)0x1fdfb0u)(res, RED_ENTITY);
    if (!an) { klog("redchar: couldn't make Red's body\n"); return 0; }
    /* what Red's own init does with his body (0xbe885..0xbe8e3) */
    ((th1_fn)(uintptr_t)0x1fdf20u)((void *)(uintptr_t)an, 2);
    for (u32 l = 0; l < 3; l++) ((th3_fn)(uintptr_t)0x1fa000u)((void *)(uintptr_t)an, l, 6, 1);
    ((thf_fn)(uintptr_t)0x1e7810u)((void *)(uintptr_t)an, 60.0f);
    *(u8 *)(uintptr_t)(an + 0x7c) = 0x13;
    float *sc = (float *)(uintptr_t)(an + 0xe8);
    sc[0] = sc[1] = sc[2] = 1.5f; sc[3] = 0;
    if (guest_ptr_ok(*(u32 *)(uintptr_t)(an + 0x190))) ((th1_fn)(uintptr_t)0x1fd3e0u)((void *)(uintptr_t)*(u32 *)(uintptr_t)(an + 0x190), 1);
    ((th1_fn)(uintptr_t)0x1e5d80u)((void *)(uintptr_t)item, an);
    g_redchar_spyro = head - 4;
    memcpy(g_redchar_spyro_scale, (void *)(uintptr_t)(g_redchar_spyro + 0xe8), 12);
    klog("redchar: Red's body %08x on player item %08x (Spyro's %08x, flags %02x / %02x, scale %g)\n", an, item,
         g_redchar_spyro, *(u8 *)(uintptr_t)(g_redchar_spyro + 0x7c), *(u8 *)(uintptr_t)(an + 0x7c), g_redchar_spyro_scale[0]);
    g_redchar_anim = an; g_redchar_item = item; g_redchar_cur = 0;
    g_red_animator = an;
    spyro_visible(0);
    return 1;
}

/* Red's animation for what the player is doing.
 *  - his own clips (bossred.edb) for gestures and the charge; they can't be cut short in the
 *    engine, so when you move during one the pose layer fades it out to his idle underneath
 *    (base override) - he never gets stuck in a clip;
 *  - made for him (redpose.c): stepping walk/run, take-off, glide, falling, double-jump wing
 *    stroke, landing squat, staff swings, taunt; state changes cross-fade;
 *  - square (X) is a staff swing instead of breath: the game gets a short charge tap instead,
 *    so the swing hits things. */
RS int g_red_x;                        /* input_sdl: the real X (the game doesn't get it) */
RS int g_red_melee_b;                  /* input_sdl: frames of B to give the game (the swing's hit) */
extern float g_pose_rise, g_pose_glide, g_pose_fall, g_pose_kick, g_pose_melee_t, g_pose_base_w, g_pose_hop;
extern int g_pose_melee_side, g_pose_capture_idle;
extern float g_pose_charge; static int g_pose_charge_on;
void redpose_xfade(float secs);
static float approach(float v, float to, float rate, float dt) { float k = rate * dt; if (k > 1) k = 1; return v + (to - v) * k; }
RS float g_cam_lift;
/* his clips for each move (from the recorded gallery; see NOTES) */
#define RED_CAST_CLIP 0x090001c8u        /* his ice-beam cast: staff thrust forward */
#define RED_CAST_HOLD_START 0.40f
#define RED_CAST_HOLD_END   0.62f
#define RED_COMBO1 0x090001c7u            /* overhead swing */
#define RED_COMBO2 0x090001cbu            /* swing back the other way */
#define RED_COMBO3 0x09000194u            /* staff lunge, the finisher */
#define RED_HURT_CLIP 0x090001a1u         /* rears back, wings flung up */
#define RED_HURT_FROM 0.12f
#define RED_HURT_SPAN 0.62f
#define RED_GETUP_CLIP 0x09000011u        /* from lying flat back onto his feet */
#define RED_IDLE_VOICES { 0x236, 0x237, 0x232 }   /* his long voiced lines (laughs, taunts) */
#define RED_GESTURES { 0x09000189u, 0x0900018au, 0x0900018eu, 0x0900018fu, 0x09000190u, 0x09000191u, 0x0900000du, 0x090001b3u }
RS int g_red_hide_a, g_red_pass_x, g_raw_a_pub, g_raw_b_pub, g_red_hide_b, g_raw_rt_pub, g_raw_y_pub;
RS int g_red_spinning, g_red_pounding, g_red_flying, g_red_shield;
RS float g_fly_bank, g_fly_pitch;   /* his tail-spin / ground pound (turn assist stays out) */
static int g_bake_idx_pub(void) { extern int g_bake_idx; return g_bake_idx; }
/* ---- recording his clips (redpose.c plays them back): once, then kept in redclips.bin ---- */
static const struct { u32 id; float frames; } g_bake_list[] = {
    { 0x090001a4, 24 }, { 0x090001a3, 25 }, { 0x090001ba, 25 }, { 0x090001b9, 20 },
    { 0x09000199, 80 }, { 0x090001c7, 41 }, { 0x090001c8, 41 }, { 0x090001c9, 41 }, { 0x090001ca, 41 },
    { 0x090001cb, 41 }, { 0x090001cc, 42 }, { 0x09000194, 54 }, { 0x09000195, 50 }, { 0x0900000d, 40 },
    { 0x09000189, 45 }, { 0x0900018a, 45 }, { 0x0900018b, 40 }, { 0x0900018e, 60 }, { 0x0900018f, 40 },
    { 0x09000190, 35 }, { 0x09000191, 41 }, { 0x0900019a, 47 }, { 0x0900019f, 54 }, { 0x090001a0, 65 },
    { 0x090001a1, 45 }, { 0x090001a2, 60 }, { 0x090001a5, 24 }, { 0x090001b2, 64 }, { 0x090001b3, 48 },
    { 0x090001b7, 64 }, { 0x090001f2, 111 }, { 0x09000054, 65 }, { 0x09000011, 24 }, { 0x090001b8, 35 },
};
#define NBAKE (int)(sizeof g_bake_list / sizeof g_bake_list[0])
int g_bake_done;                         /* 1 = his clips are recorded (or loaded) */
int g_baking;                            /* recording in progress (his idle stands in, meanwhile) */
static void redchar_play(u32 anim, int restart);
int red_ctx(void);
int g_bake_owner;                        /* whose Red is being recorded (redpose.c records only his) */
static void bake_tick(int calm_enough)
{
    extern int g_bake_idx, g_bake_got; extern u32 g_bake_res;
    void redclip_begin(int i, u32 id, float frames); void redclip_save(void); int redclip_load(void);
    static int state, k, phase, i, tries; static u32 prev_res; static float waited;
    static int owner;                     /* (both players Red: the one recording, the other waits) */
    if (state == 2) return;
    if (state == 1 && owner != red_ctx()) return;
    if (state == 0) {
        if (redclip_load()) { state = 2; g_bake_done = 1; return; }
        if (!calm_enough) return;
        state = 1; k = 0; phase = 0; g_baking = 1; owner = g_bake_owner = red_ctx();
        klog("redclips: recording his clips (once)\n");
    }
    u32 an = g_redchar_anim;
    if (!an) return;
    u32 head = *(u32 *)(uintptr_t)(an + 0x11c), tail = *(u32 *)(uintptr_t)(an + 0x120);
    u32 res = tail && head == tail ? *(u32 *)(uintptr_t)(tail - 4 + 0x18) : 0;
    int n = g_bake_list[k].frames > 40 ? 40 : (int)g_bake_list[k].frames;
    switch (phase) {
    case 0:
        prev_res = res; redchar_play(g_bake_list[k].id, 0); phase = 1; waited = 0; break;
    case 4:
        waited += 1;
        if (res && res != prev_res) { g_bake_res = res; phase = 2; }
        else if (waited > 240) phase = 3;
        break;
    case 1:
        waited += 1;
        if (res && res != prev_res) { g_bake_res = res; redclip_begin(k, g_bake_list[k].id, g_bake_list[k].frames); i = 0; tries = 0; phase = 2; }
        else if (waited > 240) { klog("redclips: %08x didn't start, skipped\n", g_bake_list[k].id); phase = 3; }
        break;
    case 2:
        if (res != g_bake_res) {                  /* its own script moved on to another clip: start it again, carry on */
            if (++tries > 4) { phase = 3; break; }
            prev_res = res; g_redchar_cur = 0x09000004u; redchar_play(g_bake_list[k].id, 0); phase = 4; waited = 0; break;
        }
        {   /* aim at the next even step; the engine moves the position on by one frame's worth
             * before it samples, so that step (measured) is taken off what is written */
            static float written, adv; static int have;
            if (i == 0) { adv = 0; have = 0; }
            if (have && res) { float a = *(float *)(uintptr_t)(tail - 4 + 0x80) - written; if (a > 0 && a < 0.5f) adv = a; }
            extern float g_bake_lastp;
            float target = (float)g_bake_got / (n - 1) * 0.96f;
            if (g_bake_got && target < g_bake_lastp + 0.5f * 0.96f / (n - 1)) target = g_bake_lastp + 0.5f * 0.96f / (n - 1);
            if (g_bake_got >= n || target > 0.985f || ++i > 3 * n + 60) { phase = 3; break; }
            written = target - adv; if (written < -0.3f) written = -0.3f;
            anim_set_progress(an, written); have = 1;
        }
        break;
    }
    if (phase == 3) {
        g_bake_idx = -1;
        if (++k >= NBAKE) {                      /* the last one (his landing) runs out into his idle */
            state = 2; g_bake_done = 1; g_baking = 0; redclip_save();
            snprintf(g_status, sizeof g_status, "Red is ready");
            return;
        }
        phase = 0;
    }
}

extern struct pad_state_dm { u16 buttons; u8 analog[8]; s16 lx, ly, rx, ry; } g_pads[4];
void red_sound_at(u32 id, u32 item, float vol, float pan);
int red_ctx(void);
static int red_pad_slot(void) { u8 coop_port_of(int k); return coop_port_of(red_ctx()) & 3; }   /* this Red's pad */
static void redchar_animate(u32 pl)
{
    u32 item = *(u32 *)(uintptr_t)(pl + 4);
    float *pos = (float *)(uintptr_t)(item + 0xc8);
    u32 md = *(u32 *)(uintptr_t)(pl + 0x7ec);
    static RS double lt; struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    double t = ts.tv_sec + ts.tv_nsec * 1e-9; float dt = lt ? (float)(t - lt) : 1.0f / 60; lt = t;
    if (dt < 0.002f) dt = 0.002f;
    if (dt > 0.1f) dt = 0.1f;
    static RS float last[3], hs, vs; static RS u32 last_item;
    if (last_item != item) { last_item = item; memcpy(last, pos, 12); }
    float v[3] = { pos[0] - last[0], pos[1] - last[1], pos[2] - last[2] }; memcpy(last, pos, 12);
    float h = sqrtf(v[0] * v[0] + v[2] * v[2]) / dt, vv = v[1] / dt;
    if (h > 200) h = 0;
    if (vv > 200 || vv < -200) vv = 0;
    {   /* research: XBOX_FAKE_HS="from:to:speed,..." (frames after gameplay began) - a treadmill: the
         * pose layer sees that ground speed while he stands still (gait filmed side-on, no walls) */
        const char *e = getenv("XBOX_FAKE_HS");
        if (e) { u32 pf = debugmenu_play_frame(); char buf[256]; snprintf(buf, sizeof buf, "%s", e); char *sv = NULL;
                 for (char *t = strtok_r(buf, ",", &sv); t; t = strtok_r(NULL, ",", &sv)) { unsigned a, b; float sp;
                     if (sscanf(t, "%u:%u:%f", &a, &b, &sp) == 3 && pf >= a && pf < b) h = sp; } }
    }
    hs = approach(hs, h, 10, dt); vs = approach(vs, vv, 12, dt);
    /* air: 5 fall, 8 jump, 11 Hunter's flip, 9/12/104 seen mid-air; 10 is Spyro's double-jump/
     * glide and also the moment of landing, so it only counts while he's moving up or down */
    int air = md == 5 || md == 8 || md == 9 || md == 11 || md == 12 || md == 104 || (md == 10 && fabsf(vs) > 0.5f);
    static RS float air_t, air_t_last; air_t = air ? air_t + dt : 0;
    {   /* snappier control: he turns to where the stick points much faster than Spyro's easing
         * (the game keeps moving him along his facing), on the ground and in the air */
        static RS float turn = -1; if (turn < 0) turn = getenv("XBOX_RED_TURN") ? atof(getenv("XBOX_RED_TURN")) : 10.0f;
        extern int g_red_aiming;
        float sx = g_pads[red_pad_slot()].lx / 32767.0f, sy = g_pads[red_pad_slot()].ly / 32767.0f, mag = sqrtf(sx * sx + sy * sy);
        u32 rc = *(u32 *)(uintptr_t)0x4703f0u;
        int busy = md == 29 || md == 17 || md == 67 || md == 3 || md == 4 || md == 26;
        if (mag > 0.35f && turn > 0 && guest_ptr_ok(rc) && !busy && !g_red_aiming && !g_red_spinning && !g_red_pounding && player_can_change()) {
            /* the render context's +0x944 points from the view back toward the camera (measured:
             * stick forward asked for the opposite of the way he ran, sideways was right) - the
             * view's forward is its negation; earlier this was taken as forward, so he was turned
             * half way round against the game's own turning on every forward/back push (the jank) */
            const float *cd = (const float *)(uintptr_t)(rc + 0x944);
            float fx = -cd[0], fz = -cd[2], fl = sqrtf(fx * fx + fz * fz);
            if (fl > 1e-3f) {
                fx /= fl; fz /= fl;
                float rx = fz, rz = -fx;                         /* screen right */
                float wx = fx * sy + rx * sx, wz = fz * sy + rz * sx;
                float want = atan2f(-wx, -wz);                   /* forward = (-sin yaw, -cos yaw) */
                float *yaw = (float *)(uintptr_t)(item + 0xdc), d = want - *yaw;
                while (d > 3.14159f) d -= 6.28318f;
                while (d < -3.14159f) d += 6.28318f;
                float k = turn * dt * (air ? 0.5f : 1.0f); if (k > 1) k = 1;
                if (fabsf(d) < 2.6f) { *yaw += d * k; *(u8 *)(uintptr_t)(item + 0x7e) = 0; }
                if (getenv("XBOX_TURN_LOG")) klog("turn want %.2f yaw %.2f d %.2f md %u\n", want, *yaw, d, md);
            }
        }
    }
    static RS int g_red_dj, g_red_gliding, g_red_dj_used;
    int g_pads_raw_a = g_raw_a_pub;
    {   /* a dragon's jump: vertical speed (physics +0x7c, + = up) set on take-off and on the
         * double jump; while A is held after the double jump he glides, sinking slowly */
        static RS int was; static RS float apex, y0;
        u32 ph = *(u32 *)(uintptr_t)(item + 0x148);
        float *vy = guest_ptr_ok(ph) ? (float *)(uintptr_t)(ph + 0x7c) : NULL;
        static RS float j1 = -1, j2;
        if (j1 < 0) { j1 = getenv("XBOX_RED_JUMP") ? atof(getenv("XBOX_RED_JUMP")) : 13.0f;
                      j2 = getenv("XBOX_RED_JUMP2") ? atof(getenv("XBOX_RED_JUMP2")) : 12.0f; }
        if (vy && getenv("XBOX_RED_VY") && (air || was)) klog("vy md %u vy %.2f y %.2f A %d\n", md, *vy, pos[1], g_pads[0].analog[0] > 128);
        int a_held = g_raw_a_pub;
        static RS int a_was, dj_used, gliding;
        g_red_dj_used = dj_used;
        if (!air) dj_used = gliding = 0;
        if (vy) {
            /* the game holds its take-off speed (6.78) while A is held (for a few frames), then
             * gravity takes it from the stored speed: on the first frame after the hold the speed
             * is raised by (j1 - 6.78), so the whole arc is a much bigger one */
            static RS int boosted;
            if (!air) boosted = 0;
            if (md == 8 && !boosted && !dj_used && *vy > 0 && *vy < 6.77f) { *vy += j1 - 6.78f; boosted = 1; }
            /* a second press in the air: a second, wing-powered jump */
            if (air && air_t > 0.08f && a_held && !a_was && !dj_used) {
                dj_used = 1; *vy = j2; g_red_dj = 1;
            }
            /* A held on the way down: the game's own glide (it holds the fall at 4/s) */
            gliding = air && a_held && *vy < 0;
        }
        g_red_gliding = gliding;
        a_was = a_held;
        if (air && !was) { apex = pos[1]; y0 = pos[1]; }
        /* the game's camera barely rises for a jump (Spyro's are small): lift it with him, and
         * ease it back after landing while the game's own camera catches up */
        extern int g_red_flying;
        extern int g_red_flycam;
        if (g_red_flycam) { y0 = pos[1]; g_cam_lift = 0; }   /* flying has its own camera; after it, lift from where he lets go */
        else g_cam_lift = air ? approach(g_cam_lift, 0.95f * (pos[1] - y0 > 0 ? pos[1] - y0 : 0), 12, dt)
                         : approach(g_cam_lift, 0, 2.5f, dt);
        if (air && pos[1] > apex) apex = pos[1];
        if (!air && was && getenv("XBOX_RED_LOG")) klog("redchar landed: rose %.1f, air %.2f s\n", apex - y0, air_t_last);
        air_t_last = air_t;
        was = air;
    }

    /* ---- the charge (hold B) is his dash. A charge started by a staff swing's hit (the B tap)
     * is part of the swing, not a dash. */
    static RS u32 prev_md;
    static RS float since_swing = 9;
    static RS int from_swing;
    int chg = md == 29 || md == 17;
    if (chg && !(prev_md == 29 || prev_md == 17)) from_swing = since_swing < 0.5f;
    if (!chg) from_swing = 0;
    int charging = chg && !from_swing && !air;
    (void)g_pose_charge_on; g_pose_charge = 0;
    prev_md = md;
    extern void red_sound(u32 id, float vol, float pan); extern void red_sound_at(u32 id, u32 item, float vol, float pan); extern void sfx_swoosh(float hz, float dur, float vol, float pan);

    /* ---- his clips: recorded once (bake_tick), then played from the recording ----
     * The engine only ever plays his idle now, so nothing of the engine's can glitch or stick.
     * Two layers: L0 how he moves (walk, dash, take-off, flying, landing), L1 what he does
     * (staff combo, magic, getting hit, collapsing, idle gestures). */
    static RS float calm;
    bake_tick(calm > 1.2f);                   /* (after his idle has been captured, at 0.6 s) */
    extern struct rclayer { int clip; float u, w; int mask; int nolift; } g_rcl[3];
    int redclip_find(u32 id); float redclip_secs(int i);
    enum { LM_FULL, LM_UPPER, LM_LOWER };
#define CL(id) redclip_find(id)
    static RS int c_dash = -2, c_walk, c_take, c_fly, c_land, c_cast, c_combo[3], c_hurt, c_collapse, c_getup, c_gest[10], ngest;
    if (g_bake_done && c_dash == -2) {
        c_dash = CL(0x090001a4); c_walk = CL(0x090001a3); c_take = CL(0x090001ba); c_fly = CL(0x090001b9); c_land = CL(0x090001b8);
        c_cast = CL(RED_CAST_CLIP); c_combo[0] = CL(RED_COMBO1); c_combo[1] = CL(RED_COMBO2); c_combo[2] = CL(RED_COMBO3);
        c_hurt = CL(RED_HURT_CLIP); c_collapse = CL(0x09000054); c_getup = CL(RED_GETUP_CLIP);
        static const u32 gl[] = RED_GESTURES;
        for (unsigned i = 0; i < sizeof gl / sizeof gl[0]; i++) { int c = CL(gl[i]); if (c >= 0) c_gest[ngest++] = c; }
        klog("redclips: dash %d walk %d take-off %d fly %d land %d cast %d combo %d %d %d hurt %d collapse %d gestures %d\n",
             c_dash, c_walk, c_take, c_fly, c_land, c_cast, c_combo[0], c_combo[1], c_combo[2], c_hurt, c_collapse, ngest);
    }
    int clips = g_bake_done && c_dash != -2;

    /* L0: how he moves */
    enum { LS_NONE, LS_WALK, LS_DASH, LS_TAKEOFF, LS_AIR, LS_LAND };
    static RS int ls; static RS float ls_u, ls_w, ls_t;
    static RS int was_air0; static RS float air_dur;
    int ls_want = LS_NONE;
    /* a plain jump is a hop, not a take-off: his take-off clip ends in level flight (body flat, staff
     * thrust ahead), which read as a dive on every little jump - so the hop uses the made air poses
     * (springing up, then dropping with wings half open) and the clip is kept for the wing-powered
     * double jump, flying and long drops. XBOX_OLD_AIR=1 = the clip on every jump. */
    static RS int old_air = -1; if (old_air < 0) old_air = getenv("XBOX_OLD_AIR") != NULL;
    static RS int flying; static RS float stamina = 3.5f;
    if (air && !was_air0 && md == 8) { red_sound_at(0x234, item, 0.65f, 0.0f);
        if (clips && old_air) { ls = LS_TAKEOFF; ls_u = 0.18f; ls_t = 0; redpose_xfade(0.1f); }
        else { g_pose_hop = 1; redpose_xfade(0.1f); } }
    if (g_red_dj) {                                                  /* double jump: the take-off stroke again */
        g_red_dj = 0; sfx_swoosh(380.0f, 0.4f, 0.75f, 0.0f); red_sound_at(0x239, item, 0.7f, 0.0f);
        if (clips) { ls = LS_TAKEOFF; ls_u = 0.25f; ls_t = 0; redpose_xfade(0.1f); } else g_pose_kick = 1;
    }
    if (air) air_dur += dt;
    if (!air && was_air0 && air_dur > 0.3f) {
        sfx_swoosh(160.0f, 0.18f, 0.5f, 0.0f);
        if (clips && c_land >= 0) { ls = LS_LAND; ls_u = 0.5f; ls_t = 0; redpose_xfade(0.08f); } else { g_pose_crouch = 1.0f; redpose_xfade(0.1f); }
    }
    if (!air) air_dur = 0;
    was_air0 = air;
    /* moves Spyro has that Red has no animation for (poles, swimming, ledges - found as they come):
     * off the ground he hovers on slow wing beats; each new one is noted in the log */
    static const u8 known_md[] = { 67, 1, 3, 4, 5, 6, 8, 9, 10, 11, 12, 16, 17, 19, 20, 22, 26, 29, 34, 61, 104 };
    int known = 0; for (unsigned i = 0; i < sizeof known_md; i++) if (md == known_md[i]) known = 1;
    int odd_move = !known && fabsf(vs) > 0.3f;
    if (!known) { static u32 seen[16]; int k; for (k = 0; k < 16 && seen[k] && seen[k] != md + 1; k++) ;
                  if (k < 16 && !seen[k]) { seen[k] = md + 1; klog("red: player move %u has no animation of his (hovering in it)\n", md); } }
    if (charging) ls_want = LS_DASH;
    else if (odd_move) ls_want = LS_AIR;
    else if (air) ls_want = ls == LS_TAKEOFF && ls_u < 0.8f ? LS_TAKEOFF
                          : old_air || flying || (g_red_gliding && air_dur > 0.15f) || vs < -16.0f || air_dur > 2.6f ? LS_AIR : LS_NONE;
    else if (ls == LS_LAND && ls_u < 0.97f && hs < 4.0f) ls_want = LS_LAND;
    /* walking and running: his upright stepping (made for him) - his own walk clip is a hunched
     * prowl that looks frantic at Spyro's running pace */
    if (ls_want == LS_DASH && ls != LS_DASH) red_sound_at(0x233, item, 0.8f, 0.0f);
    if (clips && ls_want != ls) {
        int smooth = (ls == LS_NONE && ls_want == LS_WALK) || (ls == LS_WALK && ls_want == LS_NONE);
        if (!smooth) redpose_xfade(ls_want == LS_AIR && ls == LS_TAKEOFF ? 0.2f : 0.15f);
        if (ls_want != LS_WALK || ls != LS_NONE) ls_u = 0;
        ls = ls_want; ls_t = 0;
    }
    ls_t += dt;
    if (getenv("XBOX_LS_LOG") && (air || ls != LS_NONE)) klog("ls f%u md %u air %d ls %d u %.2f air_dur %.2f fly %d glide %d odd %d hop %.2f vs %.1f\n",
        debugmenu_play_frame(), md, air, ls, ls_u, air_dur, flying, g_red_gliding, odd_move, g_pose_hop, vs);
    int c0 = -1; float rate = 1, target_w = 1; int nolift = 0;
    switch (ls) {
    case LS_DASH: c0 = c_dash; rate = 1.25f; break;
    case LS_WALK: c0 = c_walk; rate = hs / 6.0f; if (rate < 0.5f) rate = 0.5f; if (rate > 2.3f) rate = 2.3f;
                  target_w = (hs - 0.3f) / 1.5f; break;
    case LS_TAKEOFF: c0 = c_take; rate = 1.4f; nolift = 1; break;
    case LS_AIR: c0 = c_fly; rate = flying ? 1.7f : g_red_gliding || odd_move ? 0.6f : 1.0f; nolift = 1; break;
    case LS_LAND: c0 = c_land; rate = 1.6f; nolift = ls_u < 0.62f; break;
    default: target_w = 0;
    }
    if (target_w > 1) target_w = 1; if (target_w < 0) target_w = 0;
    if (c0 >= 0) {
        int loop = ls == LS_DASH || ls == LS_WALK || ls == LS_AIR;
        ls_u += dt * rate / redclip_secs(c0);
        if (loop) { while (ls_u >= 0.97f) ls_u -= 0.95f; }
        else if (ls_u > 0.99f) ls_u = 0.99f;
        ls_w = ls == LS_WALK ? approach(ls_w, target_w, 8, dt) : 1.0f;
        g_rcl[0].clip = c0; g_rcl[0].u = ls_u; g_rcl[0].w = ls_w; g_rcl[0].mask = LM_FULL; g_rcl[0].nolift = nolift;
    } else { ls_w = approach(ls_w, 0, 8, dt); g_rcl[0].w = ls_w; if (ls_w < 0.01f) g_rcl[0].clip = -1; }
    int l0_on = clips && g_rcl[0].clip >= 0 && g_rcl[0].w > 0.05f;

    /* proper flight: hold A after the double jump and he flies. The left stick steers him like a
     * glider (left/right turns and banks, forward dives and picks up speed, back climbs and slows);
     * his wings carry him for a long while (refilled on the ground), the last of it sinking. The
     * game is kept out of its glide and away from the stick meanwhile - the flying is ours. */
    extern int g_red_flying; extern float g_aim_lx, g_aim_ly;
    {
        u32 ph = *(u32 *)(uintptr_t)(item + 0x148);
        float *vy = guest_ptr_ok(ph) ? (float *)(uintptr_t)(ph + 0x7c) : NULL;
        int a_held = g_pads_raw_a;
        int up = air || md == 28;
        extern int g_red_hide_a;
        static RS float fspeed, fturn, fpitch;
        const float FLY_TIME = 12.0f;
        if (!up) { stamina = approach(stamina, FLY_TIME, 4, dt); flying = 0; }
        if (up && g_red_dj_used && a_held && stamina > 0 && air_t > 0.35f && !g_red_pounding) {
            if (!flying) { flying = 1; red_sound_at(0x239, item, 0.5f, 0.0f); fspeed = hs > 9 ? 9 : hs; }
            stamina -= dt;
            float sx = g_aim_lx, sy = g_aim_ly;
            float *yw = (float *)(uintptr_t)(item + 0xdc);
            fturn = approach(fturn, sx, 6, dt); fpitch = approach(fpitch, sy, 4, dt);
            *yw += fturn * 1.7f * dt;                                  /* + yaw = to his right */
            float want = 9.0f + (fpitch > 0 ? fpitch * 6.0f : fpitch * 3.0f);   /* unhurried: the levels are tight */
            fspeed = approach(fspeed, want, 1.6f, dt);
            float sc = g_size_cur > 0.2f ? g_size_cur : 1.0f, k = fspeed * sc * dt / (g_move_scale * g_move_xz > 0.05f ? g_move_scale * g_move_xz : 1);
            pos[0] -= sinf(*yw) * k; pos[2] -= cosf(*yw) * k; *(u8 *)(uintptr_t)(item + 0x7e) = 0;
            if (vy) *vy = stamina > 1.5f ? 0.8f - fpitch * 8.0f : -3.0f - (fpitch > 0 ? fpitch * 6.0f : 0);   /* tiring: sinking */
            { static RS int fl; if (getenv("XBOX_FLY_LOG") && (fl++ % 10) == 0) klog("fly sx %.2f sy %.2f turn %.2f pitch %.2f spd %.1f vy %.1f st %.1f y %.1f md %d\n", sx, sy, fturn, fpitch, fspeed, vy ? *vy : 0, stamina, pos[1], md); }
            static RS float beat; float b0 = beat; beat += dt * (2.2f + (fpitch < 0 ? -fpitch * 2.0f : 0)); if ((int)beat != (int)b0) sfx_swoosh(300.0f, 0.35f, 0.55f, 0.0f);
        } else { flying = 0; fturn = fpitch = 0; }
        g_red_hide_a = flying;
        g_red_flying = flying;
        {   /* the flight camera stays with him until he is down again (the game's own view was left behind) */
            static RS int after; if (flying) after = 1; if (!up) after = 0;
            extern int g_red_flycam; g_red_flycam = flying || after; }
        { extern float g_fly_bank, g_fly_pitch; g_fly_bank = flying ? fturn : 0; g_fly_pitch = flying ? fpitch : 0; }
    }
    /* ground pound: B in the air - a moment's hang with the staff raised, then he dives straight
     * down and slams in; a ring of hits around him where he lands (they hurt what the breath hurts),
     * a big shake, a thud and a burst */
    {
        extern float g_pose_dive, g_shake; void red_ring_strike(u32 item, int n, float reach, float height, float a0);
        static RS int b_prev; static RS float pound_t;
        int b_press = g_raw_b_pub && !b_prev; b_prev = g_raw_b_pub;
        u32 ph = *(u32 *)(uintptr_t)(item + 0x148);
        float *vy = guest_ptr_ok(ph) ? (float *)(uintptr_t)(ph + 0x7c) : NULL;
        extern int g_red_aiming;
        if (b_press && getenv("XBOX_POUND_LOG")) klog("red: B air %d air_t %.2f md %u\n", air, air_t, md);
        static RS float up_t; up_t = (air || md == 28 || fabsf(vs) > 1.5f) ? up_t + dt : 0;    /* (some jumps are modes the air test doesn't know, e.g. 28) */
        int red_magic_spend(float amt);
        if (b_press && !g_red_pounding && up_t > 0.15f && !g_red_aiming && vy && red_magic_spend(20.0f)) {
            g_red_pounding = 1; pound_t = 0; flying = 0; klog("red: ground pound\n");
            sfx_swoosh(700.0f, 0.25f, 0.5f, 0.0f); red_sound_at(0x233, item, 0.7f, 0.0f); redpose_xfade(0.1f);
        }
        if (g_red_pounding) {
            pound_t += dt;
            if (vy) *vy = pound_t < 0.16f ? 3.0f : -60.0f;
            if (getenv("XBOX_POUND_LOG")) klog("pound t %.2f vv %.1f air %d md %u y %.1f\n", pound_t, vv, air, md, pos[1]);
            static RS float wh; wh += dt; if (pound_t > 0.16f && wh > 0.09f) { wh = 0; sfx_swoosh(300.0f, 0.1f, 0.25f, 0.0f); }
            static RS float stop_t; stop_t = pound_t > 0.2f && vv > -3.0f ? stop_t + dt : 0;   /* not falling any more: down */
            if (stop_t > 0.04f) { stop_t = 0;
                red_ring_strike(item, 10, 9.0f, 0.8f, 0.0f);
                red_ring_strike(item, 6, 5.0f, 2.2f, 0.52f);
                {   typedef u32 (*fx_fn)(u32, u32, float, float, float, float, float, float, float, float, u32, float);
                    ((fx_fn)(uintptr_t)0x376d0u)(0x01000040u, 0x04000068u, pos[0], pos[1] + 0.5f, pos[2], 0, 0, 0, 0, 0, 0, 3.0f); }
                g_shake = 1.6f; g_pose_crouch = 1.0f;
                sfx_swoosh(70.0f, 0.4f, 1.0f, 0.0f); sfx_swoosh(120.0f, 0.3f, 0.9f, 0.0f); red_sound_at(0x238, item, 0.9f, 0.0f);
                g_red_pounding = 0; redpose_xfade(0.08f); klog("red: pound landed (%.2f s)\n", pound_t);
            }
            if (pound_t > 3.0f) g_red_pounding = 0;
        }
        g_pose_dive = approach(g_pose_dive, g_red_pounding ? 1.0f : 0.0f, 16, dt);
    }

    /* L1: what he does */
    enum { AS_NONE, AS_COMBO, AS_CAST, AS_HURT, AS_COLLAPSE, AS_GESTURE, AS_GETUP };
    static RS int as; static RS float as_u, as_w, as_len; static RS int as_clip = -1, as_full;
    /* staff combo on square: up to three of his staff attacks in a row; hold square: magic */
    static RS int prev_x, combo_step, combo_queued, swing_air; static RS float x_held, combo_gap;
    /* (the combo is made in redpose.c now - four keyed swings; XBOX_OLD_COMBO=1 plays his recorded
     * boss swings as before) */
    static RS int newc = -1; if (newc < 0) newc = getenv("XBOX_OLD_COMBO") ? 0 : 1;
    static RS int combo_move = -1, combo_last = -1; static RS float combo_t, spin_y0, fx_tr0, fx_tr1;
    static RS int casting;
    since_swing += dt; combo_gap += dt;
    int x_press = g_red_x && !prev_x;
    if (g_red_x) x_held += dt; else x_held = 0;
    prev_x = g_red_x;
    /* his magic (redcast.c): aiming (left trigger) turns square into casts; square held casts ahead */
    static RS int classic = -1; if (classic < 0) classic = getenv("XBOX_RED_BREATH") ? 1 : 0;
    int magic;
    {   int redcast_tick(u32 pl, float dt, int air, int blocked, int x_press, int x_down, float x_held, int classic);
        void red_magic_tick(float dt), red_launch_tick(float dt); int red_flyby_tick(u32 item, float dt, int x_down);
        red_magic_tick(dt); red_launch_tick(dt);
        if (g_red_flying) magic = red_flyby_tick(item, dt, g_red_x);   /* flying: square pours fire down */
        else { red_flyby_tick(item, dt, 0);
        magic = redcast_tick(pl, dt, air || md == 28, as == AS_HURT || as == AS_COLLAPSE || g_red_shield, x_press, g_red_x, x_held, classic); }
        extern int g_red_aiming, g_red_casting; extern float g_pose_aim, g_shrink_pose;
        {   void redshrink_red_tick(u32 pl, float dt, int blocked);       /* triangle: the shrink bolt */
            redshrink_red_tick(pl, dt, as == AS_HURT || as == AS_COLLAPSE || g_red_shield || g_red_flying); }
        if (g_shrink_pose > 0) magic = 1;
        if (magic && as == AS_COMBO) { as = AS_NONE; combo_gap = 0; redpose_xfade(0.12f); }
        g_pose_aim = approach(g_pose_aim, g_red_aiming || g_red_casting || g_shrink_pose > 0 ? 1.0f : 0.0f, 12, dt);
    }
    /* the staff's hit: shots fanned across the swing (they hurt what the breath hurts) and a charge
     * tap; when one lands, a hit-stop, a shake and a thump */
    static RS float hitstop; { extern int g_melee_hit; static int seen; extern float g_shake;
        if (g_melee_hit != seen) { seen = g_melee_hit; hitstop = 0.07f; g_shake = 0.45f; sfx_swoosh(90.0f, 0.16f, 0.9f, 0.0f); } }
    float sdt = hitstop > 0 ? dt * 0.08f : dt; if (hitstop > 0) hitstop -= dt;
    void red_melee_strike(u32 item, int step);
    /* (the old charge tap on each hit made the game start and stop its charge - its camera and
     * speed jerking about every swing; the swing's own hits do the damage now. XBOX_MELEE_CHARGE=1
     * brings the tap back) */
    static RS int melee_charge = -1; if (melee_charge < 0) melee_charge = getenv("XBOX_MELEE_CHARGE") ? 1 : 0;
    { extern int g_cam_steady; g_cam_steady = as == AS_COMBO || g_red_spinning || g_red_pounding || hitstop > 0; }
    /* wing shield: right trigger held on the ground - wings wrapped round in front of him, staff
     * across; whatever would hurt him is blocked, and the blow answered with a counter-swipe */
    {
        extern int g_raw_rt_pub; extern float g_pose_shield;
        int want = g_raw_rt_pub && !air && md != 28 && !g_red_spinning && as != AS_COLLAPSE && player_can_change();
        if (want && !g_red_shield) { sfx_swoosh(240.0f, 0.2f, 0.5f, 0.0f); redpose_xfade(0.08f); if (as == AS_COMBO) as = AS_NONE; }
        g_red_shield = want;
        g_pose_shield = approach(g_pose_shield, want ? 1.0f : 0.0f, 18, dt);
    }
    /* tail-spin: square while B is held - two fast turns, staff and tail swept out, hitting all
     * round him twice */
    {
        extern float g_pose_spin, g_shake; void red_ring_strike(u32 item, int n, float reach, float height, float a0);
        static RS float spin_t, yaw0;
        float *yw = (float *)(uintptr_t)(item + 0xdc);
        int red_magic_spend(float amt);
        if (x_press && g_raw_b_pub && !magic && !g_red_spinning && !g_red_pounding && as != AS_COLLAPSE && as != AS_HURT && red_magic_spend(15.0f)) {
            g_red_spinning = 1; spin_t = 0; yaw0 = *yw; x_press = 0;
            if (as == AS_COMBO) as = AS_NONE;
            redpose_xfade(0.06f); red_sound_at(0x233, item, 0.8f, 0.0f);
        }
        if (g_red_spinning) {
            float before = spin_t; spin_t += dt;
            const float len = 0.55f; float u = spin_t / len; if (u > 1) u = 1;
            float e = u < 0.5f ? 2 * u * u : 1 - 2 * (1 - u) * (1 - u);
            *yw = yaw0 + 4.0f * 3.14159265f * e; *(u8 *)(uintptr_t)(item + 0x7e) = 0;
            if (before < 0.12f && spin_t >= 0.12f) { red_ring_strike(item, 8, 8.5f, 1.2f, yaw0); sfx_swoosh(380.0f, 0.2f, 0.7f, -0.3f); }
            if (before < 0.34f && spin_t >= 0.34f) { red_ring_strike(item, 8, 8.5f, 1.2f, yaw0 + 0.39f); sfx_swoosh(420.0f, 0.2f, 0.7f, 0.3f); red_sound_at(0x238, item, 0.8f, 0.0f); }
            if (spin_t >= len) { *yw = yaw0; g_red_spinning = 0; g_shake = 0.4f; redpose_xfade(0.15f); }
        }
        g_pose_spin = approach(g_pose_spin, g_red_spinning ? 1.0f : 0.0f, 25, dt);
        g_red_hide_b = g_red_spinning || g_red_pounding;
    }
    if (x_press && !magic && !newc) {
        if (clips && as == AS_COMBO && as_u > 0.35f && combo_step < 2) combo_queued = 1;
        else if (!(as == AS_COMBO && as_u < 0.35f) && as != AS_COLLAPSE) {
            int step = clips && as == AS_COMBO ? combo_step + 1 : combo_gap < 0.5f ? (combo_step + 1) % 3 : 0;
            if (step > 2) step = 0;
            combo_step = step; combo_queued = 0;
            if (clips && c_combo[step] >= 0) {
                as = AS_COMBO; as_clip = c_combo[step]; as_u = 0.06f; as_len = redclip_secs(as_clip) / 1.35f; as_full = !air && hs < 3.0f;
                redpose_xfade(0.08f);
            } else { g_pose_melee_side = step == 1; g_pose_melee_t = 0; redpose_xfade(0.06f); }
            since_swing = 0; swing_air = air;
        }
    }
    if (as == AS_COMBO && !newc) {
        float before = as_u;
        /* a snappier swing: a held wind-up, a fast strike through the hit, then the follow-through;
         * on the ground he lunges into the strike (more on the finisher) */
        float pace = as_u < 0.2f ? 0.85f : as_u < 0.42f ? 1.75f : 1.05f;
        as_u += sdt / as_len * pace;
        if (!air && as_u > 0.18f && as_u < 0.4f && hitstop <= 0) {
            float s = g_size_cur > 0.2f ? g_size_cur : 1.0f, lunge = (combo_step == 2 ? 13.0f : 8.0f) * s * sdt;
            lunge /= g_move_scale * g_move_xz > 0.05f ? g_move_scale * g_move_xz : 1;
            float yw = *(float *)(uintptr_t)(item + 0xdc);
            pos[0] -= sinf(yw) * lunge; pos[2] -= cosf(yw) * lunge; *(u8 *)(uintptr_t)(item + 0x7e) = 0;
        }
        if (before < 0.22f && as_u >= 0.22f) {                        /* his staff whoosh, a yell on the finisher */
            red_sound_at(0x238, item, 0.85f, combo_step == 1 ? 0.3f : -0.3f);
            if (combo_step == 2 || rand() % 3 == 0) red_sound_at(0x233, item, 0.75f, 0.0f);
        }
        if (before < 0.30f && as_u >= 0.30f) { red_melee_strike(item, combo_step); if (!swing_air && !air && melee_charge) g_red_melee_b = 6; }   /* the hit */
        if (combo_queued && as_u > 0.62f) {
            combo_queued = 0; combo_step++; as_clip = c_combo[combo_step]; as_u = 0.06f; as_len = redclip_secs(as_clip) / 1.35f;
            since_swing = 0; swing_air = air; redpose_xfade(0.1f);
        } else if (as_u >= 0.95f) { as = AS_NONE; combo_gap = 0; redpose_xfade(0.18f); }
        if (as != AS_NONE && (air || hs > 3.0f)) as_full = 0;      /* moving: upper body only */
    }
    if (newc) {
        /* square: a forehand cut, a backhand sweep, a spin and a leaping slam. A press any time
         * after a swing starts is kept and the next one follows the moment this one allows
         * (straight after its follow-through), so mashing or timing both flow */
        extern int g_sc_move, g_sc_side; extern float g_sc_t, g_sc_legs, g_shake;
        void redcombo_times(int m, float *hit, float *cancel, float *len, float *l0, float *l1, float *lv, float *tr0, float *tr1);
        void red_ring_strike(u32 item, int n, float reach, float height, float a0);
        float hit, cancel, len, l0, l1, lv;
        float *yw = (float *)(uintptr_t)(item + 0xdc);
        int start = -1;
        if (as != AS_COMBO) combo_move = -1;
        if (x_press && !magic && as != AS_COLLAPSE && as != AS_HURT) {
            if (as == AS_COMBO && combo_move >= 0) { if (combo_t > 0.04f) combo_queued = 1; }
            else start = combo_last >= 0 && combo_last < 3 && combo_gap < 0.3f ? combo_last + 1 : 0;
        }
        if (as == AS_COMBO && combo_move >= 0) {
            redcombo_times(combo_move, &hit, &cancel, &len, &l0, &l1, &lv, &fx_tr0, &fx_tr1);
            float before = combo_t; combo_t += sdt;
            if (!air && combo_t > l0 && before < l1 && hitstop <= 0) {         /* stepping into the blow */
                float s = g_size_cur > 0.2f ? g_size_cur : 1.0f, st = lv * s * sdt;
                st /= g_move_scale * g_move_xz > 0.05f ? g_move_scale * g_move_xz : 1;
                pos[0] -= sinf(*yw) * st; pos[2] -= cosf(*yw) * st; *(u8 *)(uintptr_t)(item + 0x7e) = 0;
            }
            if (before < fx_tr0 && combo_t >= fx_tr0) {                         /* the staff cutting the air */
                static const float hz[4] = { 560.0f, 640.0f, 470.0f, 330.0f };
                sfx_swoosh(hz[combo_move], 0.17f, 0.6f, combo_move == 1 ? 0.35f : -0.35f);
                red_sound_at(0x238, item, 0.8f, combo_move == 1 ? 0.3f : -0.3f);
                if (combo_move == 3 || rand() % 3 == 0) red_sound_at(0x233, item, 0.75f, 0.0f);
            }
            if (combo_move == 2) {                                              /* the spin: once round, staff out */
                float u = (combo_t - 0.06f) / 0.28f; u = u < 0 ? 0 : u > 1 ? 1 : u;
                float e = u < 0.5f ? 2 * u * u : 1 - 2 * (1 - u) * (1 - u);
                *yw = spin_y0 + g_sc_side * 6.2831853f * e; if (u >= 1) *yw = spin_y0;
                *(u8 *)(uintptr_t)(item + 0x7e) = 0;
                if (before < 0.2f && combo_t >= 0.2f) sfx_swoosh(500.0f, 0.15f, 0.5f, 0.35f);
            }
            if (before < hit && combo_t >= hit) {                               /* the hit */
                if (combo_move < 2) red_melee_strike(item, combo_move);
                else if (combo_move == 2) red_ring_strike(item, 8, 8.5f, 1.2f, *yw);
                else {                                                          /* the slam: the ground shakes */
                    red_melee_strike(item, 2);
                    red_ring_strike(item, 10, 7.0f, 0.8f, 0.0f);
                    g_shake = 1.1f;
                    if (!air) { typedef u32 (*fx_fn)(u32, u32, float, float, float, float, float, float, float, float, u32, float);
                        float s = g_size_cur > 0.2f ? g_size_cur : 1.0f;
                        ((fx_fn)(uintptr_t)0x376d0u)(0x01000040u, 0x04000068u, pos[0] - sinf(*yw) * 3.5f * s, pos[1] + 0.3f, pos[2] - cosf(*yw) * 3.5f * s, 0, 0, 0, 0, 0, 0, 2.0f); }
                    sfx_swoosh(80.0f, 0.35f, 1.0f, 0.0f); sfx_swoosh(140.0f, 0.25f, 0.8f, 0.0f);
                }
                if (!swing_air && !air && melee_charge) g_red_melee_b = 6;
            }
            if (combo_queued && combo_t >= cancel) start = (combo_move + 1) % 4;
            else if (combo_t >= len) {
                if (combo_move == 2) *yw = spin_y0;
                as = AS_NONE; combo_last = combo_move; combo_move = -1; combo_gap = 0;
            }
            if (as == AS_COMBO) as_full = !air && hs < 3.0f;
        }
        if (start >= 0) {
            if (combo_move == 2) *yw = spin_y0;
            as = AS_COMBO; as_clip = -1; combo_move = combo_step = start; combo_t = 0; combo_queued = 0;
            as_full = !air && hs < 3.0f; spin_y0 = *yw; since_swing = 0; swing_air = air;
            redcombo_times(start, &hit, &cancel, &len, &l0, &l1, &lv, &fx_tr0, &fx_tr1);
            redpose_xfade(start == 0 ? 0.07f : 0.05f);
        }
        g_sc_move = as == AS_COMBO ? combo_move : -1; g_sc_t = combo_t;
        g_sc_legs = approach(g_sc_legs, !air && hs < 3.0f ? 1.0f : 0.0f, 10, dt);
        {   static const char *f; static RS float ft = -1; if (ft < 0) { f = getenv("XBOX_SC_POSE"); ft = 0; }   /* research: hold a move at a time */
            if (f && f[0] == 'g') {                                         /* "g": every move, 9 poses each, 1 s apart */
                static RS float gt = -1e9f; if (gt < -1e8f) gt = -atof(f + 1); gt += dt; int i = gt < 0 ? 0 : (int)gt % 36, mv = i / 9; float l0_, l1_, lv_, h_, c_, ln, a_, b_;
                redcombo_times(mv, &h_, &c_, &ln, &l0_, &l1_, &lv_, &a_, &b_); g_sc_move = mv; g_sc_t = ln * (i % 9) / 8.0f * 0.75f;
                static RS int li = -1; if (i != li) { li = i; klog("sc gallery: move %d t %.3f\n", mv, g_sc_t); }
                extern char g_sc_label[32]; snprintf(g_sc_label, sizeof g_sc_label, "%c t%.2f", 'A' + mv, g_sc_t);
            } else if (f) { int mv; float tt; if (sscanf(f, "%d:%f", &mv, &tt) == 2) { g_sc_move = mv; g_sc_t = tt; } } }
    }
    if (g_pose_melee_t >= 0 && !clips) {                              /* (before the recording exists) */
        float before = g_pose_melee_t; g_pose_melee_t += dt;
        if (before < 0.12f && g_pose_melee_t >= 0.12f) { red_melee_strike(item, 0); if (!swing_air && !air && melee_charge) g_red_melee_b = 6; }
        if (before < 0.06f && g_pose_melee_t >= 0.06f) red_sound_at(0x238, item, 0.8f, 0.0f);
        if (g_pose_melee_t > 0.55f) g_pose_melee_t = -1;
    }
    /* magic: square held - the game's breath (fire, ice, ...) goes out while he casts */
    {
        extern int g_red_pass_x;
        int want_cast = classic && g_red_x && x_held > 0.3f && as != AS_HURT && as != AS_COLLAPSE;
        if (want_cast && !casting) {
            casting = 1; red_sound_at(0x233, item, 0.7f, 0.0f);
            redpose_xfade(0.15f);
        }
        if (!want_cast && casting) { casting = 0; redpose_xfade(0.25f); }
        { extern float g_pose_cast; g_pose_cast = approach(g_pose_cast, casting ? 1.0f : 0.0f, 7, dt); }
        g_red_pass_x = casting;
        if (as == AS_CAST) {                                          /* wind up, then hold the casting part */
            as_u += dt / redclip_secs(as_clip) * 1.2f;
            if (as_u > RED_CAST_HOLD_END) as_u = RED_CAST_HOLD_START;
            if (air || hs > 3.0f) as_full = 0;
        }
    }
    /* getting hit / losing a life: health at INV_BASE+4, lives at INV_BASE+0x10 */
    {
        static RS u32 hp_last = 0xffffffff; static RS int lives_last = -1; static RS float down_t;
        u32 hp = *(u32 *)(uintptr_t)(INV_BASE + 4); int lives = *(signed char *)(uintptr_t)(INV_BASE + 0x10);
        if (g_red_shield && hp_last != 0xffffffff && hp_last > 0 && hp < hp_last) {      /* blocked */
            klog("red shield: blocked a hit (hp %u -> %u)\n", hp_last, hp);
            ((sethealth_fn)(uintptr_t)FN_SETHEALTH)((void *)(uintptr_t)INV_BASE, hp_last); hp = hp_last;
            extern float g_shake; void red_melee_strike(u32 item, int step);
            sfx_swoosh(1900.0f, 0.12f, 0.8f, 0.0f); sfx_swoosh(150.0f, 0.2f, 0.8f, 0.0f); g_shake = 0.5f;
            red_melee_strike(item, 1);                                  /* the counter-swipe */
            if (newc) { as = AS_COMBO; as_clip = -1; combo_move = combo_step = 1; combo_t = 0.07f; combo_queued = 0; as_full = 0; redpose_xfade(0.05f); }
            else if (clips && c_combo[1] >= 0) { as = AS_COMBO; combo_step = 1; as_clip = c_combo[1]; as_u = 0.2f; as_len = redclip_secs(as_clip) / 1.6f; as_full = 0; redpose_xfade(0.05f); }
            red_sound_at(0x238, item, 0.9f, 0.0f);
        }
        {   int redshrink_only_tiny_near(u32 me);                      /* shrunk enemies can't hurt him */
            if (!g_red_shield && hp_last != 0xffffffff && hp_last > 0 && hp < hp_last && redshrink_only_tiny_near(item)) {
                klog("shrink: a tiny enemy's hit ignored (hp %u -> %u)\n", hp_last, hp);
                ((sethealth_fn)(uintptr_t)FN_SETHEALTH)((void *)(uintptr_t)INV_BASE, hp_last); hp = hp_last;
            } }
        if (hp_last != 0xffffffff && hp < hp_last && hp > 0 && as != AS_COLLAPSE) {
            red_sound_at(0x239, item, 0.9f, 0.0f);
            if (clips && c_hurt >= 0) { as = AS_HURT; as_clip = c_hurt; as_u = RED_HURT_FROM; as_len = 0.75f; as_full = !air; redpose_xfade(0.06f); }
        }
        if ((hp == 0 && hp_last > 0 && hp_last != 0xffffffff) || (lives_last >= 0 && lives < lives_last)) {
            if (as != AS_COLLAPSE) {
                red_sound_at(0x232, item, 1.0f, 0.0f);
                if (clips && c_collapse >= 0) { as = AS_COLLAPSE; as_clip = c_collapse; as_u = 0; as_full = 1; down_t = 0; redpose_xfade(0.1f); }
            }
        }
        if (as == AS_HURT) { as_u += dt / as_len * (RED_HURT_SPAN - RED_HURT_FROM); if (as_u >= RED_HURT_SPAN) { as = AS_NONE; redpose_xfade(0.2f); } }
        if (as == AS_COLLAPSE) {
            as_u += dt / redclip_secs(as_clip); if (as_u > 0.97f) as_u = 0.97f;
            down_t += dt;
            if (down_t > 1.0f && (hp > hp_last || down_t > 6.0f || (hp > 0 && down_t > 2.5f))) {
                if (c_getup >= 0) { as = AS_GETUP; as_clip = c_getup; as_u = 0.05f; as_full = 1; redpose_xfade(0.3f); }
                else { as = AS_NONE; redpose_xfade(0.4f); }
            }
        }
        if (as == AS_GETUP) {
            as_u += dt / redclip_secs(as_clip);
            if (as_u >= 0.97f || hs > 2.0f || air) { as = AS_NONE; redpose_xfade(0.25f); }
        }
        hp_last = hp; lives_last = lives;
    }
    /* squashed (the game's mode 67, like Spyro): flattened and spread out on the ground, a squelch
     * and a cry; when the game peels him off (back to 1 x) he springs back up with a wobble */
    {
        extern float g_pose_squash; static int was_sq; static float sy = 1, vy_s, sxz = 1;
        int sq = md == 67;
        if (sq && !was_sq) { sfx_swoosh(110.0f, 0.25f, 0.9f, 0.0f); red_sound_at(0x239, item, 0.9f, 0.0f); redpose_xfade(0.06f); as = AS_NONE; }
        if (!sq && was_sq) { sfx_swoosh(520.0f, 0.3f, 0.6f, 0.0f); vy_s = 7.0f; }
        was_sq = sq;
        g_pose_squash = approach(g_pose_squash, sq ? 1.0f : 0.0f, sq ? 25 : 8, dt);
        /* his own flattening (his model scale), a spring so it pops back with a wobble */
        /* (the game flattens the player item itself - 0.25 high - and Red with it; ours is the
         * springy pop when he's peeled off) */
        /* he's much bigger than Spyro: flatter still on top of the game's 0.25 (to ~0.1) */
        static RS float sqf = 1;
        if (sq) sqf = approach(sqf, 0.42f, 18, dt); else sqf = 1;
        float ty = 1.0f;
        vy_s += ((ty - sy) * 220.0f - vy_s * 9.0f) * dt; sy += vy_s * dt;
        if (sq) { sy = 1; vy_s = 0; }
        sxz = 1.0f + (1.0f - sy) * 0.45f + (1.0f - sqf) * 0.25f;
        sy *= 1; float sy_draw = sy * sqf;
        float *sc = (float *)(uintptr_t)(g_redchar_anim + 0xe8);
        sc[0] = 1.5f * sxz; sc[1] = 1.5f * sy_draw; sc[2] = 1.5f * sxz;
        if (getenv("XBOX_SQ_LOG") && (sq || fabsf(sy - 1) > 0.01f)) { static RS u32 nl; if (nl++ % 5 == 0) klog("squash md %u sy %.2f item sc %.2f %.2f\n", md, sy,
            ((float *)(uintptr_t)(item + 0xe8))[0], ((float *)(uintptr_t)(item + 0xec))[0]); }
    }
    /* idle: after standing a while, one of his gestures, now and then with a laugh */
    {
        static RS float idle_t, next_at = 7.0f;
        int quiet = !air && hs < 0.3f && as == AS_NONE && !charging && !g_red_x && !(g_pads[red_pad_slot()].buttons & 0xf) &&
                    abs(g_pads[red_pad_slot()].lx) < 6000 && abs(g_pads[red_pad_slot()].ly) < 6000;
        idle_t = quiet ? idle_t + dt : 0;
        if (clips && ngest && idle_t > next_at) {
            as = AS_GESTURE; as_clip = c_gest[rand() % ngest]; as_u = 0; as_full = 1; redpose_xfade(0.25f);
            {   static const u32 vox[] = RED_IDLE_VOICES; static RS int last = -1; int k = rand() % (int)(sizeof vox / sizeof vox[0]);
                if (k == last) k = (k + 1) % (int)(sizeof vox / sizeof vox[0]); last = k;
                if (rand() % 3) red_sound_at(vox[k], item, 0.75f, 0.0f); }
            idle_t = 0; next_at = 8.0f + (rand() % 60) / 10.0f;
        }
        if (as == AS_GESTURE) {
            as_u += dt / redclip_secs(as_clip);
            if (as_u >= 0.97f || (!quiet && (hs > 1.0f || air || g_red_x))) { as = AS_NONE; redpose_xfade(0.25f); }
        }
    }
    if (as != AS_NONE && as_clip >= 0) {
        as_w = approach(as_w, 1, 14, dt);
        g_rcl[1].clip = as_clip; g_rcl[1].u = as_u; g_rcl[1].w = as_w; g_rcl[1].mask = as_full ? LM_FULL : LM_UPPER; g_rcl[1].nolift = 0;
    } else { g_rcl[1].w = 0; g_rcl[1].clip = -1; as_w = 0; }
    int l1_full = clips && as != AS_NONE && as_full;
    {   extern int g_fx_trail, g_fx_cast;
        { extern int g_fx_slash; g_fx_slash = newc && as == AS_COMBO && combo_move >= 0 && combo_t > fx_tr0 && combo_t < fx_tr1; }
        g_fx_trail = (as == AS_COMBO && (newc ? combo_move >= 0 && combo_t > fx_tr0 && combo_t < fx_tr1 : as_u > 0.12f && as_u < 0.6f)) || (g_pose_melee_t > 0.04f && g_pose_melee_t < 0.35f) || ls == LS_DASH;
        extern int g_red_casting; extern float g_shrink_pose; g_fx_cast = casting || g_red_casting || g_shrink_pose > 0;
    }

    /* research: XBOX_CLIP_VIEW=1 shows every recorded clip in turn (3 s each) */
    if (getenv("XBOX_CLIP_VIEW") && clips) {
        extern int g_nbc; static float vt; vt += dt;
        int k = (int)(vt / 3.0f) % (g_nbc ? g_nbc : 1);
        static RS int lastk = -1; if (k != lastk) { lastk = k; extern u32 redclip_id(int); klog("clipview %d %08x f%u\n", k, redclip_id(k), debugmenu_play_frame()); }
        g_rcl[0].clip = k; g_rcl[0].u = fmodf(vt, 3.0f) / 3.0f; g_rcl[0].w = 1; g_rcl[0].mask = LM_FULL; g_rcl[0].nolift = 1;
        g_rcl[1].clip = -1; l0_on = 1;
    }

    g_pose_base_w = 1.0f;                     /* the engine's idle is covered by his captured idle, under the layers */
    calm = air || charging || ls != LS_NONE || as != AS_NONE || g_bake_idx_pub() >= 0 ? 0 : calm + dt;
    g_pose_capture_idle = calm > 0.6f && !g_baking;
    if (g_baking) snprintf(g_status, sizeof g_status, "Getting Red ready (first time only)...");
    { extern float g_pose_nolift; g_pose_nolift = 0; }
    u32 anim = 0x09000004u;
    if (getenv("XBOX_RED_ANIM")) { anim = (u32)strtoul(getenv("XBOX_RED_ANIM"), NULL, 16); redchar_play(anim, 0); }

    /* ---- pose layer ---- */
    g_pose_time += dt; g_pose_synth_t += dt;
    g_pose_synth = 0;
    g_pose_mirror = 0;
    g_pose_flap = 0;
    g_pose_look = 0;
    {   /* air: rising, gliding (slow descent), falling fast; a wing stroke on the double jump */
        int clip = l0_on;                                            /* his own clips do the air now */
        int glide = air && g_red_gliding && !clip, rise = air && vs > 1.0f && !glide && !clip, fast = air && !rise && !glide && air_t > 0.15f && !clip;
        g_pose_rise = approach(g_pose_rise, rise ? 1.0f : 0, 7, dt);
        g_pose_glide = approach(g_pose_glide, glide ? 1.0f : 0, 4, dt);
        g_pose_fall = approach(g_pose_fall, fast ? 1.0f : 0, 4, dt);
        static RS int was_air; static RS u32 md_air; static RS float air_t_prev;
        if (air && !was_air) redpose_xfade(0.12f);
                                                 /* double jump: upward kick mid-air */
        extern void sfx_swoosh(float hz, float dur, float vol, float pan);

        (void)md_air;
        g_pose_kick -= dt * 2; if (g_pose_kick < 0) g_pose_kick = 0;
        g_pose_hop -= dt * 2.2f; if (g_pose_hop < 0 || !air) g_pose_hop = 0;
        (void)air_t_prev;
        air_t_prev = air_t;
        was_air = air;
        g_pose_crouch -= dt * 4; if (g_pose_crouch < 0) g_pose_crouch = 0;
    }
    {   /* stepping, leaning into speed and turns */
        static RS float yaw_last, yaw_rate, gait;
        float yaw = *(float *)(uintptr_t)(item + 0xdc), dy = yaw - yaw_last;
        while (dy > 3.14159f) dy -= 6.28318f;
        while (dy < -3.14159f) dy += 6.28318f;
        yaw_last = yaw;
        yaw_rate = approach(yaw_rate, dy / dt, 8, dt);
        static RS int oldg = -1; if (oldg < 0) oldg = getenv("XBOX_OLD_GAIT") != NULL;
        float want = air || l0_on || l1_full ? 0 : hs / 11.0f;
        if (want > 1) want = 1;
        /* turning on the spot: a few small shuffling steps instead of sliding round on still feet */
        float turn_w = !oldg && !air && !l0_on && !l1_full && hs < 2.0f && fabsf(yaw_rate) > 0.8f ? fabsf(yaw_rate) * 0.07f : 0;
        if (turn_w > 0.28f) turn_w = 0.28f;
        if (turn_w > want) want = turn_w;
        gait = approach(gait, want, oldg || want > gait ? 10 : 6, dt);
        g_pose_gait = gait;
        float stride = (3.0f + 3.0f * gait) * 1.3f;          /* his long legs (and quicker ground speed) */
        static RS float slowmo = -1; if (slowmo < 0) slowmo = getenv("XBOX_GAIT_SLOWMO") ? atof(getenv("XBOX_GAIT_SLOWMO")) : 1.0f;   /* research: film the stride in slow motion */
        if (!air) g_pose_gait_phase += hs * dt / stride * 6.28318f * slowmo;
        if (!air && turn_w > 0 && hs < 2.0f) g_pose_gait_phase += fabsf(yaw_rate) * dt * 1.6f;
        if (!oldg && !air && want < 0.02f && gait > 0.01f) {
            /* stopping: finish the step so the feet come down together (hips square at ph = k pi) */
            float r = fmodf(g_pose_gait_phase, 3.14159f), left = r < 0.25f ? 0 : 3.14159f - r;
            g_pose_gait_phase += left < 7.0f * dt ? left : 7.0f * dt;
        }
        if (g_pose_gait_phase > 6.28318f * 64) g_pose_gait_phase -= 6.28318f * 64;
        g_pose_lean = 0.22f * gait + (ls == LS_WALK && hs > 8 ? 0.12f * (hs - 8) / 6 : 0);
        if (!oldg && !air) {   /* weight shifts: leans in as he sets off, rocks back as he pulls up */
            static RS float hs_last, acc, sl, slv;
            acc = approach(acc, (hs - hs_last) / dt, 10, dt); hs_last = hs;
            float tl = acc * 0.012f; if (tl > 0.14f) tl = 0.14f; if (tl < -0.16f) tl = -0.16f;
            slv += (160.0f * (tl - sl) - 12.0f * slv) * dt; sl += slv * dt;
            g_pose_lean += sl;
        }
        float bank = air ? 0 : -yaw_rate * 0.05f * gait;
        if (bank > 0.35f) bank = 0.35f;
        if (bank < -0.35f) bank = -0.35f;
        if (g_red_flying) bank = -g_fly_bank * 0.6f;              /* flying: banks into the turn */
        g_pose_bank = approach(g_pose_bank, bank, 5, dt);
        if (g_red_flying) g_pose_lean = 0.35f * g_fly_pitch;      /* and noses down into a dive */
        g_pose_breathe = !l0_on && !l1_full;
    }
    if (getenv("XBOX_RED_LOG")) {
        static RS u32 n; if (n++ % 10 == 0)
            klog("redchar mode %u hs %.1f vs %.1f air %d anim %08x charge %.2f base %.0f rise %.2f glide %.2f fall %.2f melee %.2f x %d pos %.1f %.1f %.1f\n",
                 md, hs, vs, air, anim, (float)(ls * 10 + as), g_pose_base_w, g_pose_rise, g_pose_glide, g_pose_fall, g_pose_melee_t, g_red_x, pos[0], pos[1], pos[2]);
    }
}

/* ---- Sparx beside Red: XSEItemHandler_Sparx (vtable 0x314cb8) update, slot 16 (0xa5830), is
 * wrapped. Sparx follows the player at Spyro's head height, which is about Red's knee. The offset
 * we add for drawing (higher, to Red's side) is taken off again before his own update runs, so
 * his following logic never sees it and nothing builds up. */
typedef int (__attribute__((thiscall)) *upd_fn)(void *self);
static upd_fn g_sparx_orig;
static RS struct { u32 item; float off[3]; } g_sparx_off;
static int __attribute__((thiscall)) sparx_update(void *self)
{
    u32 item = *(u32 *)((u8 *)self + 4);
    float *p = guest_ptr_ok(item) ? (float *)(uintptr_t)(item + 0xc8) : NULL;
    if (p && g_sparx_off.item == item) for (int k = 0; k < 3; k++) p[k] -= g_sparx_off.off[k];
    g_sparx_off.item = 0;
    int r = g_sparx_orig(self);
    if (getenv("XBOX_SPARX_LOG") && p) { static u32 n; if (n++ % 30 == 0) { u32 pl0 = *(u32 *)(uintptr_t)PLAYER_OBJ; float *pp = guest_ptr_ok(pl0) ? (float *)(uintptr_t)(*(u32 *)(uintptr_t)(pl0 + 4) + 0xc8) : p;
        klog("sparx raw %.1f %.1f %.1f player %.1f %.1f %.1f draw %u\n", p[0], p[1], p[2], pp[0], pp[1], pp[2], *(u16 *)(uintptr_t)(item + 0x10)); } }
    u32 pl = *(u32 *)(uintptr_t)PLAYER_OBJ;
    if (p && g_redchar_anim && guest_ptr_ok(pl)) {
        u32 pit = *(u32 *)(uintptr_t)(pl + 4);
        static RS float side = -1, up; static RS float cur[3];
        if (side < 0) { side = 4.6f; up = 6.8f; if (getenv("XBOX_SPARX_OFF")) sscanf(getenv("XBOX_SPARX_OFF"), "%f,%f", &side, &up); }
        float yaw = guest_ptr_ok(pit) ? *(float *)(uintptr_t)(pit + 0xdc) : 0;
        float want[3] = { -cosf(yaw) * side, up, sinf(yaw) * side };          /* his right, above */
        for (int k = 0; k < 3; k++) cur[k] += (want[k] - cur[k]) * 0.08f;     /* drifts round as Red turns */
        for (int k = 0; k < 3; k++) { p[k] += cur[k]; g_sparx_off.off[k] = cur[k]; }
        g_sparx_off.item = item;
        *(u8 *)(uintptr_t)(item + 0x7e) = 0;
        if (getenv("XBOX_SPARX_LOG")) { static u32 n; if (n++ % 30 == 0) { float *pp = (float *)(uintptr_t)(pit + 0xc8);
            klog("sparx %.1f %.1f %.1f player %.1f %.1f %.1f\n", p[0], p[1], p[2], pp[0], pp[1], pp[2]); } }
    }
    return r;
}
/* ---- Red the boss (3Z): XSEItemHandler_Red, vtable 0x317d58, update = slot 16 (0xc0330) ---- */
static upd_fn g_redboss_orig;
u32 g_redboss;                      /* the boss handler seen last (0 = none) */
double g_redboss_time;              /* when it last ran */
static void redboss_tick(u32 self);
static int __attribute__((thiscall)) redboss_update(void *self)
{
    g_redboss = (u32)(uintptr_t)self;
    { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); g_redboss_time = ts.tv_sec + ts.tv_nsec * 1e-9; }
    int r = g_redboss_orig(self);
    redboss_tick((u32)(uintptr_t)self);
    return r;
}
static void redboss_hook_install(void)
{
    u32 *slot = (u32 *)(uintptr_t)(0x317d58 + 16 * 4);
    if (*slot != 0xc0330) { klog("debug: Red boss update slot unexpected (%08x)\n", *slot); return; }
    g_redboss_orig = (upd_fn)(uintptr_t)*slot;
    *slot = (u32)(uintptr_t)redboss_update;
}
static void redboss_tick(u32 self) { (void)self; }

static void sparx_hook_install(void)
{
    redboss_hook_install();
    u32 *slot = (u32 *)(uintptr_t)(0x314cb8 + 16 * 4);
    if (*slot != 0xa5830) { klog("debug: Sparx update slot unexpected (%08x)\n", *slot); return; }
    g_sparx_orig = (upd_fn)(uintptr_t)*slot;
    *slot = (u32)(uintptr_t)sparx_update;
}

static void redchar_tick(void)
{
    static int installed;
    if (!installed) { installed = 1; redpose_install(); sparx_hook_install();
        { extern float g_cast_spread, g_cast_tip; if (getenv("XBOX_CAST_WINGS")) sscanf(getenv("XBOX_CAST_WINGS"), "%f,%f", &g_cast_spread, &g_cast_tip); } void redvoice_install(void); redvoice_install(); }
    u32 pl = *(u32 *)(uintptr_t)PLAYER_OBJ;
    /* new level / new player object: his body went with the old item (don't touch it) */
    if (g_redchar_anim && (!guest_ptr_ok(pl) || *(u32 *)(uintptr_t)(pl + 4) != g_redchar_item)) {
        g_redchar_anim = g_redchar_item = g_redchar_spyro = 0; g_red_animator = 0; g_redchar_cur = 0;
    }
    g_redchar_cur_pub = g_redchar_cur; g_red_progress_pub = g_redchar_anim ? anim_progress(g_redchar_anim) : 0;
    if (!g_redchar_want) { if (g_redchar_anim) redchar_detach(); return; }
    if (!guest_ptr_ok(pl)) return;
    if (*(u32 *)(uintptr_t)(pl + 0x548) != (u32)g_redchar_base) return;   /* becomes Red once the base is in */
    if (!g_redchar_anim) {
        if (!redchar_attach(pl)) { snprintf(g_status, sizeof g_status, "Loading Red..."); return; }
        snprintf(g_status, sizeof g_status, "Now playing as Red");
        void red_voice_preload(void); red_voice_preload();
    }
    g_size_mult = RED_SIZE;                                  /* hitbox and camera to his size */
    {   static float sp = -1; if (sp < 0) sp = getenv("XBOX_RED_SPEED") ? atof(getenv("XBOX_RED_SPEED")) : 1.3f;
        g_move_xz = sp; }
    spyro_visible(0);                                        /* (the game may switch him back on) */
    *(u8 *)(uintptr_t)(g_redchar_anim + 0x10) |= 1;
    redchar_animate(pl);
    { void redcast_install(void), redcast_test_tick(void); redcast_install(); redcast_test_tick(); }
    if (getenv("XBOX_ADV_TRACE")) {            /* research: is Red's clip clock running? */
        extern u32 g_red_track; u32 nd = *(u32 *)(uintptr_t)(g_redchar_anim + 0x120); g_red_track = nd ? nd - 4 : 0;
        void redpose_adv_report(void); redpose_adv_report();
    }
}

void dm_red_player_tick(void) { redchar_tick(); size_tick(); }   /* player 2's (coop.c) */

/* ---- draw distance ----
 * The game camera (EXCamera, vtable 0x339930) is applied each frame through its virtual at
 * 0x1e0460 (ecx = camera). +0x4c near / +0x50 far build the projection (rctx +0x8b4). */
u32 g_cam_obj, g_cam_calls;
static const float g_dd_mult[] = { 1, 2, 4, 8, 16 };
#define NDD 5
static int g_dd = 0;
/* called from the hook before every camera apply: far (+0x50) = what the game set * multiplier.
 * The game sets far once per camera (1000 in the levels seen so far); a value we did not write
 * ourselves is taken as the new original. */
void cam_apply_c(u32 cam)
{
    static struct { u32 cam; float base, written; } t[16];
    g_cam_obj = cam; g_cam_calls++;
    float *far = (float *)(uintptr_t)(cam + 0x50);
    int i;
    for (i = 0; i < 16 && t[i].cam && t[i].cam != cam; i++) ;
    if (i == 16) i = (int)(g_cam_calls % 16);
    if (t[i].cam != cam || *far != t[i].written) { t[i].cam = cam; t[i].base = *far; }
    t[i].written = t[i].base * g_dd_mult[g_dd];
    if (!(t[i].written > 0) || !isfinite(t[i].written)) t[i].written = t[i].base;
    *far = t[i].written;
    size_camera(cam);
}
__asm__(".globl cam_apply_stub\n"
        "cam_apply_stub:\n"
        "  pushal\n"
        "  pushl %ecx\n"
        "  call cam_apply_c\n"
        "  addl $4, %esp\n"
        "  popal\n"
        "  subl $0xe0, %esp\n"
        "  pushl $0x1e0466\n"
        "  ret\n");
void cam_apply_stub(void);

/* Objects (SE_Trigger items: gems, NPCs, enemies, props) only exist while a scan point (Spyro /
 * camera, table at 0x4464e0) is within the trigger's active range: IsInside/IsOutsideActiveRange
 * (0x291a0 / 0x291c0) pass (range -/+ 1)^2 to the test at 0x290e0. Scaling that argument scales
 * how far away objects appear. */
float g_range_mult2 = 1.0f; u32 g_range_calls;
__asm__(".globl range_stub\n"
        "range_stub:\n"
        "  incl g_range_calls\n"
        "  flds 4(%esp)\n"
        "  fmuls g_range_mult2\n"
        "  fstps 4(%esp)\n"
        "  pushl %ebx\n"
        "  movl 0xc(%esp), %ebx\n"
        "  pushl $0x290e5\n"
        "  ret\n");
void range_stub(void);
static void cam_hook_install(void)
{
    static int done; if (done) return; done = 1;
    const u8 *p = (const u8 *)(uintptr_t)0x1e0460;
    extern void hle_patch_jmp(u32 at, void *to);
    if (p[0] == 0x81 && p[1] == 0xec && p[2] == 0xe0) hle_patch_jmp(0x1e0460, (void *)cam_apply_stub);
    else klog("debug: camera apply hook: unexpected bytes\n");
    p = (const u8 *)(uintptr_t)0x290e0;
    static const u8 want[] = { 0x53, 0x8b, 0x5c, 0x24, 0x0c, 0x55 };
    if (!memcmp(p, want, sizeof want)) hle_patch_jmp(0x290e0, (void *)range_stub);
    else klog("debug: trigger range hook: unexpected bytes\n");
}

static void cycle_drawdist(int d) { g_dd = (g_dd + d + NDD) % NDD; }
static const char *drawdist_name(void)
{
    static char b[32]; snprintf(b, sizeof b, g_dd ? "%gx" : "1x (original)", g_dd_mult[g_dd]); return b;
}

/* ---- frame rate ----
 * The game's display setup (0x1cfb30) stores the refresh rate in [0x4703f0]+0x84 and its target
 * frame rate in 0x34ec48 (plus 0x34ec40 = rate, 0x34ec44 = half rate). Each frame it takes
 * dt = vblanks / refresh and only updates when vblanks >= refresh / target. Raising all of them
 * together with the emulated vblank rate gives one game update per (faster) vblank. */
extern volatile int g_target_fps;
static const int g_fps_choices[] = { 60, 75, 90, 120, 144, 165, 240, 244 };
#define NFPS (int)(sizeof g_fps_choices / sizeof g_fps_choices[0])
static void apply_fps(void)
{
    static int init;
    if (!init) {
        init = 1;
        const char *e = getenv("XBOX_FPS");
        if (e) { int f = (int)parse_u32(e, 10); if (f >= 30 && f <= 360) g_target_fps = f; }
        if ((e = getenv("XBOX_MODERN_CAM"))) g_modern_cam = atoi(e) != 0;
        if ((e = getenv("XBOX_DRAWDIST"))) {
            float m = (float)atof(e);
            for (int i = 0; i < NDD; i++) if (g_dd_mult[i] <= m) g_dd = i;
        }
    }
    u32 disp = *(u32 *)(uintptr_t)0x4703f0u;
    if (!guest_ptr_ok(disp)) return;
    u32 f = (u32)g_target_fps;
    u32 *refresh = (u32 *)(uintptr_t)(disp + 0x84);
    static u32 applied;
    if (*refresh != 60 && *refresh != 50 && *refresh != applied) return;   /* not initialised yet */
    if (*refresh == f && *(u32 *)(uintptr_t)0x34ec48u == f) return;
    applied = f;
    *refresh = f;
    *(u32 *)(uintptr_t)0x34ec40u = f;
    *(u32 *)(uintptr_t)0x34ec48u = f;
    *(u32 *)(uintptr_t)0x34ec44u = f / 2;
    klog("debug: game frame rate set to %u\n", f);
}

/* ---- warp, executed on the game thread ---- */
static volatile int g_warp_pending;
static u32 g_warp_hash, g_warp_obj; static s32 g_warp_start;

static const struct { s32 id; const char *name; } g_starts[] = {
    { -1, "default" }, { 0x4a000000, "START" }, { 0x4a000001, "Restart1" }, { 0x4a000002, "Restart2" },
    { 0x4a000003, "Restart3" }, { 0x4a000004, "Restart4" }, { 0x4a000005, "Restart5" }, { 0x4a000006, "Restart6" },
};
#define NSTARTS (int)(sizeof g_starts / sizeof g_starts[0])

void debugmenu_request_warp(u32 hash, s32 start) { g_warp_obj = 0; g_warp_hash = hash; g_warp_start = start; g_warp_pending = 1; }
static void request_warp_obj(const struct mapent *m, s32 start) { g_warp_obj = m->obj; g_warp_hash = m->hash; g_warp_start = start; g_warp_pending = 1; }

/* called from the D3D Swap hook (game thread, between frames) */
/* tests: the frame at which the player has been controllable for 2 seconds */
static u32 g_play_start;
u32 debugmenu_play_frame(void)
{
    extern u32 gl_frame_number(void);
    return g_play_start ? gl_frame_number() - g_play_start + 1 : 0;
}
static void play_start_tick(void)
{
    static u32 since; extern u32 gl_frame_number(void);
    if (g_play_start) return;
    /* controllable, standing, and not paused (the idle animation is moving) */
    static float lastp; static u32 moving;
    u32 pl = *(u32 *)(uintptr_t)PLAYER_OBJ;
    float prog = -1;
    if (guest_ptr_ok(pl)) {
        u32 item = *(u32 *)(uintptr_t)(pl + 4);
        u32 ap = guest_ptr_ok(item) ? *(u32 *)(uintptr_t)(item + 0x13c) : 0;
        u32 node = guest_ptr_ok(ap) ? *(u32 *)(uintptr_t)(ap - 4 + 0x11c) : 0;
        if (guest_ptr_ok(node)) prog = *(float *)(uintptr_t)(node - 4 + 0x80);
    }
    if (player_can_change() && *(u32 *)(uintptr_t)(pl + 0x7ec) == 1) {
        if (!since) { since = gl_frame_number(); moving = 0; }
        if (prog != lastp) moving++;
        /* (with XBOX_AUTOSTART pressing START at cycle frames 0-5, only finish well clear of a
         * press, or the pause it opens lands just after "gameplay began") */
        int clear = 1;
        u32 cyc = gl_frame_number() % 240;               /* autostart presses START at 0-5, A at 60-65 */
        if (getenv("XBOX_AUTOSTART") && (cyc < 12 || (cyc >= 60 && cyc < 72))) since = gl_frame_number(), moving = 0;
        if (gl_frame_number() - since >= 120 && moving > 90 && clear) { g_play_start = gl_frame_number(); klog("debug: gameplay began at frame %u\n", g_play_start); }
        else if (gl_frame_number() - since >= 120) since = 0;
    } else since = 0;
    lastp = prog;
}
/* test frame numbers: "+N" = N frames after gameplay began */
static int test_due(const char *tok, unsigned *out_rest)
{
    (void)out_rest; extern u32 gl_frame_number(void);
    if (tok[0] == '+') { u32 r = debugmenu_play_frame(); return r && r >= parse_u32(tok + 1, 10); }
    return gl_frame_number() >= parse_u32(tok, 10);
}

void debugmenu_game_tick(void)
{
    if (!g_redchar_anim && getenv("XBOX_CAST_AT")) { void redcast_test_tick(void); redcast_test_tick(); }
    play_start_tick();
    { void coop_tick(void); coop_tick(); }
    { void redshrink_research(void), redshrink_tick(void); redshrink_research(); redshrink_tick(); }
    static int dumped = -1;
    if (dumped < 0) dumped = getenv("XBOX_LIST_MAPS") ? 0 : 1;
    extern u32 gl_frame_number(void);
    if (!dumped && gl_frame_number() > 700) { dumped = 1; debugmenu_dump_maps(); }
    if (getenv("XBOX_FIND_CAM")) {
        static int last;
        int every = 0; unsigned from = 0;
        sscanf(getenv("XBOX_FIND_CAM"), "%u:%d", &from, &every);
        if (gl_frame_number() >= from && (int)gl_frame_number() != last && (last == 0 || (every && (int)gl_frame_number() - last >= every))) {
            last = gl_frame_number(); find_camera();
        }
    }
    for (int wi = 0; wi < 2; wi++) if (getenv(wi ? "XBOX_WARP2" : "XBOX_WARP") && !g_warp_pending) {   /* XBOX_WARP=frame:hash[:start[:sub]] for automated tests (XBOX_WARP2: a second one) */
        static int dones[2]; int done = dones[wi];
        unsigned fr = 0, h = 0; int st = -1;
        const char *w = getenv(wi ? "XBOX_WARP2" : "XBOX_WARP"); int rel = w[0] == '+';
        unsigned sub = 0;                                   /* optional 4th field: the map object's sub id */
        if (!done && sscanf(w + rel, "%u:%x:%x:%x", &fr, &h, (unsigned *)&st, &sub) >= 2 &&
            (rel ? debugmenu_play_frame() && debugmenu_play_frame() >= fr : gl_frame_number() >= fr)) {
            dones[wi] = 1; debugmenu_request_warp(h, st);
            if (sub) { scan_maps(); for (int i = 0; i < g_nmaps; i++) if (g_maps[i].hash == h && g_maps[i].sub == sub) request_warp_obj(&g_maps[i], st); }
        }
    }
    if (getenv("XBOX_PROBE_MAP")) {            /* research: which map manager fields hold a map object */
        static u32 last; u32 pf = debugmenu_play_frame();
        if (pf && pf - last >= 300) { last = pf; scan_maps();
            for (u32 o = 0; o < 0x400; o += 4) { u32 v = *(u32 *)(uintptr_t)(MAP_MANAGER + o);
                for (int i = 0; i < g_nmaps; i++) if (g_maps[i].obj == v) klog("probe: mgr+%03x = %s\n", o, g_maps[i].name); }
            u32 st = *(u32 *)(uintptr_t)(MAP_MANAGER + 0x78);
            if (guest_ptr_ok(st)) for (u32 o = 0; o < 0x200; o += 4) { u32 v = *(u32 *)(uintptr_t)(st + o);
                for (int i = 0; i < g_nmaps; i++) if (g_maps[i].obj == v) klog("probe: st+%03x = %s\n", o, g_maps[i].name); } }
    }
    apply_fps();
    cam_hook_install();
    { float m = g_dd_mult[g_dd] > 4 ? 4 : g_dd_mult[g_dd]; g_range_mult2 = m * m; }   /* objects: up to 4x (memory) */
    if (getenv("XBOX_CAM_LOG") && g_cam_obj) {
        static u32 last; extern u32 gl_frame_number(void);
        if (gl_frame_number() - last >= 300) {
            last = gl_frame_number();
            const float *c = (const float *)(uintptr_t)g_cam_obj;
            klog("range calls %u\n", g_range_calls);
            klog("cam %08x vt %08x calls %u near %g far %g +54 %g +58 %g +64 %08x\n", g_cam_obj, *(u32 *)(uintptr_t)g_cam_obj,
                 g_cam_calls, c[0x4c / 4], c[0x50 / 4], c[0x54 / 4], c[0x58 / 4], *(u32 *)(uintptr_t)(g_cam_obj + 0x64));
        }
    }
    if (getenv("XBOX_DD_AT")) {                /* tests: XBOX_DD_AT=frame:index */
        static int done; unsigned fr = 0, ix = 0; extern u32 gl_frame_number(void);
        if (!done && sscanf(getenv("XBOX_DD_AT"), "%u:%u", &fr, &ix) == 2 && gl_frame_number() >= fr) { done = 1; g_dd = ix % NDD; }
    }
    if (getenv("XBOX_SQUASH_AT")) {            /* tests: XBOX_SQUASH_AT=on:off[:on:off...] */
        unsigned t[8]; int n = 0; extern u32 gl_frame_number(void);
        char buf[128]; snprintf(buf, sizeof buf, "%s", getenv("XBOX_SQUASH_AT"));
        char *sv = NULL; for (char *tok = strtok_r(buf, ":", &sv); tok && n < 8; tok = strtok_r(NULL, ":", &sv)) t[n++] = parse_u32(tok + (tok[0] == '+'), 10);
        static int done[8];
        int rel = getenv("XBOX_SQUASH_AT")[0] == '+'; u32 now = rel ? debugmenu_play_frame() : gl_frame_number();
        for (int i = 0; i < n; i++) if (!done[i] && (!rel || now) && now >= t[i]) { done[i] = 1; g_squash = !(i & 1); }
    }
    if (getenv("XBOX_OBJ_PROBE")) {            /* research: XBOX_OBJ_PROBE=frame:vtable  (objects of a class) */
        static int done; unsigned fr = 0, vt = 0; extern u32 gl_frame_number(void);
        const char *w = getenv("XBOX_OBJ_PROBE"); int rel = w[0] == '+';
        if (!done && sscanf(w + rel, "%u:%x", &fr, &vt) == 2 && (rel ? debugmenu_play_frame() >= fr && debugmenu_play_frame() : gl_frame_number() >= fr)) {
            done = 1;
            extern int kmem_committed(u32 a); int hits = 0;
            for (u32 pg = 0x10000000u; pg < 0x70000000u; pg += 0x1000) {
                if (!kmem_committed(pg)) continue;
                u32 lo = pg, hi = pg + 0x1000;
                for (u32 a = lo; a + 4 <= hi; a += 4) {
                    if (*(u32 *)(uintptr_t)a != vt || hits++ > 8) continue;
                    u32 item = *(u32 *)(uintptr_t)(a + 4);
                    klog("objprobe obj %08x item %08x\n", a, item);
                    if (!guest_ptr_ok(item)) continue;
                    const float *p = (const float *)(uintptr_t)(item + 0xc8);
                    u32 anp = *(u32 *)(uintptr_t)(item + 0x13c);
                    klog("objprobe  pos %.2f %.2f %.2f rot %.2f %.2f %.2f scale %.2f anim %08x animator %08x mode %u\n", p[0], p[1], p[2],
                         p[4], p[5], p[6], ((float *)(uintptr_t)(item + 0xe8))[0], *(u32 *)(uintptr_t)(item + 0x178), anp, *(u32 *)(uintptr_t)(a + 0x7ec));
                    if (!guest_ptr_ok(anp)) continue;
                    u32 an = anp - 4, x1 = *(u32 *)(uintptr_t)(an + 0x1c);
                    u32 set = guest_ptr_ok(x1) ? *(u32 *)(uintptr_t)(x1 + 0x24) : 0;
                    if (!guest_ptr_ok(set)) continue;
                    u32 tab = set + 0x8c;
                    short cnt = *(short *)(uintptr_t)tab, mode = *(short *)(uintptr_t)(tab + 2);
                    int32_t off = *(int32_t *)(uintptr_t)(tab + 4);
                    u32 base = off ? tab + 4 + off : 0;
                    klog("objprobe  animset %08x count %d mode %d base %08x\n", set, cnt, mode, base);
                    for (int i = 0; base && i < cnt && i < 4096; i++) {
                        const u32 *e = (const u32 *)(uintptr_t)(base + i * 0x14);
                        if (e[0] | e[1] | e[2]) klog("objprobe   anim %d: %08x %08x %08x %08x %08x\n", i, e[0], e[1], e[2], e[3], e[4]);
                    }
                }
            }
            klog("objprobe: %d objects\n", hits);
        }
    }
    if (getenv("XBOX_CLASS_PROBE")) {          /* research: XBOX_CLASS_PROBE=+frame:substring - objects by class name */
        static int done; extern u32 gl_frame_number(void);
        const char *w = getenv("XBOX_CLASS_PROBE"); int rel = w[0] == '+';
        unsigned fr = parse_u32(w + rel, 10); const char *sub = strchr(w, ':'); sub = sub ? sub + 1 : "";
        if (!done && (rel ? debugmenu_play_frame() >= fr && debugmenu_play_frame() : gl_frame_number() >= fr)) {
            done = 1;
            extern int kmem_committed(u32 a); int hits = 0;
            for (u32 pg = 0x10000000u; pg < 0x70000000u; pg += 0x1000) {
                if (!kmem_committed(pg)) continue;
                u32 lo = pg, hi = pg + 0x1000;
                for (u32 a = lo; a + 8 <= hi; a += 4) {
                    u32 vt = *(u32 *)(uintptr_t)a;
                    if (vt < 0x2e0000 || vt > 0x350000 || (vt & 3)) continue;
                    u32 f0 = *(u32 *)(uintptr_t)vt;
                    if (f0 < 0x11000 || f0 > 0x2a0000) continue;
                    const u8 *c = (const u8 *)(uintptr_t)f0;
                    if (c[0] != 0xB8 || c[5] != 0xC3) continue;
                    u32 rtc = *(u32 *)(uintptr_t)(c + 1);
                    if (rtc < 0x2e0000 || rtc > 0x350000) continue;
                    const char *nm = NULL;                    /* runtime class: {parent, name, size, create} */
                    u32 np = *(u32 *)(uintptr_t)(rtc + 4);
                    if (np >= 0x2e0000 && np < 0x350000) nm = (const char *)(uintptr_t)np;
                    if (!nm || !strstr(nm, sub) || hits++ > 40) continue;
                    u32 item = *(u32 *)(uintptr_t)(a + 4);
                    const float *p = guest_ptr_ok(item) ? (const float *)(uintptr_t)(item + 0xc8) : NULL;
                    klog("classprobe %08x vt %08x %s item %08x pos %.2f %.2f %.2f\n", a, vt, nm, item, p ? p[0] : 0, p ? p[1] : 0, p ? p[2] : 0);
                }
            }
            klog("classprobe: %d hits\n", hits);
        }
    }
    if (getenv("XBOX_DUMP_SIZE")) {            /* research: player physics + camera controller */
        static int done; extern u32 gl_frame_number(void);
        if (!done && gl_frame_number() >= parse_u32(getenv("XBOX_DUMP_SIZE"), 10)) {
            done = 1;
            u32 pl = *(u32 *)(uintptr_t)PLAYER_OBJ;
            u32 item = guest_ptr_ok(pl) ? *(u32 *)(uintptr_t)(pl + 4) : 0;
            u32 phys = guest_ptr_ok(item) ? *(u32 *)(uintptr_t)(item + 0x148) : 0;
            u32 g = *(u32 *)(uintptr_t)0x42bb18u;
            u32 cam = guest_ptr_ok(g) ? *(u32 *)(uintptr_t)(g + 0x4b8) : 0;
            struct { const char *n; u32 a, len; } r[] = { { "item", item, 0x200 }, { "phys", phys, 0x200 },
                                                          { "camctl", cam, 0x500 }, { "pl", pl, 0x900 } };
            for (int k = 0; k < 4; k++) {
                if (!guest_ptr_ok(r[k].a)) continue;
                klog("dumpsize %s %08x vt %08x\n", r[k].n, r[k].a, *(u32 *)(uintptr_t)r[k].a);
                for (u32 o = 0; o < r[k].len; o += 16) {
                    const u32 *v = (const u32 *)(uintptr_t)(r[k].a + o);
                    klog("dumpsize %s +%03x: %08x %08x %08x %08x | %g %g %g %g\n", r[k].n, o, v[0], v[1], v[2], v[3],
                         *(float *)&v[0], *(float *)&v[1], *(float *)&v[2], *(float *)&v[3]);
                }
            }
        }
    }
    if (getenv("XBOX_TRACK_PROBE")) {          /* research: animation track objects while squashed */
        static u32 prev[2][0x100 / 4], prevp[2]; extern u32 gl_frame_number(void);
        u32 f0 = parse_u32(getenv("XBOX_TRACK_PROBE"), 10), fn = gl_frame_number();
        u32 pl = *(u32 *)(uintptr_t)PLAYER_OBJ;
        u32 item = guest_ptr_ok(pl) ? *(u32 *)(uintptr_t)(pl + 4) : 0;
        u32 an = guest_ptr_ok(item) ? *(u32 *)(uintptr_t)(item + 0x13c) : 0;
        if (guest_ptr_ok(an) && fn >= f0 && fn < f0 + 30) {
            an -= 4;
            for (int k = 0; k < 2; k++) {
                u32 node = *(u32 *)(uintptr_t)(an + 0x11c + k * 4);
                if (!guest_ptr_ok(node)) continue;
                u32 t = node - 4; const u32 *o = (const u32 *)(uintptr_t)t;
                char buf[2400]; int n = 0;
                for (int i = 0; i < 0x100 / 4 && n < 2300; i++)
                    if (t != prevp[k] || o[i] != prev[k][i]) n += snprintf(buf + n, sizeof buf - n, " +%02x=%08x(%g)", i * 4, o[i], *(const float *)&o[i]);
                klog("track%d f%u mode %u obj %08x vt %08x:%s\n", k, fn, *(u32 *)(uintptr_t)(pl + 0x7ec), t, o[0], buf);
                memcpy(prev[k], o, sizeof prev[k]); prevp[k] = t;
            }
        }
    }
    if (getenv("XBOX_ANIM_PROBE")) {           /* research: animator fields while an animation plays */
        static float prev[0x300 / 4]; static u32 pf; extern u32 gl_frame_number(void);
        u32 f0 = parse_u32(getenv("XBOX_ANIM_PROBE"), 10), fn = gl_frame_number();
        u32 pl = *(u32 *)(uintptr_t)PLAYER_OBJ;
        u32 item = guest_ptr_ok(pl) ? *(u32 *)(uintptr_t)(pl + 4) : 0;
        u32 an = guest_ptr_ok(item) ? *(u32 *)(uintptr_t)(item + 0x13c) : 0;
        if (guest_ptr_ok(an) && fn >= f0 && fn < f0 + 120) {
            an -= 4;
            const float *o = (const float *)(uintptr_t)an;
            if (fn == f0) {
                klog("anim-dump f%u an %08x anim %08x mode %u:", fn, an, *(u32 *)(uintptr_t)(item + 0x178), *(u32 *)(uintptr_t)(pl + 0x7ec));
                for (int i = 0; i < 0x300 / 4; i++) klog(" +%03x=%08x", i * 4, ((u32 *)o)[i]);
                klog("\n");
            } else if (pf) {
                char buf[2048]; int n = 0;
                for (int i = 0; i < 0x300 / 4 && n < 1900; i++) {
                    if (((u32 *)o)[i] == ((u32 *)prev)[i]) continue;
                    if (i * 4 >= 0x80 && i * 4 < 0xc0) continue;          /* matrix copy */
                    n += snprintf(buf + n, sizeof buf - n, " +%03x=%08x(%g)", i * 4, ((u32 *)o)[i], o[i]);
                }
                klog("anim f%u mode %u 7a8 %x 7ac %x:%s\n", fn, *(u32 *)(uintptr_t)(pl + 0x7ec), *(u32 *)(uintptr_t)(pl + 0x7a8),
                     *(u32 *)(uintptr_t)(pl + 0x7ac), buf);
            }
            memcpy(prev, o, sizeof prev); pf = fn;
        }
    }
    if (getenv("XBOX_SIZE_AT")) {              /* tests: XBOX_SIZE_AT=frame:size[,frame:size...] */
        static int done[8]; extern u32 gl_frame_number(void);
        char buf[128]; snprintf(buf, sizeof buf, "%s", getenv("XBOX_SIZE_AT"));
        char *sv = NULL; int n = 0;
        for (char *tok = strtok_r(buf, ",", &sv); tok && n < 8; tok = strtok_r(NULL, ",", &sv), n++) {
            char *c = strchr(tok, ':'); if (!c) continue; *c++ = 0;
            if (!done[n] && test_due(tok, NULL)) { done[n] = 1; g_size = (float)atof(c); klog("debug: size %g\n", g_size); }
        }
    }
    if (getenv("XBOX_CHAR_AT")) {              /* tests: XBOX_CHAR_AT=[+]frame:player[,...] */
        static int done[8]; char buf[128], *sv = NULL; int n = 0;
        snprintf(buf, sizeof buf, "%s", getenv("XBOX_CHAR_AT"));
        for (char *tok = strtok_r(buf, ",", &sv); tok && n < 8; tok = strtok_r(NULL, ",", &sv), n++) {
            char *c = strchr(tok, ':'); if (!c) continue; *c++ = 0;
            if (!done[n] && test_due(tok, NULL)) { done[n] = 1; g_char_req = atoi(c); klog("debug: test character %d\n", g_char_req); }
        }
    }
    if (getenv("XBOX_MODE_LOG")) {             /* research: the player's mode changes */
        static u32 lm = ~0u; u32 pl = *(u32 *)(uintptr_t)PLAYER_OBJ;
        if (guest_ptr_ok(pl)) {
            u32 m = *(u32 *)(uintptr_t)(pl + 0x7ec);
            if (m != lm) { lm = m; u32 it = *(u32 *)(uintptr_t)(pl + 4);
                klog("mode %u type %u frame %u anim %08x y %.2f\n", m, *(u32 *)(uintptr_t)(pl + 0x548), debugmenu_play_frame(),
                     *(u32 *)(uintptr_t)(it + 0x178), ((float *)(uintptr_t)(it + 0xc8))[1]); }
        }
    }
    cheats_tick();
    character_tick();
    squash_tick();
    redchar_tick();
    size_tick();
    { void coop_red_tick(void); coop_red_tick(); }
    { void redcut_tick(u32 red_anim); redcut_tick(g_redchar_anim); }
   /* Red stands in for Spyro in cutscenes */
    {
        static double last; struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
        double t = ts.tv_sec + ts.tv_nsec / 1e9;
        noclip_tick(last ? (float)(t - last) : 0.0f); last = t;
    }
    if (getenv("XBOX_FIND_FAR")) {           /* research: camera objects (near 0.001 at +0x4c, far at +0x50) */
        static int done;
        if (!done && gl_frame_number() >= parse_u32(getenv("XBOX_FIND_FAR"), 10)) {
            done = 1;
            FILE *f = fopen("/proc/self/maps", "r"); char line[256];
            while (f && fgets(line, sizeof line, f)) {
                unsigned lo, hi; char perm[8];
                if (sscanf(line, "%x-%x %7s", &lo, &hi, perm) != 3 || perm[0] != 'r' || perm[1] != 'w') continue;
                if (lo < 0x10000 || hi > 0x80000000u) continue;
                for (u32 a = lo + 0x4c; a + 8 <= hi; a += 4) {
                    if (*(u32 *)(uintptr_t)a != 0x3a83126fu) continue;
                    const float *v = (const float *)(uintptr_t)a;
                    klog("farscan: obj %08x vt %08x near %g far %g +54 %g +58 %g +64 %08x\n", a - 0x4c,
                         *(u32 *)(uintptr_t)(a - 0x4c), v[0], v[1], v[2], v[3], *(u32 *)(uintptr_t)(a + 0x18));
                }
            }
            if (f) fclose(f);
        }
    }
    if (getenv("XBOX_CHEAT_AT")) {           /* tests: XBOX_CHEAT_AT=frame:mask (1 abilities 2 gems 4 health 8 noclip) */
        static int done; unsigned fr = 0, mk = 0;
        if (!done && sscanf(getenv("XBOX_CHEAT_AT"), "%u:%u", &fr, &mk) == 2 && gl_frame_number() >= fr) {
            done = 1; g_cheat_req |= mk & 6; if (mk & 1) g_unlock = 1; if (mk & 8) g_noclip = 1; if (mk & 16) g_invincible = 1;
        }
    }
    if (getenv("XBOX_DUMP_PLAYER")) {         /* research: player object + camera */
        static int done;
        if (!done && gl_frame_number() >= parse_u32(getenv("XBOX_DUMP_PLAYER"), 10)) {
            done = 1;
            u32 pl = *(u32 *)(uintptr_t)0x44c704u, rc = *(u32 *)(uintptr_t)0x4703f0u;
            if (guest_ptr_ok(rc)) { const float *c = (const float *)(uintptr_t)(rc + 0x934);
                klog("dump: camera pos %g %g %g\n", c[0], c[1], c[2]); }
            klog("dump: player %08x vtbl %08x\n", pl, guest_ptr_ok(pl) ? *(u32 *)(uintptr_t)pl : 0);
            if (guest_ptr_ok(pl)) for (u32 o = 0; o < 0x600; o += 16) {
                const float *v = (const float *)(uintptr_t)(pl + o); const u32 *u = (const u32 *)v;
                klog("dump: +%03x %11g %11g %11g %11g   %08x %08x %08x %08x\n", o, v[0], v[1], v[2], v[3], u[0], u[1], u[2], u[3]);
            }
            klog("dump: 42a620 obj %08x flags52 %02x %02x\n", *(u32 *)(uintptr_t)0x42a620u,
                 guest_ptr_ok(*(u32 *)(uintptr_t)0x42a620u) ? *(u8 *)(uintptr_t)(*(u32 *)(uintptr_t)0x42a620u + 0x52) : 0,
                 guest_ptr_ok(*(u32 *)(uintptr_t)0x42a620u) ? *(u8 *)(uintptr_t)(*(u32 *)(uintptr_t)0x42a620u + 0x53) : 0);
            const u32 *inv = (const u32 *)(uintptr_t)0x43cd10u;
            for (int i = 0; i < 40; i += 4) klog("dump: inv +%02x %08x %08x %08x %08x\n", i * 4, inv[i], inv[i+1], inv[i+2], inv[i+3]);
        }
    }
    if (getenv("XBOX_SCANMODE_AT")) {
        static int done;
        u32 o = *(u32 *)(uintptr_t)0x42a620u;
        if (!done && guest_ptr_ok(o) && gl_frame_number() >= parse_u32(getenv("XBOX_SCANMODE_AT"), 10)) {
            done = 1; *(u8 *)(uintptr_t)(o + 0x53) = 1; klog("debug: scan mode flag set\n");
        }
    }
    if (getenv("XBOX_FREECAM_AT")) {          /* automated tests */
        static int done;
        if (!done && gl_frame_number() >= parse_u32(getenv("XBOX_FREECAM_AT"), 10)) { done = 1; g_freecam_enabled = 1; }
    }
    if (!g_warp_pending) return;
    /* the manager's map-state object must exist and be idle (+0x9c == 1); wait for it */
    u32 st = *(u32 *)(uintptr_t)(MAP_MANAGER + 0x78);
    static int waited;
    if (!guest_ptr_ok(st) || *(u32 *)(uintptr_t)(st + 0x9c) != 1) {
        if (++waited == 600) { g_warp_pending = 0; waited = 0;
            snprintf(g_status, sizeof g_status, "The game is busy (loading?), try again in a moment");
            klog("debug: warp to %08x gave up (manager state %08x)\n", g_warp_hash, st); }
        return;
    }
    waited = 0;
    g_warp_pending = 0;
    u32 player = *(u32 *)(uintptr_t)PLAYER_PTR;
    u32 ctx = player ? player + 0x270 : 0;
    u32 r;
    if (g_warp_obj) {           /* a specific map object (maps such as Cloud City share one file) */
        if (g_warp_start != -1) {
            u32 sp = *(u32 *)(uintptr_t)(g_warp_obj + 0x120);
            if (guest_ptr_ok(sp)) ((setstart_fn)(uintptr_t)FN_SETSTART)((void *)(uintptr_t)sp, g_warp_start, 0);
        }
        r = ((queuemap_fn)(uintptr_t)FN_QUEUEMAP)((void *)(uintptr_t)MAP_MANAGER, g_warp_obj, ctx);
    } else r = ((gotomap_fn)(uintptr_t)FN_GOTOMAP)((void *)(uintptr_t)MAP_MANAGER, g_warp_hash, g_warp_start, ctx);
    if (r) snprintf(g_status, sizeof g_status, "Warping to %08x...", g_warp_hash);
    else snprintf(g_status, sizeof g_status, "The game is busy (loading?), try again in a moment");
    klog("debug: warp to map %08x start %d -> %u\n", g_warp_hash, g_warp_start, r);
}

/* ---- menu UI ---- */
void ov_begin(int *w, int *h);
void ov_rect(float x, float y, float w, float h, u32 rgba);
void ov_glow(float x, float y, float r, u32 c);
float ov_text(float x, float y, float size, u32 rgba, const char *s);
void ov_end(void);

static int g_open, g_page, g_sel, g_scroll, g_start_sel;
extern int g_show_fps;

int debugmenu_open(void) { return g_open; }

enum { PG_MAIN, PG_WARP };
enum { MI_WARP, MI_CHAR, MI_COOP, MI_P2CHAR, MI_P3CHAR, MI_P4CHAR, MI_SIZE, MI_UNLOCK, MI_GEMS, MI_HEALTH, MI_INVINC, MI_NOCLIP, MI_SQUASH, MI_FREECAM, MI_MODCAM, MI_STARTRED, MI_DRAWDIST,
       MI_FPS, MI_SHOWFPS, MI_CLOSE, NMAIN };
static const char *main_items[NMAIN] = { "Warp to level...", "Character", "Co-op", "Player 2", "Player 3", "Player 4", "Size", "Abilities & items", "Give 10,000 gems",
                                         "Refill health", "Invincible", "Noclip", "Squashed", "Free camera", "Modern camera", "Start as Red",
                                         "Draw distance", "Frame rate", "Show FPS", "Close" };
/* settings kept between runs: <save folder>/portcfg.txt */
int g_start_red;
extern int g_coop, g_coop_players, g_coop_char[4];   /* coop.c */
static int g_p_sel[4] = { 0, 1, 1, 1 };    /* the menu's choice for players 2..4 (A applies it) */
static void save_portcfg(void)
{
    const char *fs_save_dir(void); char path[600]; snprintf(path, sizeof path, "%s/portcfg.txt", fs_save_dir());
    FILE *f = fopen(path, "w"); if (!f) return;
    fprintf(f, "start_red=%d\ncoop=%d\ncoop_players=%d\ncoop_char2=%d\ncoop_char3=%d\ncoop_char4=%d\n", g_start_red, g_coop, g_coop_players,
            g_coop_char[1], g_coop_char[2], g_coop_char[3]); fclose(f);
}
static void load_portcfg(void)
{
    const char *fs_save_dir(void); char path[600], line[128]; snprintf(path, sizeof path, "%s/portcfg.txt", fs_save_dir());
    FILE *f = fopen(path, "r"); if (!f) return;
    while (fgets(line, sizeof line, f)) {
        sscanf(line, "start_red=%d", &g_start_red); sscanf(line, "coop=%d", &g_coop);
        sscanf(line, "coop_players=%d", &g_coop_players); if (g_coop_players < 2 || g_coop_players > 4) g_coop_players = 2;
        for (int k = 1; k < 4; k++) { char key[24]; snprintf(key, sizeof key, "coop_char%d=%%d", k + 1);
            if (sscanf(line, key, &g_coop_char[k]) == 1 && (g_coop_char[k] < 1 || g_coop_char[k] > NCHARS)) g_coop_char[k] = 1; }
    }
    for (int k = 1; k < 4; k++) g_p_sel[k] = g_coop_char[k];
    fclose(f);
}
static int toggle_item(int i)
{
    switch (i) {
    case MI_UNLOCK: g_unlock = !g_unlock; return 1;
    case MI_INVINC: g_invincible = !g_invincible; return 1;
    case MI_NOCLIP: g_noclip = !g_noclip; if (g_noclip) g_freecam_enabled = 0; return 1;
    case MI_FREECAM: g_freecam_enabled = !g_freecam_enabled; if (g_freecam_enabled) g_noclip = 0; return 1;
    case MI_SHOWFPS: g_show_fps = !g_show_fps; return 1;
    case MI_SQUASH: g_squash = !g_squash; return 1;
    case MI_MODCAM: g_modern_cam = !g_modern_cam; return 1;
    case MI_STARTRED: g_start_red = !g_start_red; save_portcfg(); return 1;
    case MI_COOP:                              /* off -> 2 -> 3 -> 4 players -> off */
        if (!g_coop) { g_coop = 1; g_coop_players = 2; } else if (g_coop_players < 4) g_coop_players++; else g_coop = 0;
        save_portcfg();
        if (g_coop) snprintf(g_status, sizeof g_status, "Co-op: %d players (controllers 2-%d; player 2 can use the keyboard)", g_coop_players, g_coop_players);
        else snprintf(g_status, sizeof g_status, "Co-op off"); return 1;
    }
    return 0;
}

static void cycle_fps(int d)
{
    int i = 0;
    while (i < NFPS && g_fps_choices[i] != g_target_fps) i++;
    if (i == NFPS) i = 0;
    i = (i + d + NFPS) % NFPS;
    g_target_fps = g_fps_choices[i];
}

static void activate(void)
{
    if (g_page == PG_MAIN) {
        switch (g_sel) {
        case MI_WARP: scan_maps(); g_page = PG_WARP; g_sel = 0; g_scroll = 0; break;
        case MI_FREECAM: case MI_NOCLIP:
            toggle_item(g_sel); if (g_freecam_enabled || g_noclip) g_open = 0; break;
        case MI_UNLOCK: case MI_INVINC: case MI_SHOWFPS: case MI_SQUASH: case MI_MODCAM: case MI_STARTRED: case MI_COOP: toggle_item(g_sel); break;
        case MI_P2CHAR: case MI_P3CHAR: case MI_P4CHAR: {
            int k = g_sel - MI_P2CHAR + 1;
            g_coop_char[k] = g_p_sel[k];
            if (!g_coop) { g_coop = 1; g_coop_players = 2; }
            if (g_coop_players < k + 1) g_coop_players = k + 1;
            save_portcfg(); snprintf(g_status, sizeof g_status, "Player %d: %s", k + 1, g_char_names[g_coop_char[k]]); g_open = 0; break; }
        case MI_CHAR:
            if (!guest_ptr_ok(*(u32 *)(uintptr_t)PLAYER_OBJ)) { snprintf(g_status, sizeof g_status, "Start or load a game first"); break; }
            g_char_req = g_char_sel; g_open = 0; break;
        case MI_DRAWDIST: cycle_drawdist(1); break;
        case MI_SIZE: cycle_size(1); break;
        case MI_GEMS: g_cheat_req |= 2; break;
        case MI_HEALTH: g_cheat_req |= 4; break;
        case MI_FPS: cycle_fps(1); break;
        case MI_CLOSE: g_open = 0; break;
        }
    } else if (g_page == PG_WARP && g_sel < g_nmaps) {
        struct mapent *m = &g_maps[g_sel];
        if (!m->shipped) { snprintf(g_status, sizeof g_status, "%s is not on the disc", m->file); return; }
        u32 st = *(u32 *)(uintptr_t)(MAP_MANAGER + 0x78);
        if (!guest_ptr_ok(st)) { snprintf(g_status, sizeof g_status, "Start or load a game first, then warp"); return; }
        request_warp_obj(m, g_starts[g_start_sel].id);
        g_open = 0;
    }
}

/* navigation input: 1 up, 2 down, 3 left, 4 right, 5 accept, 6 back */
void debugmenu_nav(int k)
{
    if (!g_open) return;
    int n = g_page == PG_MAIN ? NMAIN : g_nmaps;
    switch (k) {
    case 1: g_sel = (g_sel + n - 1) % (n ? n : 1); break;
    case 2: g_sel = (g_sel + 1) % (n ? n : 1); break;
    case 3: case 4:
        if (g_page == PG_WARP) g_start_sel = (g_start_sel + (k == 3 ? NSTARTS - 1 : 1)) % NSTARTS;
        else if (g_sel == MI_FPS) cycle_fps(k == 3 ? -1 : 1);
        else if (g_sel == MI_CHAR) g_char_sel = (g_char_sel - 1 + (k == 3 ? NCHARS - 1 : 1)) % NCHARS + 1;
        else if (g_sel >= MI_P2CHAR && g_sel <= MI_P4CHAR) { int *ps = &g_p_sel[g_sel - MI_P2CHAR + 1]; *ps = (*ps - 1 + (k == 3 ? NCHARS - 1 : 1)) % NCHARS + 1; }
        else if (g_sel == MI_COOP && k == 3) {          /* left: 4 -> 3 -> 2 players -> off */
            if (g_coop && g_coop_players > 2) g_coop_players--; else if (g_coop) g_coop = 0; else { g_coop = 1; g_coop_players = 4; }
            save_portcfg(); }
        else if (g_sel == MI_DRAWDIST) cycle_drawdist(k == 3 ? -1 : 1);
        else if (g_sel == MI_SIZE) cycle_size(k == 3 ? -1 : 1);
        else toggle_item(g_sel);
        break;
    case 5: activate(); break;
    case 6: if (g_page == PG_WARP) { g_page = PG_MAIN; g_sel = 0; } else g_open = 0; break;
    }
}

void debugmenu_toggle(void)
{
    g_open = !g_open;
    if (g_open) { g_page = PG_MAIN; g_sel = 0; g_status[0] = 0; }
}

/* ---- Red's staff effects (drawn over the frame: his own particle effects are fired by his boss
 * logic, which doesn't run for the player) - a glowing trail from the staff tip while it swings or
 * he dashes, a pulsing glow while he casts. World -> screen with the frame's world transform. */
extern float g_staff_w[3], g_staff_ax[3]; extern int g_staff_ok;
RS int g_fx_trail, g_fx_cast;               /* set by redchar_animate */
/* ---- the slash trail: a ribbon swept by the staff's blade (grip to crystal) through each swing,
 * smoothed between frames, fading from its newest edge back and toward the grip; a coloured
 * glow (the breath's element) with a white-hot edge along the crystal's path ---- */
RS int g_fx_slash;                          /* set by redchar_animate while a swing cuts */
char g_sc_label[32];                     /* (research: the pose gallery's label) */
void ov_tri(float x0, float y0, u32 c0, float x1, float y1, u32 c1, float x2, float y2, u32 c2);
void ov_additive(int on);
static void slash_draw(void)
{
    #define SL_N 40
    static RS struct { float in[3], out[3]; double t; } sl[SL_N]; static RS int n;
    extern float g_staff_head[3], g_staff_grip[3]; extern int g_staff_head_ok;
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); double now = ts.tv_sec + ts.tv_nsec * 1e-9;
    static float life = 0; if (!life) life = getenv("XBOX_SLASH_LIFE") ? atof(getenv("XBOX_SLASH_LIFE")) : 0.26f;
    u32 pl = *(u32 *)(uintptr_t)PLAYER_OBJ; if (!guest_ptr_ok(pl)) return;
    u32 item = *(u32 *)(uintptr_t)(pl + 4); if (!guest_ptr_ok(item) || !g_redchar_anim) return;
    if (g_fx_slash && g_staff_head_ok) {
        const float *P = (const float *)(uintptr_t)(item + 0xc8), *sc = (const float *)(uintptr_t)(g_redchar_anim + 0xe8);
        float yaw = *(float *)(uintptr_t)(item + 0xdc), c = cosf(yaw), s = sinf(yaw);
        float a[3], b[3];
        for (int k = 0; k < 3; k++) { float d = g_staff_head[k] - g_staff_grip[k]; a[k] = g_staff_grip[k] + d * 0.15f; b[k] = g_staff_head[k] + d * 0.22f; }
        if (n == SL_N) { memmove(sl, sl + 1, sizeof sl[0] * (SL_N - 1)); n--; }
        float *q[2] = { a, b }, *o[2] = { sl[n].in, sl[n].out };
        for (int j = 0; j < 2; j++) {
            float x = q[j][0] * sc[0], y = q[j][1] * sc[1], z = q[j][2] * sc[2];
            o[j][0] = P[0] + x * c + z * s; o[j][1] = P[1] + y; o[j][2] = P[2] - x * s + z * c;
        }
        sl[n].t = now; n++;
    }
    while (n && now - sl[0].t > life) { memmove(sl, sl + 1, sizeof sl[0] * (n - 1)); n--; }
    if (g_sc_label[0]) { int w0, h0; ov_begin(&w0, &h0); ov_text(20, 20, h0 / 14.0f, 0xffffffff, g_sc_label); ov_end(); }
    if (n < 2) return;
    u32 rc = *(u32 *)(uintptr_t)0x4703f0u; if (!guest_ptr_ok(rc)) return;
    const float *VP = (const float *)(uintptr_t)(rc + 0x8f4);
    extern int g_pres_rect[4];
    int w, h; ov_begin(&w, &h);
    float pw = g_pres_rect[2] ? g_pres_rect[2] : w, ph = g_pres_rect[3] ? g_pres_rect[3] : h;
    u32 type = *(u32 *)(uintptr_t)0x43cd10u;
    u32 col = type == 4 ? 0x60d8ff : type == 2 ? 0xfff070 : type == 8 ? 0x4090ff : 0xff7a20;
    /* smoothed: 4 steps between samples on a Catmull-Rom curve, then to the screen */
    enum { SUB = 4 };
    static float sx[SL_N * SUB][2][2]; static float sa[SL_N * SUB]; static int ok[SL_N * SUB]; int m = 0;
    for (int i = 0; i + 1 < n; i++)
        for (int k = 0; k < SUB; k++) {
            float u = k / (float)SUB, u2 = u * u, u3 = u2 * u;
            int i0 = i > 0 ? i - 1 : 0, i3 = i + 2 < n ? i + 2 : n - 1;
            float age = (float)(now - (sl[i].t + (sl[i + 1].t - sl[i].t) * u)) / life;
            sa[m] = age < 0 ? 1 : age > 1 ? 0 : (1 - age) * (1.6f - 0.6f * age);
            ok[m] = 1;
            for (int j = 0; j < 2; j++) {
                float p[3];
                for (int e = 0; e < 3; e++) {
                    float p0 = j ? sl[i0].out[e] : sl[i0].in[e], p1 = j ? sl[i].out[e] : sl[i].in[e];
                    float p2 = j ? sl[i + 1].out[e] : sl[i + 1].in[e], p3 = j ? sl[i3].out[e] : sl[i3].in[e];
                    p[e] = 0.5f * (2 * p1 + (p2 - p0) * u + (2 * p0 - 5 * p1 + 4 * p2 - p3) * u2 + (3 * p1 - p0 - 3 * p2 + p3) * u3);
                }
                float cx = p[0] * VP[0] + p[1] * VP[4] + p[2] * VP[8] + VP[12], cy = p[0] * VP[1] + p[1] * VP[5] + p[2] * VP[9] + VP[13];
                float cw = p[0] * VP[3] + p[1] * VP[7] + p[2] * VP[11] + VP[15];
                if (cw < 0.1f) { ok[m] = 0; continue; }
                sx[m][j][0] = g_pres_rect[0] + (cx / cw * 0.5f + 0.5f) * pw; sx[m][j][1] = g_pres_rect[1] + (0.5f - cy / cw * 0.5f) * ph;
            }
            m++;
            if (i + 2 == n && k == SUB - 1) {                 /* and the newest sample itself */
                float age = (float)(now - sl[n - 1].t) / life; sa[m] = age > 1 ? 0 : (1 - age) * (1.6f - 0.6f * age); ok[m] = 1;
                float *q[2] = { sl[n - 1].in, sl[n - 1].out };
                for (int j = 0; j < 2; j++) {
                    const float *p = q[j];
                    float cx = p[0] * VP[0] + p[1] * VP[4] + p[2] * VP[8] + VP[12], cy = p[0] * VP[1] + p[1] * VP[5] + p[2] * VP[9] + VP[13];
                    float cw = p[0] * VP[3] + p[1] * VP[7] + p[2] * VP[11] + VP[15];
                    if (cw < 0.1f) { ok[m] = 0; continue; }
                    sx[m][j][0] = g_pres_rect[0] + (cx / cw * 0.5f + 0.5f) * pw; sx[m][j][1] = g_pres_rect[1] + (0.5f - cy / cw * 0.5f) * ph;
                }
                m++;
            }
        }
    ov_additive(1);
    for (int pass = 0; pass < 2; pass++) {               /* the glow, then the hot edge on the crystal's side */
        float f0 = pass ? 0.62f : 0.0f;
        for (int i = 0; i + 1 < m; i++) {
            if (!ok[i] || !ok[i + 1]) continue;
            float A[2][2], B[2][2];                          /* inner / outer edge at i and i+1 */
            for (int e = 0; e < 2; e++) {
                A[0][e] = sx[i][0][e] + (sx[i][1][e] - sx[i][0][e]) * f0; A[1][e] = sx[i][1][e];
                B[0][e] = sx[i + 1][0][e] + (sx[i + 1][1][e] - sx[i + 1][0][e]) * f0; B[1][e] = sx[i + 1][1][e];
            }
            float a0 = sa[i], a1 = sa[i + 1];
            u32 rgb = pass ? 0xfff6e8 : col;
            float k = pass ? 1.0f : 0.9f, inner = pass ? 0.2f : 0.05f;
            if (a0 > 1) a0 = 1; if (a1 > 1) a1 = 1;
            u32 ci0 = (u32)(255 * a0 * k * inner) << 24 | rgb, co0 = (u32)(255 * a0 * k) << 24 | rgb;
            u32 ci1 = (u32)(255 * a1 * k * inner) << 24 | rgb, co1 = (u32)(255 * a1 * k) << 24 | rgb;
            ov_tri(A[0][0], A[0][1], ci0, A[1][0], A[1][1], co0, B[1][0], B[1][1], co1);
            ov_tri(A[0][0], A[0][1], ci0, B[1][0], B[1][1], co1, B[0][0], B[0][1], ci1);
        }
    }
    if (ok[m - 1] && sa[m - 1] > 0.05f) {               /* a flare where the crystal is now */
        float a = sa[m - 1] > 1 ? 1 : sa[m - 1], r = ph * 0.035f;
        ov_glow(sx[m - 1][1][0], sx[m - 1][1][1], r * 2.2f, (u32)(150 * a) << 24 | col);
        ov_glow(sx[m - 1][1][0], sx[m - 1][1][1], r, (u32)(220 * a) << 24 | 0xfff6e8);
    }
    ov_end();
}
static void red_fx_draw(void)
{
    slash_draw();
    { void redcast_draw(void), redshrink_draw(void); redcast_draw(); redshrink_draw(); }
    if (getenv("XBOX_FX_LOG")) { static u32 nl; if ((g_fx_trail || g_fx_cast) && nl++ % 15 == 0) klog("fx state ok %d anim %08x\n", g_staff_ok, g_redchar_anim); }
    if (!g_redchar_anim || !g_staff_ok) return;
    static RS struct { float p[3]; double t; } tr[24]; static RS int ntr;
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); double now = ts.tv_sec + ts.tv_nsec * 1e-9;
    static float len = 0; if (!len) len = getenv("XBOX_STAFF_LEN") ? atof(getenv("XBOX_STAFF_LEN")) : 1.1f;
    float al = sqrtf(g_staff_ax[0] * g_staff_ax[0] + g_staff_ax[1] * g_staff_ax[1] + g_staff_ax[2] * g_staff_ax[2]);
    if (al < 1e-4f) return;
    static float fx_size = 0; if (!fx_size) fx_size = getenv("XBOX_FX_SIZE") ? atof(getenv("XBOX_FX_SIZE")) : 2.2f;
    extern float g_staff_head[3]; extern int g_staff_head_ok; (void)len;
    if (!g_staff_head_ok) return;
    float tl[3]; for (int k = 0; k < 3; k++) tl[k] = g_staff_head[k];   /* model space */
    /* to the world: Red's model scale (animator +0xe8), then the item's world matrix (+0x118, rows) */
    u32 pl = *(u32 *)(uintptr_t)PLAYER_OBJ; if (!guest_ptr_ok(pl)) return;
    u32 item = *(u32 *)(uintptr_t)(pl + 4); if (!guest_ptr_ok(item)) return;
    const float *P = (const float *)(uintptr_t)(item + 0xc8), *sc = (const float *)(uintptr_t)(g_redchar_anim + 0xe8);
    float yaw = *(float *)(uintptr_t)(item + 0xdc), cy_ = cosf(yaw), sy_ = sinf(yaw);
    float lx = tl[0] * sc[0], ly = tl[1] * sc[1], lz = tl[2] * sc[2];
    float tip[3] = { P[0] + lx * cy_ + lz * sy_, P[1] + ly, P[2] - lx * sy_ + lz * cy_ };   /* model -z = forward (-sin, -cos) */
    if (getenv("XBOX_FX_LOG")) { static u32 nl; if (nl++ % 30 == 0) { const float *m = g_dbg_world_m; float r[4];
        for (int j = 0; j < 4; j++) r[j] = P[0] * m[j] + P[1] * m[4 + j] + P[2] * m[8 + j] + m[12 + j];
        float c[4]; for (int j = 0; j < 4; j++) c[j] = P[0] * m[j * 4] + P[1] * m[j * 4 + 1] + P[2] * m[j * 4 + 2] + m[j * 4 + 3];
        klog("fx proj pos rows %.1f %.1f %.1f %.1f  cols %.1f %.1f %.1f %.1f\n", r[0], r[1], r[2], r[3], c[0], c[1], c[2], c[3]); } }
    if (getenv("XBOX_FX_LOG")) { static u32 nl; if (nl++ % 30 == 0) klog("fx item pos %.1f %.1f %.1f M12 %.1f %.1f %.1f sc %.2f\n",
        ((float *)(uintptr_t)(item + 0xc8))[0], ((float *)(uintptr_t)(item + 0xc8))[1], ((float *)(uintptr_t)(item + 0xc8))[2], tip[0], tip[1], tip[2], sc[0]); }
    if ((g_fx_trail && !g_fx_slash) || g_fx_cast) {
        if (ntr == 24) { memmove(tr, tr + 1, sizeof tr[0] * 23); ntr--; }
        memcpy(tr[ntr].p, tip, 12); tr[ntr].t = now; ntr++;
    }
    while (ntr && now - tr[0].t > 0.25) { memmove(tr, tr + 1, sizeof tr[0] * (ntr - 1)); ntr--; }
    if (getenv("XBOX_FX_CAL")) {          /* research: markers to calibrate model -> world */
        u32 rc0 = *(u32 *)(uintptr_t)0x4703f0u; const float *VP0 = (const float *)(uintptr_t)(rc0 + 0x8f4);
        extern int g_pres_rect[4]; int w0, h0; ov_begin(&w0, &h0);
        float hyp[5][3];
        for (int k = 0; k < 3; k++) hyp[0][k] = P[k];
        float S = getenv("XBOX_FX_CAL")[0] == '2' ? 2.2f : 1.0f;
        extern float g_staff_cal[4][3]; (void)S;
        for (int hh = 0; hh < 4; hh++) {        /* 0 staff origin (white), 1 +axis (red), 2 -axis (blue), 3 hand-ish (green) */
            float q[3]; for (int k = 0; k < 3; k++) q[k] = g_staff_cal[hh][k];
            float x = q[0] * sc[0], y = q[1] * sc[1], z = q[2] * sc[2];
            hyp[hh][0] = P[0] + x * cy_ + z * sy_; hyp[hh][1] = P[1] + y; hyp[hh][2] = P[2] - x * sy_ + z * cy_;
        }
        hyp[4][0] = P[0]; hyp[4][1] = P[1]; hyp[4][2] = P[2];
        static const u32 col[5] = { 0xffffffff, 0xffff0000, 0xff0000ff, 0xff00ff00, 0xffffff00 };
        for (int i = 0; i < 5; i++) {
            const float *q = hyp[i];
            float X = q[0] * VP0[0] + q[1] * VP0[4] + q[2] * VP0[8] + VP0[12], Y = q[0] * VP0[1] + q[1] * VP0[5] + q[2] * VP0[9] + VP0[13];
            float W = q[0] * VP0[3] + q[1] * VP0[7] + q[2] * VP0[11] + VP0[15];
            if (W <= 0) continue;
            ov_glow(g_pres_rect[0] + (X / W * 0.5f + 0.5f) * g_pres_rect[2], g_pres_rect[1] + (0.5f - Y / W * 0.5f) * g_pres_rect[3], 9, col[i]);
        }
        ov_end();
    }
    if (!ntr && !g_fx_cast) return;
    u32 rc = *(u32 *)(uintptr_t)0x4703f0u; if (!guest_ptr_ok(rc)) return;
    const float *VP = (const float *)(uintptr_t)(rc + 0x8f4);       /* the game's view-projection (rows) */
    int w, h; ov_begin(&w, &h);
    for (int i = 0; i < ntr + (g_fx_cast ? 1 : 0); i++) {
        const float *p = i < ntr ? tr[i].p : tip;
        float cx = p[0] * VP[0] + p[1] * VP[4] + p[2] * VP[8] + VP[12], cy = p[0] * VP[1] + p[1] * VP[5] + p[2] * VP[9] + VP[13];
        float cw = p[0] * VP[3] + p[1] * VP[7] + p[2] * VP[11] + VP[15];
        if (getenv("XBOX_FX_LOG")) { static u32 nl; if (nl++ % 15 == 0) klog("fx tip %.1f %.1f %.1f staff %.1f %.1f %.1f clip %.2f %.2f %.2f trail %d cast %d\n", p[0], p[1], p[2],
            g_staff_w[0], g_staff_w[1], g_staff_w[2], cx, cy, cw, g_fx_trail, g_fx_cast); }
        if (cw < 0.1f) continue;
        extern int g_pres_rect[4];
        float pw = g_pres_rect[2] ? g_pres_rect[2] : w, phh = g_pres_rect[3] ? g_pres_rect[3] : h;
        static int ydown = -1; if (ydown < 0) ydown = getenv("XBOX_FX_YDOWN") ? 1 : 0;   /* (screen y = 1 - ndc y) */
        float ny = cy / cw * 0.5f + 0.5f;
        float sx = g_pres_rect[0] + (cx / cw * 0.5f + 0.5f) * pw, sy = g_pres_rect[1] + (ydown ? ny : 1 - ny) * phh;
        const float *cam = (const float *)(uintptr_t)(rc + 0x934);     /* the camera position */
        float dd = sqrtf((p[0] - cam[0]) * (p[0] - cam[0]) + (p[1] - cam[1]) * (p[1] - cam[1]) + (p[2] - cam[2]) * (p[2] - cam[2]));
        float r = phh * 0.9f / (dd > 1 ? dd : 1) * fx_size; if (r > phh * 0.08f) r = phh * 0.08f;
        if (i < ntr) {
            float age = (float)(now - tr[i].t) / 0.25f; if (age > 1) age = 1;
            u32 a = (u32)(235 * (1 - age));
            ov_glow(sx, sy, r * (1 - 0.4f * age), a << 24 | 0xff6a1a);       /* ember orange */
            ov_glow(sx, sy, r * 0.45f * (1 - 0.5f * age), a << 24 | 0xfff0c0);
        } else {
            float pulse = 0.75f + 0.25f * sinf((float)now * 14);
            ov_glow(sx, sy, r * 2.2f * pulse, 0x90b040ffu);                   /* magic: violet */
            ov_glow(sx, sy, r * 0.9f * pulse, 0xe0f0d0ffu);
        }
    }
    ov_end();
}

void debugmenu_draw(void)
{
    {   /* co-op split screen: each half gets its player's Red effects, meter and reticle */
        int coop_overlay_begin(int v); void coop_overlay_end(void);
        for (int v = 0;;) {
            int next = coop_overlay_begin(v);
            if (!next) { if (!v) red_fx_draw(); break; }
            red_fx_draw(); coop_overlay_end(); v = next;
        }
    }
    if (!g_open) {
        if (g_status[0]) {                               /* brief status line after closing */
            static u32 shown; extern u32 gl_frame_number(void);
            if (!shown) shown = gl_frame_number();
            if (gl_frame_number() - shown > 180) { g_status[0] = 0; shown = 0; return; }
            int w, h; ov_begin(&w, &h);
            float sz = h / 36.0f;
            ov_rect(0, h - sz * 1.6f, w, sz * 1.6f, 0xA0000000);
            ov_text(sz, h - sz * 1.3f, sz, 0xFFFFFFFF, g_status);
            ov_end();
        }
        return;
    }
    int w, h;
    ov_begin(&w, &h);
    float sz = h / 32.0f, lh = sz * 1.25f;
    float bw = sz * 34, bx = (w - bw) / 2, by = sz * 1.5f;
    int rows_vis = (int)((h - by * 2 - lh * 5) / lh);
    if (rows_vis < 4) rows_vis = 4;
    int n = g_page == PG_MAIN ? NMAIN : g_nmaps;
    int shown = n < rows_vis ? n : rows_vis;
    float bh = lh * (shown + 4.5f);
    ov_rect(bx, by, bw, bh, 0xD0101018);
    ov_rect(bx, by, bw, lh * 1.3f, 0xFF6A2E8C);
    ov_text(bx + sz * 0.6f, by + lh * 0.15f, sz, 0xFFFFFFFF, g_page == PG_MAIN ? "PORT DEBUG MENU" : "WARP TO LEVEL");
    float y = by + lh * 1.6f;
    if (g_sel < g_scroll) g_scroll = g_sel;
    if (g_sel >= g_scroll + rows_vis) g_scroll = g_sel - rows_vis + 1;
    char line[160];
    for (int i = g_scroll; i < n && i < g_scroll + rows_vis; i++, y += lh) {
        u32 col = 0xFFE0E0E0;
        if (g_page == PG_MAIN) {
            const int *tg = i == MI_FREECAM ? &g_freecam_enabled : i == MI_NOCLIP ? &g_noclip : i == MI_UNLOCK ? &g_unlock :
                            i == MI_INVINC ? &g_invincible : i == MI_SQUASH ? &g_squash : i == MI_MODCAM ? &g_modern_cam : i == MI_STARTRED ? &g_start_red : NULL;
            if (tg) snprintf(line, sizeof line, "%-18s %s", main_items[i], *tg ? "ON" : "off");
            else if (i == MI_FPS) snprintf(line, sizeof line, "%-18s < %d >%s", main_items[i], g_target_fps,
                                          g_target_fps == 60 ? "  (original)" : "  (experimental)");
            else if (i == MI_CHAR) {
                u32 cur = player_type();
                snprintf(line, sizeof line, "%-18s < %s >%s", main_items[i], g_char_names[g_char_sel],
                         g_char_loading ? "  loading..." : cur == (u32)g_char_sel ? "  (current)" : "  A = switch");
            }
            else if (i == MI_COOP) { if (g_coop) snprintf(line, sizeof line, "%-18s < %d players >", main_items[i], g_coop_players);
                                     else snprintf(line, sizeof line, "%-18s < off >", main_items[i]); }
            else if (i >= MI_P2CHAR && i <= MI_P4CHAR) { u32 coop_player(int k); int k = i - MI_P2CHAR + 1;
                snprintf(line, sizeof line, "%-18s < %s >%s", main_items[i], g_char_names[g_p_sel[k]],
                         !g_coop || g_coop_players <= k ? "  A = join" : coop_player(k) && g_p_sel[k] == g_coop_char[k] ? "  (current)" : "  A = switch"); }
            else if (i == MI_SIZE) snprintf(line, sizeof line, "%-18s < %s >", main_items[i], size_name());
            else if (i == MI_DRAWDIST) snprintf(line, sizeof line, "%-18s < %s >", main_items[i], drawdist_name());
            else if (i == MI_SHOWFPS) snprintf(line, sizeof line, "%-18s %s", main_items[i], g_show_fps ? "ON" : "off");
            else snprintf(line, sizeof line, "%s", main_items[i]);
        } else {
            struct mapent *m = &g_maps[i];
            if (m->section && (i == 0 || g_maps[i - 1].section != m->section)) { }
            snprintf(line, sizeof line, "%-34.34s %s", m->name, m->file);
            if (!m->shipped) col = 0xFF707070;
            else if (m->section) col = 0xFFB0B0C8;
        }
        if (i == g_sel) ov_rect(bx + sz * 0.3f, y - lh * 0.08f, bw - sz * 0.6f, lh, 0xFF3A6EA5);
        ov_text(bx + sz * 0.8f, y, sz, col, line);
    }
    y = by + bh - lh * 2.6f;
    if (g_page == PG_WARP) {
        snprintf(line, sizeof line, "Start point: < %s >", g_starts[g_start_sel].name);
        ov_text(bx + sz * 0.8f, y, sz, 0xFFFFD060, line);
    }
    ov_text(bx + sz * 0.8f, y + lh, sz * 0.8f, 0xFFA0A0A0,
            g_page == PG_MAIN ? "Up/Down select  A/Enter choose  B/Esc close" : "A/Enter warp  Left/Right start point  B/Esc back");
    if (g_status[0]) ov_text(bx + sz * 0.8f, by + bh + lh * 0.3f, sz * 0.8f, 0xFFFF8080, g_status);
    ov_end();
}
