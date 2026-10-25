/* DirectSound (Xbox) high-level emulation.
 * Buffers keep real-time play cursors so the game's streaming logic works;
 * mixing to SDL audio is layered on top (audio_sdl.c). */
#define _GNU_SOURCE
#include "xbox.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>

u32 hle_sym(const char *name);
int hle_hook(const char *name, void *target);
void audio_buffer_changed(void *b);

struct dsbuf {
    u32 magic;
    u32 refs;
    u16 fmt_tag, channels; u32 rate, avg_bps; u16 block_align, bits;
    u32 freq_override;
    u8 *data; u32 bytes;
    u32 loop_start, loop_len;
    int playing, looping;
    double start_time;      /* when the cursor was at start_pos */
    u32 start_pos;
    s32 volume;             /* hundredths of dB, 0 = full */
    u32 flags;
    /* device-driven playback (audio_sdl.c): byte position + 16.16 fraction of a frame */
    u32 pos, frac;
    float bin_gain[8];      /* linear gain per mix bin (FL FR C LFE BL BR) */
    int has_bins;
};
int g_audio_live;           /* set by the SDL backend: cursors follow the audio device */
#define DSB_MAGIC 0x44534231u
static pthread_mutex_t g_dslock = PTHREAD_MUTEX_INITIALIZER;
struct dsbuf *g_bufs[512];
int g_nbufs;

static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

static u32 bytes_per_sec(struct dsbuf *b)
{
    u32 rate = b->freq_override ? b->freq_override : b->rate;
    if (b->fmt_tag == 0x69) return rate * 36 * b->channels / 64;   /* Xbox ADPCM: 64 samples per 36-byte block */
    return rate * b->block_align;
}

/* current play cursor; stops one-shot buffers at the end. lock held */
static u32 cursor(struct dsbuf *b)
{
    if (g_audio_live) return b->pos;
    if (!b->playing || !b->bytes) return b->start_pos;
    double el = now_s() - b->start_time;
    u64 pos = b->start_pos + (u64)(el * bytes_per_sec(b));
    if (b->looping) {
        u32 ls = b->loop_start, ll = b->loop_len ? b->loop_len : b->bytes - b->loop_start;
        if (ll == 0) ll = b->bytes;
        if (pos >= ls + ll) pos = ls + (pos - ls) % ll;
    } else if (pos >= b->bytes) {
        b->playing = 0;
        b->start_pos = 0;
        return 0;
    }
    return (u32)pos;
}

static u32 g_fake_ds[64];

static u32 STDCALL h_DirectSoundCreate(void *guid, u32 *ppDS, void *unk)
{
    klog("DirectSoundCreate -> HLE\n");
    { extern void audio_sdl_start(void); audio_sdl_start(); }
    *ppDS = (u32)(uintptr_t)g_fake_ds;
    return 0;
}

/* DSBUFFERDESC { dwSize, dwFlags, dwBufferBytes, lpwfxFormat, lpMixBins, dwInputMixBin } */
static u32 STDCALL h_IDirectSound_CreateSoundBuffer(void *ds, u32 *desc, u32 *ppBuf, void *unk)
{
    struct dsbuf *b = calloc(1, sizeof *b);
    b->magic = DSB_MAGIC; b->refs = 1; b->flags = desc[1];
    const u8 *wfx = (const u8 *)(uintptr_t)desc[3];
    if (wfx) {
        b->fmt_tag = *(u16 *)wfx; b->channels = *(u16 *)(wfx + 2); b->rate = *(u32 *)(wfx + 4);
        b->avg_bps = *(u32 *)(wfx + 8); b->block_align = *(u16 *)(wfx + 12); b->bits = *(u16 *)(wfx + 14);
    } else { b->fmt_tag = 1; b->channels = 2; b->rate = 48000; b->block_align = 4; b->bits = 16; }
    if (!b->block_align) b->block_align = b->channels * b->bits / 8;
    if (desc[2]) { b->bytes = desc[2]; b->data = calloc(1, desc[2]); }
    pthread_mutex_lock(&g_dslock);
    if (g_nbufs < 512) g_bufs[g_nbufs++] = b;
    pthread_mutex_unlock(&g_dslock);
    *ppBuf = (u32)(uintptr_t)b;
    static int n;
    if (n++ < 8) klog("CreateSoundBuffer(fmt %#x ch %u %u Hz %u bits, %u bytes, flags %#x) -> %p\n",
                      b->fmt_tag, b->channels, b->rate, b->bits, b->bytes, desc[1], (void *)b);
    return 0;
}

