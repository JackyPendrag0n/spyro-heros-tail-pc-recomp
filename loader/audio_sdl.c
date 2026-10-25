/* SDL2 audio output: the DirectSound HLE buffers are mixed in the device callback, and buffer
 * play cursors advance with the samples the device actually consumes (so the game's software
 * mixer, which refills its ring buffers just ahead of the cursor, stays in step). */
#include "xbox.h"
#include <SDL2/SDL.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

void ds_mix(float *out, int frames, int out_rate);
void xmv_mix(float *out, int frames);
extern int g_audio_live;

static SDL_AudioDeviceID g_dev;
static int g_rate;
static float g_master = 1.0f;

/* ---- the port's own sound effects (Red's staff swings, wing beats) ----
 * A swoosh: white noise through a band-pass filter whose centre sweeps up then down, with a
 * quick attack and a longer tail, panned a little. Up to 4 at once; started from the game thread
 * (sfx_swoosh), rendered here. */
#define NSW 4
static struct sw { volatile int on; float t, dur, f0, f1, vol, pan, lp, bp; unsigned seed; } g_sw[NSW];
void sfx_swoosh(float peak_hz, float dur, float vol, float pan)
{
    for (int i = 0; i < NSW; i++) if (!g_sw[i].on) {
        struct sw *w = &g_sw[i];
        w->t = 0; w->dur = dur; w->f0 = peak_hz * 0.25f; w->f1 = peak_hz; w->vol = vol; w->pan = pan;
        w->lp = w->bp = 0; w->seed = 0x1234567u + i * 977u;
        w->on = 1; return;
    }
}
static void swoosh_mix(float *mix, int frames, int rate)
{
    for (int i = 0; i < NSW; i++) {
        struct sw *w = &g_sw[i];
        if (!w->on) continue;
        float dt = 1.0f / rate, l = 0.5f - 0.4f * w->pan, r = 0.5f + 0.4f * w->pan;
        for (int n = 0; n < frames; n++) {
            float x = w->t / w->dur;
            if (x >= 1) { w->on = 0; break; }
            /* centre: rises to the peak at 35 %, falls back after */
            float f = x < 0.35f ? w->f0 + (w->f1 - w->f0) * (x / 0.35f) : w->f1 - (w->f1 - w->f0) * 0.7f * ((x - 0.35f) / 0.65f);
            float env = x < 0.12f ? x / 0.12f : (1 - x) * (1 - x) / (0.88f * 0.88f);
            w->seed = w->seed * 1664525u + 1013904223u;
            float noise = ((w->seed >> 9) / 4194304.0f) - 1.0f;
            float fc = 2.0f * sinf(3.14159f * f / rate), q = 0.35f;          /* state-variable band-pass */
            w->lp += fc * w->bp;
            float hp = noise - w->lp - q * w->bp;
            w->bp += fc * hp;
            float v = w->bp * env * w->vol;
            mix[n * 2] += v * l; mix[n * 2 + 1] += v * r;
            w->t += dt;
        }
    }
}

/* ---- sampled sounds (Red's voice, decoded from the game's bank by redvoice.c) ---- */
#define NPV 4
static struct pv { volatile int on; const s16 *pcm; int n; double pos, step; float vol, l, r; } g_pv[NPV];
void sfx_pcm(const s16 *pcm, int n, int rate, float vol, float pan)
{
    if (!g_rate || !pcm || n <= 0) return;
    int i, best = 0;
    for (i = 0; i < NPV && g_pv[i].on; i++) ;
    if (i == NPV) {                                     /* all busy: take over the one furthest along */
        for (i = 1; i < NPV; i++) if (g_pv[i].pos / g_pv[i].n > g_pv[best].pos / g_pv[best].n) best = i;
        i = best; g_pv[i].on = 0;
    }
    struct pv *v = &g_pv[i];
    v->pcm = pcm; v->n = n; v->pos = 0; v->step = (double)rate / g_rate; v->vol = vol;
    v->l = 0.5f - 0.4f * pan; v->r = 0.5f + 0.4f * pan;
    v->on = 1;
}
static void pcm_mix(float *mix, int frames)
{
    for (int i = 0; i < NPV; i++) {
        struct pv *v = &g_pv[i];
        if (!v->on) continue;
        for (int k = 0; k < frames; k++) {
            int j = (int)v->pos;
            if (j + 1 >= v->n) { v->on = 0; break; }
            float f = (float)(v->pos - j), x = (v->pcm[j] + (v->pcm[j + 1] - v->pcm[j]) * f) / 32768.0f * v->vol * 2.0f;
            mix[k * 2] += x * v->l; mix[k * 2 + 1] += x * v->r;
            v->pos += v->step;
        }
    }
}

static void callback(void *ud, Uint8 *stream, int len)
{
    (void)ud;
    int frames = len / 4;
    static float *mix; static int cap;
    if (frames * 2 > cap) { cap = frames * 2; mix = realloc(mix, cap * sizeof(float)); }
    memset(mix, 0, frames * 2 * sizeof(float));
    ds_mix(mix, frames, g_rate);
    xmv_mix(mix, frames);
    static int cal = -1; if (cal < 0) cal = getenv("XBOX_VOX_CAL") != NULL;
    float pk_game = 0, pk_ours = 0;
    if (cal) for (int i = 0; i < frames * 2; i++) if (fabsf(mix[i]) > pk_game) pk_game = fabsf(mix[i]);
    swoosh_mix(mix, frames, g_rate);
    if (cal) { static float *tmp; static int tc; if (frames * 2 > tc) { tc = frames * 2; tmp = realloc(tmp, tc * sizeof(float)); }
        memset(tmp, 0, frames * 2 * sizeof(float)); pcm_mix(tmp, frames);
        for (int i = 0; i < frames * 2; i++) { if (fabsf(tmp[i]) > pk_ours) pk_ours = fabsf(tmp[i]); mix[i] += tmp[i]; }
        static float mg, mo; static int nb; if (pk_game > mg) mg = pk_game; if (pk_ours > mo) mo = pk_ours;
        if (++nb >= 12) { klog("voxcal peak game %.3f ours %.3f\n", mg, mo); mg = mo = 0; nb = 0; }
    } else
    pcm_mix(mix, frames);
    s16 *o = (s16 *)stream;
    for (int i = 0; i < frames * 2; i++) {
        float v = mix[i] * g_master;
        if (v > 1.0f) v = 1.0f; else if (v < -1.0f) v = -1.0f;
        o[i] = (s16)(v * 32767.0f);
    }
}

int audio_out_rate(void) { return g_rate; }

void audio_sdl_start(void)
{
    if (g_dev || getenv("XBOX_NO_AUDIO")) return;
    if (getenv("XBOX_VOLUME")) g_master = (float)parse_u32(getenv("XBOX_VOLUME"), 10) / 100.0f;
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) { klog("audio: SDL audio init failed: %s (running silent)\n", SDL_GetError()); return; }
    SDL_AudioSpec want, have;
    memset(&want, 0, sizeof want);
    want.freq = 48000; want.format = AUDIO_S16SYS; want.channels = 2; want.samples = 1024; want.callback = callback;
    g_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
    if (!g_dev) { klog("audio: no audio device: %s (running silent)\n", SDL_GetError()); return; }
    g_rate = have.freq;
    g_audio_live = 1;
    SDL_PauseAudioDevice(g_dev, 0);
    klog("audio: %s, %d Hz stereo, %d-frame buffer\n", SDL_GetCurrentAudioDriver(), have.freq, have.samples);
}
