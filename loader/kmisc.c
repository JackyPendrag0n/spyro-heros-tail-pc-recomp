/* Hal/Av/Ex settings/Dbg/Io devices/Xe sections and the logging helpers. */
#define _GNU_SOURCE
#include "xbox.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

int g_trace;
static pthread_mutex_t g_loglock = PTHREAD_MUTEX_INITIALIZER;
static struct timespec g_t0;

void klog(const char *fmt, ...)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    if (!g_t0.tv_sec) g_t0 = t;
    double s = (t.tv_sec - g_t0.tv_sec) + (t.tv_nsec - g_t0.tv_nsec) / 1e9;
    va_list ap;
    va_start(ap, fmt);
    pthread_mutex_lock(&g_loglock);
    fprintf(stderr, "[%8.3f %p] ", s, xthread_current_ethread());
    vfprintf(stderr, fmt, ap);
    pthread_mutex_unlock(&g_loglock);
    va_end(ap);
}

void kstub_log(int ordinal, const char *name, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    klog("STUB kernel #%d %s(%s)\n", ordinal, name, buf);
}

/* ---- debug output ---- */
u32 CDECLX k_DbgPrint(const char *fmt, ...)
{
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    size_t n = strlen(buf);
    while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = 0;
    klog("DbgPrint: %s\n", buf);
    return 0;
}

u32 STDCALL k_KeBugCheck(u32 code)
{
    klog("KeBugCheck(%08x)\n", code);
    abort();
}

/* ---- power / firmware ---- */
u32 STDCALL k_HalReturnToFirmware(u32 Routine)
{
    klog("HalReturnToFirmware(%u): game asked to reboot/quit\n", Routine);
    fflush(NULL);
    _exit(0);
}
u32 STDCALL k_HalInitiateShutdown(void) { return k_HalReturnToFirmware(0); }
u32 STDCALL k_HalIsResetOrShutdownPending(void) { return 0; }
u32 STDCALL k_HalRegisterShutdownNotification(void *r, u32 reg) { (void)r; (void)reg; return 0; }

u32 STDCALL k_HalReadWritePCISpace(u32 bus, u32 slot, u32 reg, void *buf, u32 len, u32 write)
{
    TRACE("HalReadWritePCISpace(%u,%u,%x,len %u,%s)\n", bus, slot, reg, len, write ? "W" : "R");
    if (!write) memset(buf, 0, len);
    return 0;
}

/* ---- AV ---- */
u32 STDCALL k_AvGetSavedDataAddress(void) { return 0; }
u32 STDCALL k_AvSetSavedDataAddress(u32 a) { (void)a; return 0; }
u32 STDCALL k_AvSetDisplayMode(u32 a, u32 b, u32 c, u32 d, u32 e, u32 f)
{ (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; TRACE("AvSetDisplayMode\n"); return 0; }
u32 STDCALL k_AvSendTVEncoderOption(u32 RegisterBase, u32 Option, u32 Param, u32 *Result)
{
    (void)RegisterBase; (void)Param;
    TRACE("AvSendTVEncoderOption(opt %u)\n", Option);
    if (Result) *Result = (Option == 6) ? 0x00400101 : 0;   /* AV_QUERY_AV_CAPABILITIES: NTSC, HDTV pack */
    return 0;
}

/* ---- settings (EEPROM) ---- */
#define REG_DWORD 4
#define REG_BINARY 3
u32 STDCALL k_ExQueryNonVolatileSetting(u32 Index, u32 *Type, void *Value, u32 ValueLength, u32 *ResultLength)
{
    u32 v = 0, known = 1;
    const char *env;
    switch (Index) {
    case 7: v = (env = getenv("XBOX_LANGUAGE")) ? parse_u32(env, 10) : 1; break;   /* XC_LANGUAGE: 1 = English */
    case 8:                                          /* XC_VIDEO: 0x10000 widescreen, 0x20000 720p */
        v = 0x00020000;
        if (!((env = getenv("XBOX_WIDESCREEN")) && env[0] == '0')) v |= 0x00010000;
        break;
    case 9: v = 0x00010000; break;                  /* XC_AUDIO: Dolby Digital */
    case 0xA: case 0xB: case 0xC: v = 0; break;     /* parental controls off */
    case 0x103: v = 0x00400100; break;              /* XC_FACTORY_AV_REGION: NTSC-M */
    case 0x104: v = 1; break;                       /* XC_FACTORY_GAME_REGION: North America */
    case 0: case 3: v = 0; break;                   /* timezone bias / std bias */
    default: known = 0;
    }
    klog("ExQueryNonVolatileSetting(%#x)%s -> %#x\n", Index, known ? "" : " (unknown, 0)", v);
    if (Type) *Type = REG_DWORD;
    if (Value && ValueLength) { memset(Value, 0, ValueLength); memcpy(Value, &v, ValueLength < 4 ? ValueLength : 4); }
    if (ResultLength) *ResultLength = 4;
    return STATUS_SUCCESS;
}

/* ---- XBE sections ---- */
u32 STDCALL k_XeLoadSection(u8 *SectionHeader)
{
    /* All sections are mapped at startup; maintain the reference count only. */
    u32 *ref = *(u32 **)(SectionHeader + 0x18);
    if (ref) (*ref)++;
    TRACE("XeLoadSection(%s)\n", (char *)(uintptr_t)*(u32 *)(SectionHeader + 0x14));
    return STATUS_SUCCESS;
}
u32 STDCALL k_XeUnloadSection(u8 *SectionHeader)
{
    u32 *ref = *(u32 **)(SectionHeader + 0x18);
    if (ref && *ref) (*ref)--;
    return STATUS_SUCCESS;
}

/* ---- I/O manager bits used by XAPI's device drivers (memory units, etc.) ---- */
u32 STDCALL k_IoCreateDevice(void *DriverObject, u32 ExtSize, OBJECT_STRING *Name, u32 Type, u32 Exclusive, void **Device)
{
    (void)DriverObject; (void)Type; (void)Exclusive;
    char n[128] = "";
    if (Name) snprintf(n, sizeof n, "%.*s", Name->Length, Name->Buffer);
    klog("IoCreateDevice(%s)\n", n);
    *Device = calloc(1, 0x100 + ExtSize);
    return STATUS_SUCCESS;
}
