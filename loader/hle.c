/* High-level emulation of statically linked XDK library functions.
 *
 * Addresses come from XbSymbolDatabase output (<game_dir>/default.xbsym or
 * $XBOX_SYMBOLS), so nothing here is specific to one build of the game. */
#define _GNU_SOURCE
#include "xbox.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct sym { char name[96]; u32 addr; };
static struct sym *g_syms;
static int g_nsyms;

static void load_symbols(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) { klog("hle: no symbol file %s - library hooks disabled\n", path); return; }
    char line[1024];
    int cap = 0;
    while (fgets(line, sizeof line, f)) {
        /* LIB__FUN__conv__Name(params) = 0xADDR   or   LIB__VAR__Name = 0xADDR */
        char *eq = strstr(line, " = 0x");
        if (!eq) continue;
        char *kind = strstr(line, "__FUN__") ? strstr(line, "__FUN__") + 7 : strstr(line, "__VAR__") ? strstr(line, "__VAR__") + 7 : NULL;
        if (!kind) continue;
        if (strstr(line, "__FUN__")) { char *c = strstr(kind, "__"); if (!c) continue; kind = c + 2; }
        size_t n = strcspn(kind, "( ");
        if (g_nsyms == cap) { cap = cap ? cap * 2 : 512; g_syms = realloc(g_syms, cap * sizeof *g_syms); }
        snprintf(g_syms[g_nsyms].name, sizeof g_syms[0].name, "%.*s", (int)n, kind);
        g_syms[g_nsyms].addr = parse_u32(eq + 3, 16);
        g_nsyms++;
    }
    fclose(f);
    klog("hle: %d symbols from %s\n", g_nsyms, path);
}

u32 hle_sym(const char *name)
{
    for (int i = 0; i < g_nsyms; i++)
        if (!strcmp(g_syms[i].name, name)) return g_syms[i].addr;
    return 0;
}

/* Overwrite the start of a game function with `jmp target`. */
void hle_patch_jmp(u32 at, void *target)
{
    u8 *p = (u8 *)(uintptr_t)at;
    p[0] = 0xE9;
    *(u32 *)(p + 1) = (u32)(uintptr_t)target - (at + 5);
}

int hle_hook(const char *name, void *target)
{
    u32 a = hle_sym(name);
    if (!a) { klog("hle: %s not found, not hooked\n", name); return 0; }
    hle_patch_jmp(a, target);
    TRACE("hle: hooked %s @%08x\n", name, a);
    return 1;
}

/* ================= input (XPP / XInput) ================= */
/* XPP_DEVICE_TYPE { ULONG CurrentConnected, ChangeConnected, PreviousConnected; } */
static u32 g_gamepad_type, g_mu_type, g_ir_type;

/* Host input state, filled by a backend (headless: neutral pad on port 0). */
struct pad_state { u16 buttons; u8 analog[8]; s16 lx, ly, rx, ry; };
struct pad_state g_pads[4];
u32 g_pads_connected = 1;          /* bit per port */
static u32 g_packet;

static u32 STDCALL h_XInitDevices(u32 count, void *types)
{
    klog("XInitDevices(%u types) -> HLE, pads connected mask %x\n", count, g_pads_connected);
    if (g_gamepad_type) {
        u32 *t = (u32 *)(uintptr_t)g_gamepad_type;
        t[0] = g_pads_connected; t[1] = g_pads_connected; t[2] = 0;
    }
    return 0;
}

static u32 STDCALL h_XGetDevices(u32 type)
{
    u32 r = 0;
    if (type == g_gamepad_type) r = g_pads_connected;
    if (type) { u32 *t = (u32 *)(uintptr_t)type; t[0] = r; t[1] = 0; }
    TRACE("XGetDevices(%08x) -> %x\n", type, r);
    return r;
}

static u32 STDCALL h_XInputOpen(u32 type, u32 port, u32 slot, void *params)
{
    u32 h = 0;
    if (type == g_gamepad_type && port < 4 && (g_pads_connected >> port & 1)) h = 0x50AD0000 | port;
    klog("XInputOpen(type %08x, port %u, slot %u) -> %08x\n", type, port, slot, h);
    return h;
}

static u32 STDCALL h_XInputClose(u32 h) { klog("XInputClose(%08x)\n", h); return 0; }

static u32 STDCALL h_XInputGetState(u32 h, u8 *state)
{
    u32 port = h & 3;
    if ((h & 0xFFFF0000) != 0x50AD0000 || !(g_pads_connected >> port & 1)) return 1167; /* ERROR_DEVICE_NOT_CONNECTED */
    struct pad_state *p = &g_pads[port];
    *(u32 *)state = ++g_packet;
    *(u16 *)(state + 4) = p->buttons;
    memcpy(state + 6, p->analog, 8);
    *(s16 *)(state + 14) = p->lx; *(s16 *)(state + 16) = p->ly;
    *(s16 *)(state + 18) = p->rx; *(s16 *)(state + 20) = p->ry;
    return 0;
}

static u32 STDCALL h_XInputSetState(u32 h, u8 *feedback)
{
    /* XINPUT_FEEDBACK_HEADER { DWORD dwStatus; HANDLE hEvent; ... }: rumble accepted, done. */
    (void)h;
    *(u32 *)feedback = 0;
    return 997;  /* ERROR_IO_PENDING, as the real driver reports for async feedback */
}

static void install_input(void)
{
    g_gamepad_type = hle_sym("g_DeviceType_Gamepad");
    g_mu_type = hle_sym("g_DeviceType_MU");
    g_ir_type = hle_sym("g_DeviceType_IRDongle");
    hle_hook("XInitDevices", h_XInitDevices);
    hle_hook("XGetDevices", h_XGetDevices);
    hle_hook("XInputOpen", h_XInputOpen);
    hle_hook("XInputClose", h_XInputClose);
    hle_hook("XInputGetState", h_XInputGetState);
    hle_hook("XInputSetState", h_XInputSetState);
}

void d3d_install(void);
void dsound_install(void);

void hle_install(const char *symbols_path)
{
    load_symbols(symbols_path);
    if (!g_nsyms) return;
    install_input();
    { extern void timing_install(void); timing_install(); }
    d3d_install();
    dsound_install();
    { extern void xmv_install(void); xmv_install(); }
}
