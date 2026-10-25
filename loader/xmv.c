/* XMV movie playback (intro logos, FMV cutscenes).
 *
 * The XDK's XMV player is statically linked (section XMV) and drives the APU directly, so the
 * game's 6 entry points into it are replaced here. The game's own movie code (0x2323c0 open,
 * 0x232580 per-frame update + draw) is left alone: it allocates two YUY2 textures of the movie's
 * size, asks for frames with GetNextFrame and draws them as a screen quad. We demux with ffmpeg
 * (statically linked: xmv demuxer, wmv2 video, Xbox ADPCM / PCM / WMA audio), convert video to
 * YUY2 straight into the game's surface and play the audio through the SDL mixer (xmv_mix).
 *
 * Game call sites (NA retail): CreateDecoderForFile 0x2bed4d, CloseDecoder 0x2bf291,
 * GetVideoDescriptor 0x2bf375, EnableAudioStream 0x2bf3e2, GetAudioStream 0x2bf561,
 * GetNextFrame 0x2bf580. XBOX_MOVIES=0 hides .xmv files again (old behaviour: movies skipped). */
#define _GNU_SOURCE
#include "xbox.h"
#include "gfx/gfx.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <math.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>

char *fs_resolve(const char *name, OBJECT_ATTRIBUTES *oa);
const char *fs_find(const char *name);
int audio_out_rate(void);

#define XMV_NOFRAME 0
#define XMV_NEWFRAME 1
#define XMV_ENDOFFILE 2
#define XMV_FAIL 3
#define E_FAIL 0x80004005u

#define MAXA 4
/* audio ring: stereo s16 at the device rate, written by the game thread, read by the mixer */
struct aring { s16 *buf; u32 cap, rd, wr; int mode; };   /* mode 0 front, 1 centre/LFE, 2 rear */

struct stream_obj { u32 vtbl; u32 refs; struct xmvdec *dec; };

struct xmvdec {
    AVFormatContext *fmt;
    int vidx;
    AVCodecContext *vdec;
    struct SwsContext *sws;
    int w, h, sws_w, sws_h, sws_fmt;
    int nat;                               /* audio tracks in the file */
    u16 aflags[MAXA];                      /* XMV track flags: 1/2/4 = front, centre/LFE, rear part of 5.1 ADPCM */
    int a_on[MAXA];                        /* enabled by EnableAudioStream */
    AVCodecContext *adec[MAXA];
    SwrContext *swr[MAXA];
    struct aring ring[MAXA];
    struct stream_obj sobj[MAXA];
    AVPacket *pkt;
    AVFrame *frm, *afrm;
    int have_frame;                        /* frm holds a decoded, not yet shown video frame */
    double frame_t;                        /* its time (s) */
    double last_t, frame_dur;
    int eof, vdone;
    double t0;                             /* wall clock at the first shown frame */
    int started;
    float gain;
    int shown, dropped;
    char name[128];
};

static pthread_mutex_t g_alock = PTHREAD_MUTEX_INITIALIZER;
static struct xmvdec *volatile g_cur;     /* the movie the mixer plays */

static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

/* ---- fake IDirectSoundStream handed to the game (it only AddRefs, Releases and sets volume) ---- */
static u32 STDCALL s_addref(struct stream_obj *s) { return ++s->refs; }
static u32 STDCALL s_release(struct stream_obj *s) { if (s->refs) s->refs--; return s->refs; }
static u32 STDCALL s_stub(struct stream_obj *s) { return 0; }
static u32 g_svtbl[16];

int xmv_is_stream(void *p)
{
    struct xmvdec *d = g_cur;
    if (!d || !p) return 0;
    for (int i = 0; i < MAXA; i++) if (p == (void *)&d->sobj[i]) return 1;
    return 0;
}
void xmv_stream_volume(void *p, s32 v)
{
    struct xmvdec *d = g_cur;
    if (!xmv_is_stream(p)) return;
    d->gain = v >= 0 ? 1.0f : v <= -10000 ? 0.0f : powf(10.0f, (float)v / 2000.0f);
}

/* ---- audio ---- */
static void ring_push(struct aring *r, const s16 *src, u32 frames)
{
    pthread_mutex_lock(&g_alock);
    u32 used = r->wr - r->rd;
    if (used + frames > r->cap) {                   /* grow, keeping order */
        u32 ncap = r->cap ? r->cap : 48000;
        while (used + frames > ncap) ncap *= 2;
        s16 *nb = malloc((size_t)ncap * 4);
        for (u32 i = 0; i < used; i++) memcpy(nb + 2 * i, r->buf + 2 * ((r->rd + i) % r->cap), 4);
        free(r->buf); r->buf = nb; r->cap = ncap; r->rd = 0; r->wr = used;
    }
    for (u32 i = 0; i < frames; i++) memcpy(r->buf + 2 * ((r->wr + i) % r->cap), src + 2 * i, 4);
    r->wr += frames;
    pthread_mutex_unlock(&g_alock);
}

