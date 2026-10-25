/* Rtl* runtime helpers, critical sections, exceptions, Xc* crypto. */
#define _GNU_SOURCE
#include "xbox.h"
#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---- critical sections ----
 * Xbox RTL_CRITICAL_SECTION: KEVENT (0x10) | LockCount (0x10) | RecursionCount (0x14) | OwningThread (0x18)
 * The fields are kept accurate because XAPI code may inspect them inline. */
typedef struct { DISPATCHER_HEADER Event; s32 LockCount; s32 RecursionCount; void *OwningThread; } RTL_CS;

u32 STDCALL k_RtlInitializeCriticalSection(RTL_CS *cs)
{
    memset(cs, 0, sizeof *cs);
    cs->Event.Type = EventSynchronizationObject;
    cs->Event.Size = 4;
    cs->LockCount = -1;
    return 0;
}

u32 STDCALL k_RtlEnterCriticalSection(RTL_CS *cs)
{
    void *me = xthread_current_ethread();
    ksync_lock();
    if (cs->OwningThread == me) { cs->RecursionCount++; cs->LockCount++; ksync_unlock(); return 0; }
    cs->LockCount++;
    while (cs->OwningThread) {
        extern void ksync_wait_locked(void);
        ksync_wait_locked();
    }
    cs->OwningThread = me;
    cs->RecursionCount = 1;
    ksync_unlock();
    return 0;
}

u32 STDCALL k_RtlLeaveCriticalSection(RTL_CS *cs)
{
    ksync_lock();
    cs->LockCount--;
    if (--cs->RecursionCount <= 0) {
        cs->RecursionCount = 0;
        cs->OwningThread = NULL;
        ksync_broadcast();
    }
    ksync_unlock();
    return 0;
}

/* ---- strings ---- */
u32 STDCALL k_RtlInitAnsiString(ANSI_STRING *Dest, const char *Src)
{
    Dest->Buffer = (char *)Src;
    if (Src) { size_t n = strlen(Src); Dest->Length = (u16)n; Dest->MaximumLength = (u16)(n + 1); }
    else Dest->Length = Dest->MaximumLength = 0;
    return 0;
}

u32 STDCALL k_RtlEqualString(ANSI_STRING *a, ANSI_STRING *b, u32 CaseInsensitive)
{
    if (a->Length != b->Length) return 0;
    for (u32 i = 0; i < a->Length; i++) {
        char x = a->Buffer[i], y = b->Buffer[i];
        if (CaseInsensitive) { x = toupper((u8)x); y = toupper((u8)y); }
        if (x != y) return 0;
    }
    return 1;
}

u32 STDCALL k_RtlUnicodeStringToAnsiString(ANSI_STRING *Dest, UNICODE_STRING *Src, u32 Allocate)
{
    u32 n = Src->Length / 2;
    if (Allocate) {
        Dest->Buffer = malloc(n + 1);
        Dest->MaximumLength = (u16)(n + 1);
    } else if (n + 1 > Dest->MaximumLength) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    for (u32 i = 0; i < n; i++) Dest->Buffer[i] = Src->Buffer[i] < 0x100 ? (char)Src->Buffer[i] : '?';
    Dest->Buffer[n] = 0;
    Dest->Length = (u16)n;
    return STATUS_SUCCESS;
}

u32 STDCALL k_RtlCompareMemoryUlong(u32 *Source, u32 Length, u32 Pattern)
{
    u32 i = 0;
    while (i + 4 <= Length && Source[i / 4] == Pattern) i += 4;
    return i;
}

u32 STDCALL k_RtlNtStatusToDosError(u32 Status)
{
    switch (Status) {
    case STATUS_SUCCESS: return 0;
    case STATUS_OBJECT_NAME_NOT_FOUND: case STATUS_NO_SUCH_FILE: return 2;   /* ERROR_FILE_NOT_FOUND */
    case STATUS_OBJECT_PATH_NOT_FOUND: return 3;                             /* ERROR_PATH_NOT_FOUND */
    case STATUS_ACCESS_DENIED: return 5;
    case STATUS_INVALID_HANDLE: return 6;
    case STATUS_NO_MEMORY: return 8;
    case STATUS_NO_MORE_FILES: return 18;
    case STATUS_END_OF_FILE: return 38;                                      /* ERROR_HANDLE_EOF */
    case STATUS_OBJECT_NAME_COLLISION: return 183;                           /* ERROR_ALREADY_EXISTS */
    case STATUS_PENDING: return 997;
    case STATUS_TIMEOUT: return 258;
    case STATUS_BUFFER_TOO_SMALL: return 122;
    case STATUS_NOT_A_DIRECTORY: return 267;
    default: return 317;                                                     /* ERROR_MR_MID_NOT_FOUND */
    }
}