#define BUF(p) struct dsbuf *b = (struct dsbuf *)(p); if (!b || b->magic != DSB_MAGIC) return 0x80004005
static u32 STDCALL h_Buf_SetBufferData(void *p, void *data, u32 bytes)
{
    BUF(p);
    pthread_mutex_lock(&g_dslock);
    b->data = data; b->bytes = bytes; b->start_pos = 0; b->loop_start = 0; b->loop_len = 0; b->pos = 0; b->frac = 0;
    if (b->playing) b->start_time = now_s();
    pthread_mutex_unlock(&g_dslock);
    audio_buffer_changed(b);
    return 0;
}
static u32 STDCALL h_Buf_Play(void *p, u32 r1, u32 r2, u32 flags)
{
    BUF(p);
    pthread_mutex_lock(&g_dslock);
    if (!b->playing) { b->start_time = now_s(); b->playing = 1; }
    b->looping = flags & 1;
    pthread_mutex_unlock(&g_dslock);
    return 0;
}
static u32 STDCALL h_Buf_Stop(void *p)
{
    BUF(p);
    pthread_mutex_lock(&g_dslock);
    b->start_pos = cursor(b); b->playing = 0;
    pthread_mutex_unlock(&g_dslock);
    return 0;
}
static float db100_to_gain(s32 v) { if (v >= 0) return 1.0f; if (v <= -10000) return 0.0f; return powf(10.0f, (float)v / 2000.0f); }
static u32 STDCALL h_Buf_GetCurrentPosition(void *p, u32 *play, u32 *write)
{
    BUF(p);
    pthread_mutex_lock(&g_dslock);
    u32 c = cursor(b);
    pthread_mutex_unlock(&g_dslock);
    if (play) *play = c;
    if (write) *write = b->bytes ? (c + b->block_align * 64) % b->bytes : 0;
    return 0;
}
static u32 STDCALL h_Buf_SetCurrentPosition(void *p, u32 pos)
{
    BUF(p);
    pthread_mutex_lock(&g_dslock);
    b->start_pos = pos; b->start_time = now_s(); b->pos = b->bytes ? pos % b->bytes : 0; b->frac = 0;
    pthread_mutex_unlock(&g_dslock);
    return 0;
}
static u32 STDCALL h_Buf_SetLoopRegion(void *p, u32 start, u32 len) { BUF(p); b->loop_start = start; b->loop_len = len; return 0; }
static u32 STDCALL h_Buf_SetFrequency(void *p, u32 f)
{
    BUF(p);
    pthread_mutex_lock(&g_dslock);
    b->start_pos = cursor(b); b->start_time = now_s(); b->freq_override = f;
    pthread_mutex_unlock(&g_dslock);
    return 0;
}
static u32 STDCALL h_Buf_SetVolume(void *p, s32 v) { BUF(p); b->volume = v; return 0; }
static u32 STDCALL h_Buf_SetHeadroom(void *p, u32 h) { return 0; }
/* DSMIXBINS { dwMixBinCount, DSMIXBINVOLUMEPAIR *lpMixBinVolumePairs { dwMixBin, lVolume } } */
static u32 STDCALL h_Buf_SetMixBins(void *p, void *m)
{
    BUF(p);
    pthread_mutex_lock(&g_dslock);
    memset(b->bin_gain, 0, sizeof b->bin_gain);
    b->has_bins = 0;
    if (m) {
        const u32 *mb = m;
        const u32 *pairs = (const u32 *)(uintptr_t)mb[1];
        for (u32 i = 0; i < mb[0] && i < 8 && pairs; i++) {
            u32 bin = pairs[2 * i];
            if (bin < 8) { b->bin_gain[bin] = db100_to_gain((s32)pairs[2 * i + 1]); b->has_bins = 1; }
        }
    }
    pthread_mutex_unlock(&g_dslock);
    static int n;
    if (n++ < 8) klog("dsound: buffer %p mix bins FL %.2f FR %.2f C %.2f LFE %.2f BL %.2f BR %.2f\n", (void *)b,
                      b->bin_gain[0], b->bin_gain[1], b->bin_gain[2], b->bin_gain[3], b->bin_gain[4], b->bin_gain[5]);
    return 0;
}
static u32 STDCALL h_Buf_Release(void *p)
{
    BUF(p);
    if (b->refs) b->refs--;
    if (!b->refs) { pthread_mutex_lock(&g_dslock); b->playing = 0; pthread_mutex_unlock(&g_dslock); }
    return b->refs;
}
static u32 STDCALL h_DS_SetMixBinHeadroom(void *ds, u32 mask, u32 h) { return 0; }
static u32 STDCALL h_DS_SynchPlayback(void *ds) { return 0; }
static u32 STDCALL h_DS_SetDopplerFactor(void *ds, u32 f, u32 apply) { return 0; }
static u32 STDCALL h_DirectSoundDoWork(void) { return 0; }
void xmv_stream_volume(void *s, s32 v);
static u32 STDCALL h_Stream_SetVolume(void *s, s32 v) { xmv_stream_volume(s, v); return 0; }
static u32 STDCALL h_Stream_SetHeadroom(void *s, u32 h) { return 0; }
static u32 STDCALL h_Stream_SetMixBins(void *s, void *m) { return 0; }