static u32 audio_queued(struct xmvdec *d)
{
    u32 m = 0;
    pthread_mutex_lock(&g_alock);
    for (int i = 0; i < d->nat && i < MAXA; i++) if (d->a_on[i] && d->ring[i].wr - d->ring[i].rd > m) m = d->ring[i].wr - d->ring[i].rd;
    pthread_mutex_unlock(&g_alock);
    return m;
}

/* SDL audio thread: add the movie's sound to the mix */
void xmv_mix(float *out, int frames)
{
    pthread_mutex_lock(&g_alock);
    struct xmvdec *d = g_cur;
    if (d && d->started) {
        float g = d->gain;
        for (int k = 0; k < MAXA; k++) {
            struct aring *r = &d->ring[k];
            if (!d->a_on[k] || !r->cap) continue;
            u32 n = r->wr - r->rd;
            if (n > (u32)frames) n = frames;
            for (u32 i = 0; i < n; i++) {
                const s16 *s = r->buf + 2 * ((r->rd + i) % r->cap);
                float l = s[0] / 32768.0f * g, rr = s[1] / 32768.0f * g;
                if (r->mode == 1) { float c = 0.7071f * l + 0.5f * rr; out[2 * i] += c; out[2 * i + 1] += c; }
                else if (r->mode == 2) { out[2 * i] += 0.7071f * l; out[2 * i + 1] += 0.7071f * rr; }
                else { out[2 * i] += l; out[2 * i + 1] += rr; }
            }
            r->rd += n;
        }
    }
    pthread_mutex_unlock(&g_alock);
}

static int open_audio(struct xmvdec *d, int k)
{
    AVStream *st = NULL;
    int seen = 0;
    for (unsigned i = 0; i < d->fmt->nb_streams; i++)
        if (d->fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && seen++ == k) { st = d->fmt->streams[i]; break; }
    if (!st) return -1;
    const AVCodec *c = avcodec_find_decoder(st->codecpar->codec_id);
    if (!c) { klog("xmv: no decoder for audio codec %s (tag %#x)\n", avcodec_get_name(st->codecpar->codec_id), st->codecpar->codec_tag); return -1; }
    AVCodecContext *a = avcodec_alloc_context3(c);
    avcodec_parameters_to_context(a, st->codecpar);
    if (avcodec_open2(a, c, NULL) < 0) { avcodec_free_context(&a); return -1; }
    int rate = audio_out_rate();
    if (rate <= 0) rate = 48000;
    AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO, in = a->ch_layout;
    if (in.order == AV_CHANNEL_ORDER_UNSPEC) av_channel_layout_default(&in, a->ch_layout.nb_channels);
    SwrContext *s = NULL;
    if (swr_alloc_set_opts2(&s, &stereo, AV_SAMPLE_FMT_S16, rate, &in, a->sample_fmt, a->sample_rate, 0, NULL) < 0 || swr_init(s) < 0) {
        swr_free(&s); avcodec_free_context(&a); return -1;
    }
    d->adec[k] = a; d->swr[k] = s;
    klog("xmv: %s audio track %d: %s, %d Hz, %d ch\n", d->name, k, c->name, a->sample_rate, a->ch_layout.nb_channels);
    return 0;
}

static void decode_audio(struct xmvdec *d, int k, AVPacket *p)
{
    AVCodecContext *a = d->adec[k];
    if (!a || avcodec_send_packet(a, p) < 0) return;
    while (avcodec_receive_frame(a, d->afrm) == 0) {
        int outn = swr_get_out_samples(d->swr[k], d->afrm->nb_samples) + 32;
        s16 *tmp = malloc((size_t)outn * 4);
        int n = swr_convert(d->swr[k], (u8 **)&tmp, outn, (const u8 **)d->afrm->extended_data, d->afrm->nb_samples);
        if (n > 0) ring_push(&d->ring[k], tmp, n);
        free(tmp);
        av_frame_unref(d->afrm);
    }
}

