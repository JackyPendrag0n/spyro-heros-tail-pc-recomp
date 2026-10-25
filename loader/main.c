/* xboxrun: run an original-Xbox XBE natively on 32-bit x86 Linux.
 *
 * usage: xboxrun <game_dir>       (game_dir contains default.xbe)
 * env:   XBOX_TRACE=1             log every kernel call
 *        XBOX_SAVE_ROOT=<dir>     emulated hard disk (default <game_dir>/../xbox_hdd)
 *        XBOX_SYMBOLS=<file>      XbSymbolDatabase output for HLE hooks */
#define _GNU_SOURCE
#include "xbox.h"
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ucontext.h>
#include <sys/mman.h>
#include <unistd.h>

void kmem_init(void);
void kthread_init(void);
void *kthread_new_ethread(u32 ext);

static struct xbe_info g_xbe;

/* XAPI startup inspects the kernel image header at 0x80010000 (it looks for an
 * INIT section to reclaim).  Provide a minimal PE header with one section. */
static void map_fake_kernel_header(void)
{
    u8 *k = mmap((void *)0x80010000, 0x1000, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (k != (u8 *)0x80010000) { fprintf(stderr, "cannot map fake kernel header\n"); exit(1); }
    k[0] = 'M'; k[1] = 'Z';
    *(u32 *)(k + 0x3C) = 0x80;                 /* e_lfanew */
    u8 *nt = k + 0x80;
    memcpy(nt, "PE\0\0", 4);
    *(u16 *)(nt + 4) = 0x14C;                  /* i386 */
    *(u16 *)(nt + 6) = 1;                      /* NumberOfSections */
    *(u16 *)(nt + 0x14) = 0xE0;                /* SizeOfOptionalHeader */
    memcpy(nt + 0x18 + 0xE0, ".text\0\0\0", 8);
}

static void crash(int sig, siginfo_t *si, void *uc_)
{
    ucontext_t *uc = uc_;
    greg_t *r = uc->uc_mcontext.gregs;
    { extern int timing_sigill(void *); if (sig == SIGILL && timing_sigill(uc_)) return; }
    {   /* cache flushes the game's D3D code does (wbinvd / invd, privileged: a fault here): skip them.
         * (Sparx's mini-game reaches one in a material set-up, 0x22dda4.) */
        const u8 *ip = (const u8 *)(uintptr_t)r[REG_EIP];
        if (sig == SIGSEGV && r[REG_EIP] >= 0x11000 && r[REG_EIP] < 0x4c3000 && ip[0] == 0x0f && (ip[1] == 0x09 || ip[1] == 0x08)) {
            static int told; if (!told++) fprintf(stderr, "cpu: skipping %s at %08x\n", ip[1] == 0x09 ? "wbinvd" : "invd", (u32)r[REG_EIP]);
            r[REG_EIP] += 2; return;
        }
    }
    fprintf(stderr, "\n*** signal %d (%s) at eip=%08x fault addr=%p thread=%p\n",
            sig, strsignal(sig), (u32)r[REG_EIP], si->si_addr, xthread_current_ethread());
    fprintf(stderr, "eax=%08x ebx=%08x ecx=%08x edx=%08x esi=%08x edi=%08x ebp=%08x esp=%08x\n",
            (u32)r[REG_EAX], (u32)r[REG_EBX], (u32)r[REG_ECX], (u32)r[REG_EDX],
            (u32)r[REG_ESI], (u32)r[REG_EDI], (u32)r[REG_EBP], (u32)r[REG_ESP]);
    u32 *sp = (u32 *)(uintptr_t)r[REG_ESP];
    fprintf(stderr, "probable return addresses in game code:");
    int n = 0;
    for (int i = 0; i < 2048 && n < 24; i++) {
        u32 v = sp[i];
        if (v >= 0x11000 && v < 0x4c3000) { fprintf(stderr, " %08x", v); n++; }
    }
    fprintf(stderr, "\n");
    fflush(stderr);
    _exit(128 + sig);
}

static void install_crash_handler(void)
{
    static u8 altstack[1 << 16];
    stack_t ss = { .ss_sp = altstack, .ss_size = sizeof altstack };
    sigaltstack(&ss, NULL);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = crash;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGTRAP, &sa, NULL);
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s <game_dir>\n", argv[0]); return 1; }
    g_trace = getenv("XBOX_TRACE") && parse_u32(getenv("XBOX_TRACE"), 10);
    setvbuf(stderr, NULL, _IOLBF, 0);

    char xbe_path[1024];
    snprintf(xbe_path, sizeof xbe_path, "%s/default.xbe", argv[1]);
    kmem_init();
    map_fake_kernel_header();
    if (xbe_load(xbe_path, &g_xbe)) return 1;
    ksync_init();
    kthread_init();
    fs_init(argv[1]);
    install_crash_handler();
    xthread_init_gdt();
    /* symbol file: $XBOX_SYMBOLS, else next to this executable, else in the game folder */
    char sym_path[1024];
    if (getenv("XBOX_SYMBOLS")) snprintf(sym_path, sizeof sym_path, "%s", getenv("XBOX_SYMBOLS"));
    else {
        char exe[900];
        ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
        exe[n > 0 ? n : 0] = 0;
        char *slash = strrchr(exe, '/');
        if (slash) *slash = 0;
        snprintf(sym_path, sizeof sym_path, "%s/default.xbsym", exe);
        if (access(sym_path, R_OK)) snprintf(sym_path, sizeof sym_path, "%s/default.xbsym", argv[1]);
    }
    hle_install(sym_path);

    /* The kernel's boot thread runs the entry point; give it an Xbox identity. */
    u8 *eth = kthread_new_ethread(0);
    u8 tls[0x200] __attribute__((aligned(16)));
    memset(tls, 0, sizeof tls);
    *(void **)(eth + KT_TLSDATA) = tls;
    xthread_setup_current(eth, tls + sizeof tls, NULL);

    klog("jumping to entry point %08x\n", g_xbe.entry);
    void (*entry)(void) = (void (*)(void))(uintptr_t)g_xbe.entry;
    entry();
    klog("entry point returned; boot thread parks (game runs on its own threads)\n");
    pthread_exit(NULL);
}
