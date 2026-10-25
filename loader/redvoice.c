/* Red's voice: his grunts, roars and laughs live in the sound bank of his boss level
 * (eng_sb_realm3z.sfx, inside the Filelist archive), which is only loaded there. The bank is read
 * here straight from the archive, the samples decoded (Eurocom Xbox IMA ADPCM) and played through
 * our own mixer (audio_sdl.c sfx_pcm), so he can be heard in any level.
 *
 * Bank (MUSX, "XB__"): header +0x20/+0x24 sfx table off/size, +0x28/+0x2c sample table, +0x30
 * sample data. Sfx table: count, then {u32 hash (0x00f0xxxx; the game's ids are 0x1af0xxxx),
 * u32 offset}; an sfx record = 16 bytes, u16 count, count x {u16 sample, ...6 bytes} (random
 * variants). Sample table: count, then 0x20-byte {?, offset, size, rate, size, ...}.
 * Sample data: 32-byte blocks: s16 sample, u8 step index, u8 block letter ('A'..'Z'), then 28
 * bytes = 56 4-bit IMA codes, high nibble first.
 *
 * Filelist.bin (version 7): see tools/filelist.py. */
#include "xbox.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

const char *fs_find(const char *name);
void sfx_pcm(const s16 *pcm, int n, int rate, float vol, float pan);

static u8 *read_file(const char *path, long *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); *len = ftell(f); fseek(f, 0, SEEK_SET);
    u8 *d = malloc(*len);
    if (d && fread(d, 1, *len, f) != (size_t)*len) { free(d); d = NULL; }
    fclose(f);
    return d;
}
static u32 rv32(const u8 *p) { u32 v; memcpy(&v, p, 4); return v; }

/* one file out of the Filelist archive, by name */
static u8 *filelist_get(const char *want, long *len)
{
    long blen; const char *bp = fs_find("Filelist.bin");
    u8 *b = bp ? read_file(bp, &blen) : NULL;
    if (!b) return NULL;
    u32 n = rv32(b + 8), nameoff = rv32(b + 0x10);
    u32 o = 0x14, t = 0x10 + nameoff;
    u8 *out = NULL;
    for (u32 k = 0; k < n && !out; k++) {
        u32 ln = rv32(b + o), nloc = rv32(b + o + 16);
        u32 off = nloc ? rv32(b + o + 20) : 0, arc = nloc ? rv32(b + o + 24) : 0;
        o += 20 + 8 * nloc;
        u32 rel = rv32(b + t + 4 * k); const u8 *np = b + t + 4 * k + rel;
        char name[256]; int i;
        for (i = 0; i < 255; i++) { char c = (char)((np[i] - (0xEA + k + i)) & 0xFF); name[i] = c; if (!c) break; }
        name[255] = 0;
        const char *base = strrchr(name, '\\'); base = base ? base + 1 : name;
        if (strcasecmp(base, want) || !nloc) continue;
        char an[32]; snprintf(an, sizeof an, "Filelist.%03u", arc);
        const char *ap = fs_find(an);
        FILE *f = ap ? fopen(ap, "rb") : NULL;
        if (!f) break;
        out = malloc(ln);
        if (fseek(f, off, SEEK_SET) || fread(out, 1, ln, f) != ln) { free(out); out = NULL; }
        else *len = ln;
        fclose(f);
    }
    free(b);
    return out;
}

static const int g_step[89] = { 7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,50,55,60,66,73,80,88,97,107,118,130,
    143,157,173,190,209,230,253,279,307,337,371,408,449,494,544,598,658,724,796,876,963,1060,1166,1282,1411,1552,1707,1878,2066,
    2272,2499,2749,3024,3327,3660,4026,4428,4871,5358,5894,6484,7132,7845,8630,9493,10442,11487,12635,13899,15289,16818,18500,
    20350,22385,24623,27086,29794,32767 };
static const int g_idxtab[8] = { -1, -1, -1, -1, 2, 4, 6, 8 };
static s16 *decode(const u8 *d, u32 size, int *n)
{
    s16 *out = malloc((size / 32) * 56 * sizeof(s16)), *o = out;
    for (u32 b = 0; b + 32 <= size; b += 32) {
        int pred = (s16)(d[b] | d[b + 1] << 8), idx = d[b + 2] > 88 ? 88 : d[b + 2];
        for (int i = 4; i < 32; i++)
            for (int h = 0; h < 2; h++) {
                int nib = h ? d[b + i] & 15 : d[b + i] >> 4, st = g_step[idx], diff = st >> 3;
                if (nib & 1) diff += st >> 2;
                if (nib & 2) diff += st >> 1;
                if (nib & 4) diff += st;
                pred += nib & 8 ? -diff : diff;
                if (pred > 32767) pred = 32767; if (pred < -32768) pred = -32768;
                idx += g_idxtab[nib & 7]; if (idx < 0) idx = 0; if (idx > 88) idx = 88;
                *o++ = (s16)pred;
            }
    }
    *n = (int)(o - out);
    return out;
}