/* ---- video ---- */
/* Demux until a video frame is decoded into d->frm (or the file ends). Audio found on the way is queued. */
static int next_video(struct xmvdec *d)
{
    for (;;) {
        int r = avcodec_receive_frame(d->vdec, d->frm);
        if (r == 0) {
            s64 ts = d->frm->best_effort_timestamp;
            AVRational tb = d->fmt->streams[d->vidx]->time_base;
            double t = ts == AV_NOPTS_VALUE ? d->last_t + d->frame_dur : ts * av_q2d(tb);
            if (t < d->last_t) t = d->last_t + d->frame_dur;   /* keep it monotonic */
            if (d->have_frame || d->shown) { double dt = t - d->last_t; if (dt > 0.005 && dt < 0.5) d->frame_dur = dt; }
            d->frame_t = d->last_t = t;
            d->have_frame = 1;
            return 1;
        }
        if (r == AVERROR_EOF || d->eof) { d->vdone = 1; return 0; }   /* after the flush the decoder ends with EOF */
        r = av_read_frame(d->fmt, d->pkt);
        if (r < 0) { d->eof = 1; avcodec_send_packet(d->vdec, NULL); continue; }
        AVStream *st = d->fmt->streams[d->pkt->stream_index];
        if (d->pkt->stream_index == d->vidx) avcodec_send_packet(d->vdec, d->pkt);
        else if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            int k = 0;
            for (unsigned i = 0; i < d->fmt->nb_streams && (int)i < d->pkt->stream_index; i++)
                if (d->fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) k++;
            if (k < MAXA && d->a_on[k]) decode_audio(d, k, d->pkt);
        }
        av_packet_unref(d->pkt);
    }
}

/* Write d->frm into the game's YUY2 surface. */
static void put_frame(struct xmvdec *d, XSurface *s)
{
    if (!s || !s->Data) return;
    u32 w = d->w, h = d->h, pitch = w * 2;
    if (s->Size) { w = (s->Size & 0xFFF) + 1; h = ((s->Size >> 12) & 0xFFF) + 1; pitch = (((s->Size >> 24) & 0xFF) + 1) * 64; }
    u32 fmt = XFMT_FMT(s->Format);
    enum AVPixelFormat out = fmt == 0x25 ? AV_PIX_FMT_UYVY422 : AV_PIX_FMT_YUYV422;
    if (!d->sws || d->sws_w != d->frm->width || d->sws_h != d->frm->height || d->sws_fmt != d->frm->format) {
        sws_freeContext(d->sws);
        d->sws = sws_getContext(d->frm->width, d->frm->height, d->frm->format, w, h, out, SWS_BILINEAR, NULL, NULL, NULL);
        d->sws_w = d->frm->width; d->sws_h = d->frm->height; d->sws_fmt = d->frm->format;
    }
    if (!d->sws) return;
    u8 *dst[4] = { (u8 *)guestp(s->Data) }; int ls[4] = { (int)pitch };
    sws_scale(d->sws, (const u8 *const *)d->frm->data, d->frm->linesize, 0, d->frm->height, dst, ls);
    gl_texture_dirty(s->Data & 0x7FFFFFFFu);
    if (s->Parent) gl_texture_dirty(s->Parent->Data & 0x7FFFFFFFu);
}

/* ---- the XMV API ---- */
static void close_dec(struct xmvdec *d)
{
    if (!d) return;
    pthread_mutex_lock(&g_alock);
    if (g_cur == d) g_cur = NULL;
    pthread_mutex_unlock(&g_alock);
    klog("xmv: %s closed after %d frames (%d dropped)\n", d->name, d->shown, d->dropped);
    for (int k = 0; k < MAXA; k++) { avcodec_free_context(&d->adec[k]); swr_free(&d->swr[k]); free(d->ring[k].buf); }
    avcodec_free_context(&d->vdec);
    sws_freeContext(d->sws);
    av_packet_free(&d->pkt); av_frame_free(&d->frm); av_frame_free(&d->afrm);
    avformat_close_input(&d->fmt);
    free(d);
}

static char *host_path(const char *name)
{
    char x[1024];
    if (name[0] && name[1] == ':') snprintf(x, sizeof x, "\\??\\%s", name);
    else snprintf(x, sizeof x, "%s", name);
    char *h = fs_resolve(x, NULL);
    struct stat st;
    if (h && !stat(h, &st)) return h;
    free(h);
    const char *b = strrchr(name, '\\');
    const char *f = fs_find(b ? b + 1 : name);
    return f ? strdup(f) : NULL;
}