/* TIME_FIELDS { Year, Month, Day, Hour, Minute, Second, Milliseconds, Weekday } (shorts) */
u32 STDCALL k_RtlTimeToTimeFields(s64 *Time, s16 *tf)
{
    s64 t = *Time;
    time_t secs = (time_t)(t / 10000000LL - 11644473600LL);
    struct tm tm;
    gmtime_r(&secs, &tm);
    tf[0] = tm.tm_year + 1900; tf[1] = tm.tm_mon + 1; tf[2] = tm.tm_mday;
    tf[3] = tm.tm_hour; tf[4] = tm.tm_min; tf[5] = tm.tm_sec;
    tf[6] = (s16)((t / 10000) % 1000); tf[7] = tm.tm_wday;
    return 0;
}

u32 STDCALL k_RtlTimeFieldsToTime(s16 *tf, s64 *Time)
{
    struct tm tm = { 0 };
    tm.tm_year = tf[0] - 1900; tm.tm_mon = tf[1] - 1; tm.tm_mday = tf[2];
    tm.tm_hour = tf[3]; tm.tm_min = tf[4]; tm.tm_sec = tf[5];
    time_t s = timegm(&tm);
    *Time = ((s64)s + 11644473600LL) * 10000000LL + (s64)tf[6] * 10000;
    return 1;
}

/* ---- structured exceptions ----
 * Minimal x86 SEH dispatcher: walk the fs:[0] registration chain and call each
 * handler.  Enough for MSVC C++ throw / __try in practice. */
typedef struct EXCEPTION_RECORD {
    u32 ExceptionCode, ExceptionFlags;
    struct EXCEPTION_RECORD *ExceptionRecord;
    void *ExceptionAddress;
    u32 NumberParameters;
    u32 ExceptionInformation[15];
} EXCEPTION_RECORD;
typedef struct REG { struct REG *Next; void *Handler; } REG;
typedef u32 CDECLX (*seh_handler_t)(EXCEPTION_RECORD *, REG *, void *ctx, void *disp);

#define EXCEPTION_NONCONTINUABLE 1
#define EXCEPTION_UNWINDING      2
#define EXCEPTION_EXIT_UNWIND    4

/* CONTEXT is not modelled; handlers that inspect it get a zeroed buffer. */
static u8 g_dummy_ctx[0x2CC];

u32 STDCALL k_RtlRaiseException(EXCEPTION_RECORD *rec)
{
    klog("RtlRaiseException(code %08x addr %p params %u)\n", rec->ExceptionCode, rec->ExceptionAddress, rec->NumberParameters);
    REG *r;
    __asm__ volatile("movl %%fs:0, %0" : "=r"(r));
    while (r && r != (REG *)0xFFFFFFFF) {
        u32 disp = 0;
        u32 res = ((seh_handler_t)r->Handler)(rec, r, g_dummy_ctx, &disp);
        if (res == 0) {          /* ExceptionContinueExecution */
            if (rec->ExceptionFlags & EXCEPTION_NONCONTINUABLE) break;
            return 0;
        }
        r = r->Next;
    }
    klog("unhandled exception %08x - aborting\n", rec->ExceptionCode);
    abort();
}

/* Call handlers of frames above TargetFrame with EXCEPTION_UNWINDING, unlink them,
 * then (MSVC semantics) return so the caller continues at its own landing pad. */
