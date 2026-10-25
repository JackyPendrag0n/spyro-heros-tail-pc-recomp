/* Per-thread Xbox processor control region, reached through the FS segment.
 *
 * i386 glibc uses GS for its own TLS, leaving FS free.  We grab one of the three
 * per-thread GDT TLS slots with set_thread_area() and, in every thread that runs
 * Xbox code, point it at that thread's KPCR. */
#define _GNU_SOURCE
#include "xbox.h"
#include <asm/ldt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

static int g_entry = -1;
static __thread KPCR *t_pcr;

static int set_area(struct user_desc *d)
{
    return syscall(SYS_set_thread_area, d);
}

static void load_fs(void *base)
{
    struct user_desc d;
    memset(&d, 0, sizeof d);
    d.entry_number = g_entry;
    d.base_addr = (u32)(uintptr_t)base;
    d.limit = 0xFFFFF;              /* pages -> whole 4 GB, base-relative */
    d.seg_32bit = 1;
    d.limit_in_pages = 1;
    d.useable = 1;
    if (set_area(&d) != 0) { perror("set_thread_area"); abort(); }
    g_entry = d.entry_number;
    u16 sel = (u16)((g_entry << 3) | 3);
    __asm__ volatile("movw %0, %%fs" : : "r"(sel));
}

void xthread_init_gdt(void)
{
    /* allocate a slot once (entry_number = -1 asks the kernel for a free one) */
    static KPCR dummy;
    g_entry = -1;
    load_fs(&dummy);
    klog("FS uses GDT TLS entry %d\n", g_entry);
}

KPCR *xthread_setup_current(void *ethread, void *stack_base, void *stack_limit)
{
    KPCR *p = calloc(1, sizeof(KPCR));
    p->NtTib.ExceptionList = (void *)0xFFFFFFFF;
    p->NtTib.StackBase = stack_base;
    p->NtTib.StackLimit = stack_limit;
    p->NtTib.Self = &p->NtTib;
    p->SelfPcr = p;
    p->Prcb = &p->CurrentThread;
    p->Irql = 0;
    p->CurrentThread = ethread;
    p->DpcListHead.Flink = p->DpcListHead.Blink = &p->DpcListHead;
    t_pcr = p;
    load_fs(p);
    return p;
}

KPCR *xthread_pcr(void) { return t_pcr; }
void *xthread_current_ethread(void) { return t_pcr ? t_pcr->CurrentThread : NULL; }