static u32 STDCALL h_CreateDecoderForFile(u32 flags, const char *name, struct xmvdec **out)
{
    *out = NULL;
    char *path = host_path(name);
    if (!path) { klog("xmv: %s not found\n", name); return E_FAIL; }
    struct xmvdec *d = calloc(1, sizeof *d);
    const char *b = strrchr(name, '\\');
    snprintf(d->name, sizeof d->name, "%s", b ? b + 1 : name);
    d->gain = 1.0f; d->frame_dur = 1.0 / 30; d->last_t = -d->frame_dur;
    int r = avformat_open_input(&d->fmt, path, NULL, NULL);
    if (r < 0) { klog("xmv: can't open %s (%s)\n", path, av_err2str(r)); free(d); free(path); return E_FAIL; }
    d->fmt->probesize = 1 << 20;               /* streams appear with the first packet: read it */
    d->fmt->max_analyze_duration = 0;
    avformat_find_stream_info(d->fmt, NULL);
    FILE *hf = fopen(path, "rb");              /* track flags aren't exposed by the demuxer */
    if (hf) {
        u8 hd[36 + 12 * MAXA] = { 0 };
        size_t got = fread(hd, 1, sizeof hd, hf);
        fclose(hf);
        int nt = got >= 36 ? hd[32] | hd[33] << 8 : 0;
        for (int k = 0; k < nt && k < MAXA && 36 + 12 * k + 12 <= (int)got; k++) d->aflags[k] = hd[36 + 12 * k + 10] | hd[36 + 12 * k + 11] << 8;
    }
    d->vidx = -1;
    for (unsigned i = 0; i < d->fmt->nb_streams; i++) {
        AVCodecParameters *cp = d->fmt->streams[i]->codecpar;
        if (cp->codec_type == AVMEDIA_TYPE_VIDEO && d->vidx < 0) d->vidx = i;
        else if (cp->codec_type == AVMEDIA_TYPE_AUDIO) d->nat++;
    }
    const AVCodec *vc = d->vidx >= 0 ? avcodec_find_decoder(d->fmt->streams[d->vidx]->codecpar->codec_id) : NULL;
    if (vc) {
        d->vdec = avcodec_alloc_context3(vc);
        avcodec_parameters_to_context(d->vdec, d->fmt->streams[d->vidx]->codecpar);
        if (avcodec_open2(d->vdec, vc, NULL) < 0) avcodec_free_context(&d->vdec);
    }
    if (!d->vdec) { klog("xmv: %s has no playable video\n", path); avformat_close_input(&d->fmt); free(d); free(path); return E_FAIL; }
    d->w = d->vdec->width; d->h = d->vdec->height;
    d->pkt = av_packet_alloc(); d->frm = av_frame_alloc(); d->afrm = av_frame_alloc();
    for (int k = 0; k < MAXA; k++) {
        d->sobj[k].vtbl = (u32)(uintptr_t)g_svtbl; d->sobj[k].refs = 1; d->sobj[k].dec = d;
    }
    klog("xmv: playing %s (%dx%d, %d audio track%s)\n", path, d->w, d->h, d->nat, d->nat == 1 ? "" : "s");
    free(path);
    pthread_mutex_lock(&g_alock); g_cur = d; pthread_mutex_unlock(&g_alock);   /* the mixer waits for d->started */
    *out = d;
    return 0;
}

static void STDCALL h_CloseDecoder(struct xmvdec *d) { close_dec(d); }

static void STDCALL h_GetVideoDescriptor(struct xmvdec *d, u32 *desc)
{
    desc[0] = d->w; desc[1] = d->h;
    AVRational fr = d->fmt->streams[d->vidx]->avg_frame_rate;
    desc[2] = fr.num && fr.den ? (u32)(av_q2d(fr) + 0.5) : 30;
    desc[3] = d->nat;
}

static u32 STDCALL h_EnableAudioStream(struct xmvdec *d, u32 stream, u32 flags, void *mixbins, u32 *ppstream)
{
    if (stream >= (u32)d->nat || stream >= MAXA) return E_FAIL;
    /* 5.1 ADPCM is three stereo tracks (front, centre/LFE, rear) starting at `stream`; like the
     * XDK, all three are enabled when the caller gives no mix bins. We downmix them to stereo. */
    int n = 1;
    if (!flags && !mixbins && !ppstream && (d->aflags[stream] & 7) == 1 && stream + 3 <= (u32)d->nat && stream + 3 <= MAXA) n = 3;
    for (int i = 0; i < n; i++) {
        int k = stream + i;
        if (!d->adec[k] && open_audio(d, k) < 0) continue;
        d->a_on[k] = 1;
        d->ring[k].mode = (d->aflags[k] & 2) ? 1 : (d->aflags[k] & 4) ? 2 : 0;
    }
    if (ppstream) { d->sobj[stream].refs++; *ppstream = (u32)(uintptr_t)&d->sobj[stream]; }
    return 0;
}