u32 STDCALL k_RtlUnwind(REG *TargetFrame, void *TargetIp, EXCEPTION_RECORD *rec, u32 ReturnValue)
{
    (void)TargetIp; (void)ReturnValue;
    EXCEPTION_RECORD local = { 0 };
    if (!rec) { local.ExceptionCode = 0xC0000027; rec = &local; }   /* STATUS_UNWIND */
    rec->ExceptionFlags |= TargetFrame ? EXCEPTION_UNWINDING : (EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND);
    REG *r;
    __asm__ volatile("movl %%fs:0, %0" : "=r"(r));
    while (r && r != (REG *)0xFFFFFFFF && r != TargetFrame) {
        u32 disp = 0;
        ((seh_handler_t)r->Handler)(rec, r, g_dummy_ctx, &disp);
        r = r->Next;
        __asm__ volatile("movl %0, %%fs:0" : : "r"(r));
    }
    return 0;
}

/* ---- SHA-1 / HMAC (save signatures) ---- */
typedef struct { u32 h[5]; u64 len; u8 buf[64]; u32 n; } SHA1;

static u32 rol(u32 x, int n) { return (x << n) | (x >> (32 - n)); }
static void sha1_block(SHA1 *s, const u8 *p)
{
    u32 w[80];
    for (int i = 0; i < 16; i++) w[i] = (u32)p[4*i] << 24 | (u32)p[4*i+1] << 16 | (u32)p[4*i+2] << 8 | p[4*i+3];
    for (int i = 16; i < 80; i++) w[i] = rol(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
    u32 a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4];
    for (int i = 0; i < 80; i++) {
        u32 f, k;
        if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
        else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
        else { f = b ^ c ^ d; k = 0xCA62C1D6; }
        u32 t = rol(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rol(b, 30); b = a; a = t;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e;
}
static void sha1_init(SHA1 *s)
{
    s->h[0] = 0x67452301; s->h[1] = 0xEFCDAB89; s->h[2] = 0x98BADCFE; s->h[3] = 0x10325476; s->h[4] = 0xC3D2E1F0;
    s->len = 0; s->n = 0;
}
static void sha1_update(SHA1 *s, const u8 *p, u32 n)
{
    s->len += n;
    while (n--) { s->buf[s->n++] = *p++; if (s->n == 64) { sha1_block(s, s->buf); s->n = 0; } }
}
static void sha1_final(SHA1 *s, u8 *out)
{
    u64 bits = s->len * 8;
    u8 pad = 0x80;
    sha1_update(s, &pad, 1);
    pad = 0;
    while (s->n != 56) sha1_update(s, &pad, 1);
    u8 l[8];
    for (int i = 0; i < 8; i++) l[i] = (u8)(bits >> (56 - 8 * i));
    sha1_update(s, l, 8);
    for (int i = 0; i < 5; i++) { out[4*i] = s->h[i] >> 24; out[4*i+1] = s->h[i] >> 16; out[4*i+2] = s->h[i] >> 8; out[4*i+3] = s->h[i]; }
}

_Static_assert(sizeof(SHA1) <= 116, "SHA ctx must fit the Xbox A_SHA_CTX");
u32 STDCALL k_XcSHAInit(SHA1 *ctx) { sha1_init(ctx); return 0; }
u32 STDCALL k_XcSHAUpdate(SHA1 *ctx, const u8 *in, u32 n) { sha1_update(ctx, in, n); return 0; }
u32 STDCALL k_XcSHAFinal(SHA1 *ctx, u8 *digest) { sha1_final(ctx, digest); return 0; }

u32 STDCALL k_XcHMAC(const u8 *key, u32 keylen, const u8 *d1, u32 n1, const u8 *d2, u32 n2, u8 *out)
{
    u8 k[64] = { 0 }, ipad[64], opad[64], inner[20];
    if (keylen > 64) keylen = 64;
    memcpy(k, key, keylen);
    for (int i = 0; i < 64; i++) { ipad[i] = k[i] ^ 0x36; opad[i] = k[i] ^ 0x5C; }
    SHA1 s;
    sha1_init(&s); sha1_update(&s, ipad, 64);
    if (d1) sha1_update(&s, d1, n1);
    if (d2) sha1_update(&s, d2, n2);
    sha1_final(&s, inner);
    sha1_init(&s); sha1_update(&s, opad, 64); sha1_update(&s, inner, 20); sha1_final(&s, out);
    return 0;
}