/* audio thread access */
void ds_lock(void) { pthread_mutex_lock(&g_dslock); }
void ds_unlock(void) { pthread_mutex_unlock(&g_dslock); }
u32 ds_cursor_locked(void *b) { return cursor(b); }

void dsound_install(void)
{
    static const struct { const char *n; void *f; } hooks[] = {
        { "DirectSoundCreate", h_DirectSoundCreate },
        { "IDirectSound_CreateSoundBuffer", h_IDirectSound_CreateSoundBuffer },
        { "IDirectSoundBuffer_SetBufferData", h_Buf_SetBufferData },
        { "IDirectSoundBuffer_Play", h_Buf_Play },
        { "IDirectSoundBuffer_Stop", h_Buf_Stop },
        { "IDirectSoundBuffer_GetCurrentPosition", h_Buf_GetCurrentPosition },
        { "IDirectSoundBuffer_SetCurrentPosition", h_Buf_SetCurrentPosition },
        { "IDirectSoundBuffer_SetLoopRegion", h_Buf_SetLoopRegion },
        { "IDirectSoundBuffer_SetFrequency", h_Buf_SetFrequency },
        { "IDirectSoundBuffer_SetVolume", h_Buf_SetVolume },
        { "IDirectSoundBuffer_SetHeadroom", h_Buf_SetHeadroom },
        { "IDirectSoundBuffer_SetMixBins", h_Buf_SetMixBins },
        { "IDirectSoundBuffer_Release", h_Buf_Release },
        { "IDirectSound_SetMixBinHeadroom", h_DS_SetMixBinHeadroom },
        { "IDirectSound_SynchPlayback", h_DS_SynchPlayback },
        { "IDirectSound_SetDopplerFactor", h_DS_SetDopplerFactor },
        { "DirectSoundDoWork", h_DirectSoundDoWork },
        { "IDirectSoundStream_SetVolume", h_Stream_SetVolume },
        { "IDirectSoundStream_SetHeadroom", h_Stream_SetHeadroom },
        { "IDirectSoundStream_SetMixBins", h_Stream_SetMixBins },
    };
    int ok = 0;
    for (size_t i = 0; i < sizeof hooks / sizeof hooks[0]; i++) ok += hle_hook(hooks[i].n, hooks[i].f);
    klog("dsound: %d/%zu audio functions replaced\n", ok, sizeof hooks / sizeof hooks[0]);
}

void audio_buffer_changed(void *b) { (void)b; }

/* ---- mixer, called from the audio device thread: adds `frames` stereo frames at `out_rate` ---- */
void ds_mix(float *out, int frames, int out_rate)
{
    pthread_mutex_lock(&g_dslock);
    for (int i = 0; i < g_nbufs; i++) {
        struct dsbuf *b = g_bufs[i];
        if (!b || !b->playing || !b->data || !b->bytes || !b->refs) continue;
        if (b->fmt_tag != 1 || (b->bits != 16 && b->bits != 8) || b->channels < 1 || b->channels > 2) continue;
        float g = db100_to_gain(b->volume);
        float gl, gr;
        if (b->has_bins) {
            const float *k = b->bin_gain;
            gl = k[0] + 0.7071f * k[2] + 0.7071f * k[4] + 0.5f * k[3];
            gr = k[1] + 0.7071f * k[2] + 0.7071f * k[5] + 0.5f * k[3];
        } else gl = gr = 1.0f;
        gl *= g; gr *= g;
        u32 rate = b->freq_override ? b->freq_override : b->rate;
        u32 step = (u32)(((u64)rate << 16) / (u32)out_rate);     /* source frames per output frame, 16.16 */
        u32 ba = b->block_align ? b->block_align : b->channels * b->bits / 8;
        u32 nframes = b->bytes / ba;
        u32 lstart = b->loop_start / ba, llen = b->loop_len ? b->loop_len / ba : nframes - lstart;
        if (!llen || lstart + llen > nframes) { lstart = 0; llen = nframes; }
        u32 f = b->pos / ba;
        for (int n = 0; n < frames; n++) {
            float l, r;
            const u8 *src = b->data + (size_t)f * ba;
            if (b->bits == 16) { const s16 *sp = (const s16 *)src; l = sp[0] / 32768.0f; r = b->channels == 2 ? sp[1] / 32768.0f : l; }
            else { l = (src[0] - 128) / 128.0f; r = b->channels == 2 ? (src[1] - 128) / 128.0f : l; }
            if (b->channels == 2) { out[2 * n] += l * g; out[2 * n + 1] += r * g; }
            else { out[2 * n] += l * gl; out[2 * n + 1] += r * gr; }
            b->frac += step;
            f += b->frac >> 16; b->frac &= 0xFFFF;
            if (b->looping) { while (f >= lstart + llen) f -= llen; }
            else if (f >= nframes) { b->playing = 0; f = 0; break; }
        }
        b->pos = f * ba;
    }
    pthread_mutex_unlock(&g_dslock);
}