/* his sounds: the game's sfx id (low 16 bits) -> the bank's variants */
#define MAXV 12
static struct snd { u32 id; int nv; s16 *pcm[MAXV]; int len[MAXV], rate[MAXV]; } g_snd[64];
static int g_nsnd, g_loaded;
static void load_bank(void)
{
    if (g_loaded) return;
    g_loaded = 1;
    long len; u8 *d = filelist_get("eng_sb_realm3z.sfx", &len);
    if (!d) { klog("redvoice: Red's sound bank not found in the Filelist\n"); return; }
    u32 st = rv32(d + 0x20), sm = rv32(d + 0x28), sd = rv32(d + 0x30);
    u32 nsfx = rv32(d + st), nsm = rv32(d + sm);
    for (u32 i = 0; i < nsfx && g_nsnd < 64; i++) {
        u32 h = rv32(d + st + 4 + 8 * i), off = rv32(d + st + 8 + 8 * i);
        const u8 *r = d + st + off;
        int nv = r[16] | r[17] << 8;
        struct snd *s = &g_snd[g_nsnd];
        s->id = h & 0xffff; s->nv = 0;
        for (int v = 0; v < nv && s->nv < MAXV; v++) {
            u32 si = r[18 + 8 * v] | r[19 + 8 * v] << 8;
            if (si >= nsm) continue;
            const u8 *e = d + sm + 4 + 0x20 * si;
            u32 so = rv32(e + 4), ss = rv32(e + 8), rate = rv32(e + 12);
            if (sd + so + ss > (u32)len) continue;
            s->pcm[s->nv] = decode(d + sd + so, ss, &s->len[s->nv]);
            s->rate[s->nv] = (int)rate; s->nv++;
        }
        if (s->nv) g_nsnd++;
    }
    free(d);
    klog("redvoice: %d sounds from Red's bank\n", g_nsnd);
}

/* through the game's own sound system when his bank is loaded (his boss level): positioned and at
 * the game's own volume; 0x154350 (cdecl: hash, item) is what his boss code uses */
float g_red_vox_gain = 0.55f;          /* our own playback, matched to the game's level */
static int game_sound(u32 id, u32 item)
{
    extern double g_redboss_time;           /* his bank is only loaded where he is (his boss levels) */
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    if (!item || getenv("XBOX_VOX_OURS") || ts.tv_sec + ts.tv_nsec * 1e-9 - g_redboss_time > 1.0) return 0;
    char ok = ((char (*)(u32, u32))(uintptr_t)0x154350u)(0x1af00000u | (id & 0xffff), item);
    if (getenv("XBOX_VOX_LOG")) klog("redvoice: game sound %x -> %d\n", id, ok);
    return ok != 0;
}
void red_sound(u32 id, float vol, float pan);
void red_sound_at(u32 id, u32 item, float vol, float pan)
{
    if (game_sound(id, item)) return;
    red_sound(id, vol, pan);
}
/* play one of his sounds (game sfx id, e.g. 0x231), a random variant */
void red_sound(u32 id, float vol, float pan)
{
    vol *= g_red_vox_gain;
    load_bank();
    for (int i = 0; i < g_nsnd; i++)
        if (g_snd[i].id == (id & 0xffff)) {
            static int last[64];
            int v = rand() % g_snd[i].nv;
            if (g_snd[i].nv > 1 && v == last[i]) v = (v + 1) % g_snd[i].nv;
            last[i] = v;
            sfx_pcm(g_snd[i].pcm[v], g_snd[i].len[v], g_snd[i].rate[v], vol, pan);
            return;
        }
}
void red_voice_preload(void) { load_bank(); }

/* research: the game's two sound starts (2D 0x1cd730, positioned 0x1cd890), hash = first arg */
void sfx_seen(u32 hash, u32 kind)
{
    extern u32 g_redchar_cur_pub; extern float g_red_progress_pub;
    klog("sfx %s %08x (red clip %08x at %.2f)\n", kind ? "3d" : "2d", hash, g_redchar_cur_pub, g_red_progress_pub);
}
#define SFX_STUB(n, addr) \
    __asm__(".globl sfx_stub" #n "\n" "sfx_stub" #n ":\n" \
            "  pushal\n  pushl $" #n "\n  pushl 40(%esp)\n  call sfx_seen\n  addl $8, %esp\n  popal\n" \
            "  pushl %ebx\n  pushl %ebp\n  pushl %esi\n  pushl %edi\n  movl %ecx, %edi\n" \
            "  pushl $" #addr "+6\n  ret\n"); \
    void sfx_stub##n(void);
SFX_STUB(0, 0x1cd730)
SFX_STUB(1, 0x1cd890)
void redvoice_install(void)
{
    if (!getenv("XBOX_SFX_LOG")) return;
    static const u32 at[2] = { 0x1cd730, 0x1cd890 };
    void (*fn[2])(void) = { sfx_stub0, sfx_stub1 };
    for (int i = 0; i < 2; i++) {
        u8 *p = (u8 *)(uintptr_t)at[i];
        if (p[0] != 0x53 || p[4] != 0x8b) { klog("redvoice: unexpected bytes at %x\n", at[i]); continue; }
        u32 rel = (u32)(uintptr_t)fn[i] - (at[i] + 5);
        p[0] = 0xe9; memcpy(p + 1, &rel, 4); p[5] = 0x90;
    }
}
