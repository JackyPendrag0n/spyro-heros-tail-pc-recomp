/* Guest time stamp counter.
 *
 * Xbox code reads the CPU time stamp counter (rdtsc) and divides by
 * QueryPerformanceFrequency = 733,333,333 Hz (the Xbox CPU clock).  Host TSCs
 * run at several GHz, so every rdtsc in the image is redirected to a counter
 * derived from the host monotonic clock at the Xbox rate. */
#define _GNU_SOURCE
#include "xbox.h"
#include <signal.h>
#include <string.h>
#include <time.h>
#include <ucontext.h>

#define XBOX_TSC_HZ 733333333ULL

static struct timespec g_t0;

u64 xbox_tsc(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    u64 ns = (u64)(t.tv_sec - g_t0.tv_sec) * 1000000000ULL + (u64)(t.tv_nsec - g_t0.tv_nsec);
    /* 733.333... MHz = 11/15 ticks per ns; start well above zero like a booted console */
    return ns / 15 * 11 + (ns % 15) * 11 / 15 + 0x100000000ULL;
}

/* BOOL QueryPerformanceCounter(LARGE_INTEGER *) */
static u32 STDCALL h_QueryPerformanceCounter(u64 *out) { *out = xbox_tsc(); return 1; }
/* functions that are exactly `rdtsc; ret` */
static u64 h_rdtsc_ret(void) { return xbox_tsc(); }

/* inline rdtsc sites are rewritten to ud2 and emulated in the SIGILL handler */
#define MAX_SITES 32
static u32 g_sites[MAX_SITES];
static int g_nsites;

int timing_sigill(void *uc_)
{
    ucontext_t *uc = uc_;
    greg_t *r = uc->uc_mcontext.gregs;
    u32 eip = (u32)r[REG_EIP];
    for (int i = 0; i < g_nsites; i++)
        if (g_sites[i] == eip) {
            u64 t = xbox_tsc();
            r[REG_EAX] = (greg_t)(u32)t;
            r[REG_EDX] = (greg_t)(u32)(t >> 32);
            r[REG_EIP] = (greg_t)(eip + 2);
            return 1;
        }
    return 0;
}

void hle_patch_jmp(u32 at, void *target);
u32 hle_sym(const char *name);

static void patch_inline(u32 at)
{
    u8 *p = (u8 *)(uintptr_t)at;
    if (p[0] != 0x0F || p[1] != 0x31) { klog("timing: no rdtsc at %08x, left alone\n", at); return; }
    if (g_nsites == MAX_SITES) return;
    g_sites[g_nsites++] = at;
    p[0] = 0x0F; p[1] = 0x0B;      /* ud2 */
}

void timing_install(void)
{
    clock_gettime(CLOCK_MONOTONIC, &g_t0);
    u32 qpc = hle_sym("QueryPerformanceCounter");
    const u8 *q = (const u8 *)(uintptr_t)qpc;
    if (qpc && q[4] == 0x0F && q[5] == 0x31) hle_patch_jmp(qpc, h_QueryPerformanceCounter);
    else klog("timing: QueryPerformanceCounter not found or unexpected\n");

    /* Spyro: A Hero's Tail (NTSC) engine timers and D3D: `rdtsc; ret` helpers */
    static const u32 rdtsc_ret[] = { 0x226ae0, 0x28d620 };
    for (unsigned i = 0; i < sizeof rdtsc_ret / sizeof rdtsc_ret[0]; i++) {
        const u8 *p = (const u8 *)(uintptr_t)rdtsc_ret[i];
        if (p[0] == 0x0F && p[1] == 0x31 && p[2] == 0xC3) hle_patch_jmp(rdtsc_ret[i], h_rdtsc_ret);
        else klog("timing: no rdtsc helper at %08x\n", rdtsc_ret[i]);
    }
    static const u32 inline_sites[] = { 0x2268ac, 0x2268ec, 0x22691c, 0x22694e, 0x226993, 0x2269e1 };
    for (unsigned i = 0; i < sizeof inline_sites / sizeof inline_sites[0]; i++) patch_inline(inline_sites[i]);
    klog("timing: guest TSC at 733 MHz (%d inline sites)\n", g_nsites);
}