static u32 STDCALL h_GetAudioStream(struct xmvdec *d, u32 stream, u32 *pp)
{
    struct stream_obj *s = &d->sobj[stream < MAXA ? stream : 0];
    s->refs++;
    *pp = (u32)(uintptr_t)s;
    return 0;
}

static u32 STDCALL h_GetNextFrame(struct xmvdec *d, XSurface *surf, u32 *result, s64 *prt)
{
    static int skip_all = -1;
    if (skip_all < 0) skip_all = getenv("XBOX_XMV_SKIP") != NULL;
    if (skip_all) { *result = XMV_ENDOFFILE; return 0; }
    if (!d->have_frame && !d->vdone) next_video(d);
    if (!d->have_frame) {
        /* video done: let the sound finish (bounded) */
        double now = now_s();
        if (d->started && audio_queued(d) > 0 && now - d->t0 < d->last_t + 3.0) { *result = XMV_NOFRAME; return 0; }
        *result = XMV_ENDOFFILE;
        return 0;
    }
    if (!d->started) {                      /* first frame: start the clock and the sound now */
        d->t0 = now_s() - d->frame_t;
        pthread_mutex_lock(&g_alock); d->started = 1; pthread_mutex_unlock(&g_alock);
    }
    double clock = now_s() - d->t0;
    if (d->frame_t > clock) { *result = XMV_NOFRAME; return 0; }
    /* late (loading stall, slow machine): skip frames whose successor is already due too */
    while (clock - d->frame_t >= d->frame_dur && !d->vdone) {
        d->have_frame = 0;
        if (!next_video(d)) break;
        d->dropped++;
    }
    if (!d->have_frame) { *result = XMV_ENDOFFILE; return 0; }
    put_frame(d, surf);
    if (prt) *prt = (s64)(d->frame_t * 1e7);
    d->have_frame = 0;
    d->shown++;
    *result = XMV_NEWFRAME;
    return 0;
}

static int g_on = -1;
int xmv_enabled(void)
{
    if (g_on < 0) { const char *e = getenv("XBOX_MOVIES"); g_on = !(e && e[0] == '0'); }
    return g_on;
}

void xmv_install(void)
{
    static const struct { u32 at; u8 sig[6]; void *f; } hooks[] = {
        { 0x2bed4d, { 0x55, 0x8b, 0xec, 0x83, 0xec, 0x20 }, h_CreateDecoderForFile },
        { 0x2bf291, { 0x55, 0x8b, 0xec, 0x51, 0x53, 0x56 }, h_CloseDecoder },
        { 0x2bf375, { 0x8b, 0x44, 0x24, 0x04, 0x8b, 0x50 }, h_GetVideoDescriptor },
        { 0x2bf3e2, { 0x55, 0x8b, 0xec, 0x83, 0xec, 0x2c }, h_EnableAudioStream },
        { 0x2bf561, { 0x8b, 0x44, 0x24, 0x04, 0x8b, 0x80 }, h_GetAudioStream },
        { 0x2bf580, { 0x55, 0x8b, 0xec, 0x83, 0xec, 0x3c }, h_GetNextFrame },
    };
    if (!xmv_enabled()) { klog("xmv: movies off (XBOX_MOVIES=0)\n"); return; }
    for (size_t i = 0; i < sizeof hooks / sizeof hooks[0]; i++)
        if (memcmp((void *)(uintptr_t)hooks[i].at, hooks[i].sig, 6)) {
            klog("xmv: XMV library not where expected (%#x) - movies disabled\n", hooks[i].at);
            g_on = 0;
            return;
        }
    g_svtbl[0] = (u32)(uintptr_t)s_addref; g_svtbl[1] = (u32)(uintptr_t)s_release;
    for (int i = 2; i < 16; i++) g_svtbl[i] = (u32)(uintptr_t)s_stub;
    av_log_set_level(getenv("XBOX_XMV_LOG") ? AV_LOG_VERBOSE : AV_LOG_ERROR);
    for (size_t i = 0; i < sizeof hooks / sizeof hooks[0]; i++) hle_patch_jmp(hooks[i].at, hooks[i].f);
    /* the game's movie code flushes the CPU cache (wbinvd, privileged) after each new frame: nop it */
    static const u32 wb[] = { 0x23256c, 0x2325c2 };
    for (size_t i = 0; i < 2; i++) { u8 *p = (u8 *)(uintptr_t)wb[i]; if (p[0] == 0x0f && p[1] == 0x09) p[0] = p[1] = 0x90; }
    klog("xmv: movie playback on (ffmpeg %s)\n", av_version_info());
}
