/* Threads, waits, events, timers, DPCs, IRQL, time. */
#define _GNU_SOURCE
#include "xbox.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

u32 g_tls_data_size;
static u32 g_next_tid = 0x10;

struct tstart {
    u32 TlsDataSize, ExtSize;
    void *StartRoutine, *StartContext, *SystemRoutine;
    u8 *ethread;
    u32 handle;
};

typedef void STDCALL (*sysroutine_t)(void *, void *);
typedef void STDCALL (*startroutine_t)(void *);

/* Run an Xbox thread body on this host thread. The TLS block lives on our
 * stack directly under NT_TIB.StackBase, as the Xbox kernel lays it out
 * (game code reads TLS at [fs:[4] + tls_index*4], tls_index negative). */
static void run_xbox_thread(struct tstart *s)
{
    u32 tls = (s->TlsDataSize + 15) & ~15u;
    u8 *block = __builtin_alloca(tls + 32);
    u8 *tlsdata = (u8 *)(((uintptr_t)block + 15) & ~(uintptr_t)15);
    memset(tlsdata, 0, tls);
    u8 *stack_base = tlsdata + s->TlsDataSize;
    pthread_attr_t a;
    void *stk_lo = NULL; size_t stk_sz = 0;
    pthread_getattr_np(pthread_self(), &a);
    pthread_attr_getstack(&a, &stk_lo, &stk_sz);
    pthread_attr_destroy(&a);

    *(void **)(s->ethread + KT_TLSDATA) = s->TlsDataSize ? tlsdata : NULL;
    *(void **)(s->ethread + KT_STACKBASE) = stack_base;
    *(void **)(s->ethread + KT_STACKLIMIT) = stk_lo;
    xthread_setup_current(s->ethread, stack_base, stk_lo);

    if (s->SystemRoutine)
        ((sysroutine_t)s->SystemRoutine)(s->StartRoutine, s->StartContext);
    else
        ((startroutine_t)s->StartRoutine)(s->StartContext);
}

static void thread_exit_signal(u8 *ethread)
{
    ksync_lock();
    ((DISPATCHER_HEADER *)ethread)->SignalState = 1;
    ksync_broadcast();
    ksync_unlock();
}

static void *host_thread_main(void *p)
{
    struct tstart *s = p;
    run_xbox_thread(s);
    klog("thread %p returned from its system routine\n", (void *)s->ethread);
    thread_exit_signal(s->ethread);
    free(s);
    return NULL;
}

void *kthread_new_ethread(u32 ext)
{
    u8 *e = calloc(1, KTHREAD_SIZE + ext);
    DISPATCHER_HEADER *h = (DISPATCHER_HEADER *)e;
    h->Type = ThreadObject;
    h->Size = KTHREAD_SIZE / 4;
    h->WaitListHead.Flink = h->WaitListHead.Blink = &h->WaitListHead;
    *(u32 *)(e + ET_UNIQUETHREAD) = __sync_fetch_and_add(&g_next_tid, 4);
    e[KT_PRIORITY] = 8;
    return e;
}

u32 STDCALL k_PsCreateSystemThreadEx(u32 *ThreadHandle, u32 ThreadExtensionSize, u32 KernelStackSize,
                                     u32 TlsDataSize, u32 *ThreadId, void *StartRoutine, void *StartContext,
                                     u32 CreateSuspended, u32 DebuggerThread, void *SystemRoutine)
{
    (void)DebuggerThread;
    struct tstart *s = calloc(1, sizeof *s);
    s->TlsDataSize = TlsDataSize;
    s->StartRoutine = StartRoutine;
    s->StartContext = StartContext;
    s->SystemRoutine = SystemRoutine;
    s->ethread = kthread_new_ethread(ThreadExtensionSize);
    u32 h = handle_alloc(H_THREAD);
    handle_get(h)->object = s->ethread;
    s->handle = h;
    if (ThreadHandle) *ThreadHandle = h;
    if (ThreadId) *ThreadId = *(u32 *)(s->ethread + ET_UNIQUETHREAD);
    klog("PsCreateSystemThreadEx(start=%p ctx=%p sys=%p stack=%x tls=%x%s) -> handle %x tid %x\n",
         StartRoutine, StartContext, SystemRoutine, KernelStackSize, TlsDataSize,
         CreateSuspended ? " SUSPENDED(ignored)" : "", h, *(u32 *)(s->ethread + ET_UNIQUETHREAD));
    pthread_attr_t a;
    pthread_attr_init(&a);
    size_t ss = KernelStackSize * 4;
    if (ss < (4u << 20)) ss = 4u << 20;             /* host code needs headroom */
    pthread_attr_setstacksize(&a, ss);
    pthread_t t;
    pthread_create(&t, &a, host_thread_main, s);
    pthread_attr_destroy(&a);
    pthread_detach(t);
    return STATUS_SUCCESS;
}

u32 STDCALL k_PsTerminateSystemThread(u32 ExitStatus)
{
    klog("PsTerminateSystemThread(%x) on thread %p\n", ExitStatus, xthread_current_ethread());
    thread_exit_signal(xthread_current_ethread());
    pthread_exit(NULL);
}

static void *object_of(u32 Handle)
{
    if (Handle == 0xFFFFFFFE) return xthread_current_ethread();   /* NtCurrentThread() */
    struct handle *h = handle_get(Handle);
    return h ? h->object : NULL;
}

u32 STDCALL k_ObReferenceObjectByHandle(u32 Handle, void *ObjectType, void **Object)
{
    (void)ObjectType;
    void *o = object_of(Handle);
    TRACE("ObReferenceObjectByHandle(%x) -> %p\n", Handle, o);
    if (!o) return STATUS_INVALID_HANDLE;
    *Object = o;
    return STATUS_SUCCESS;
}
u32 FASTCALL k_ObfDereferenceObject(u32 Object) { (void)Object; return 0; }

u32 STDCALL k_KeWaitForSingleObject(void *Object, u32 WaitReason, u32 WaitMode, u32 Alertable, s64 *Timeout)
{
    (void)WaitReason; (void)WaitMode;
    return ksync_wait_object(Object, Timeout, Alertable);
}

u32 STDCALL k_NtWaitForSingleObjectEx(u32 Handle, u32 WaitMode, u32 Alertable, s64 *Timeout)
{
    (void)WaitMode;
    void *o = object_of(Handle);
    if (!o) { klog("NtWaitForSingleObjectEx: bad handle %x\n", Handle); return STATUS_INVALID_HANDLE; }
    return ksync_wait_object(o, Timeout, Alertable);
}

u32 STDCALL k_NtWaitForSingleObject(u32 Handle, u32 Alertable, s64 *Timeout)
{
    return k_NtWaitForSingleObjectEx(Handle, 0, Alertable, Timeout);
}

u32 STDCALL k_KeSetEvent(KEVENT *Event, u32 Increment, u32 Wait)
{
    (void)Increment; (void)Wait;
    return (u32)ksync_signal_event(Event);
}

u32 STDCALL k_NtSetEvent(u32 Handle, s32 *PreviousState)
{
    void *o = object_of(Handle);
    if (!o) return STATUS_INVALID_HANDLE;
    s32 old = ksync_signal_event(o);
    if (PreviousState) *PreviousState = old;
    return STATUS_SUCCESS;
}

u32 STDCALL k_KeDelayExecutionThread(u32 WaitMode, u32 Alertable, s64 *Interval)
{
    (void)WaitMode; (void)Alertable;
    s64 t = *Interval;
    if (t < 0) {
        struct timespec ts = { (-t) / 10000000, ((-t) % 10000000) * 100 };
        if (ts.tv_sec == 0 && ts.tv_nsec == 0) sched_yield();
        else nanosleep(&ts, NULL);
    } else {
        sched_yield();
    }
    return STATUS_SUCCESS;
}

u32 STDCALL k_KeSetBasePriorityThread(void *Thread, s32 Increment)
{
    (void)Thread;
    TRACE("KeSetBasePriorityThread(%p, %d)\n", Thread, Increment);
    return 0;
}

u32 STDCALL k_KeStallExecutionProcessor(u32 us) { usleep(us); return 0; }

/* ---- IRQL ---- */
u32 FASTCALL k_KfRaiseIrql(u32 NewIrql)
{
    KPCR *p = xthread_pcr(); u8 old = p->Irql; p->Irql = (u8)NewIrql; return old;
}
u32 FASTCALL k_KfLowerIrql(u32 NewIrql) { xthread_pcr()->Irql = (u8)NewIrql; return 0; }
u32 STDCALL k_KeRaiseIrqlToDpcLevel(void) { return k_KfRaiseIrql(2); }

/* ---- time ---- */
extern volatile u32 kv_KeTickCount[];
static struct timespec g_boot;

static void *ticker(void *p)
{
    (void)p;
    for (;;) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        kv_KeTickCount[0] = (u32)((now.tv_sec - g_boot.tv_sec) * 1000 + (now.tv_nsec - g_boot.tv_nsec) / 1000000);
        usleep(1000);
    }
    return NULL;
}

/* ---- timers + DPCs: one service thread ---- */
struct ktimer_ent { KTIMER *t; struct timespec due; s32 period_ms; int active; };
#define MAXTIMERS 128
static struct ktimer_ent g_timers[MAXTIMERS];
static KDPC *g_dpcq[256];
static int g_dpcq_n;
static pthread_mutex_t g_tlock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_tcond = PTHREAD_COND_INITIALIZER;

typedef void STDCALL (*dpc_routine_t)(KDPC *, void *, void *, void *);

static void ts_add_ms(struct timespec *t, s64 ms)
{
    t->tv_sec += ms / 1000; t->tv_nsec += (ms % 1000) * 1000000;
    if (t->tv_nsec >= 1000000000L) { t->tv_sec++; t->tv_nsec -= 1000000000L; }
}

static void *timer_thread(void *p)
{
    (void)p;
    /* DPC routines are Xbox code: give this thread an Xbox environment */
    struct tstart s = { 0 };
    s.ethread = kthread_new_ethread(0);
    u8 tlsdata[256] = { 0 };
    *(void **)(s.ethread + KT_TLSDATA) = tlsdata;
    xthread_setup_current(s.ethread, tlsdata + sizeof tlsdata, NULL);
    xthread_pcr()->Irql = 2;
    pthread_mutex_lock(&g_tlock);
    for (;;) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        struct timespec next = now; next.tv_sec += 1;
        for (int i = 0; i < MAXTIMERS; i++) {
            struct ktimer_ent *e = &g_timers[i];
            if (!e->active) continue;
            if (e->due.tv_sec < now.tv_sec || (e->due.tv_sec == now.tv_sec && e->due.tv_nsec <= now.tv_nsec)) {
                ksync_lock();
                e->t->Header.SignalState = 1;
                e->t->Header.Inserted = e->period_ms > 0;
                ksync_broadcast();
                ksync_unlock();
                if (e->t->Dpc && g_dpcq_n < 256) g_dpcq[g_dpcq_n++] = e->t->Dpc;
                if (e->period_ms > 0) ts_add_ms(&e->due, e->period_ms);
                else e->active = 0;
            }
            if (e->active && (e->due.tv_sec < next.tv_sec || (e->due.tv_sec == next.tv_sec && e->due.tv_nsec < next.tv_nsec)))
                next = e->due;
        }
        while (g_dpcq_n) {
            KDPC *d = g_dpcq[0];
            memmove(g_dpcq, g_dpcq + 1, --g_dpcq_n * sizeof g_dpcq[0]);
            d->Inserted = 0;
            pthread_mutex_unlock(&g_tlock);
            xthread_pcr()->DpcRoutineActive = 1;
            ((dpc_routine_t)d->DeferredRoutine)(d, d->DeferredContext, d->SystemArgument1, d->SystemArgument2);
            xthread_pcr()->DpcRoutineActive = 0;
            pthread_mutex_lock(&g_tlock);
        }
        pthread_cond_timedwait(&g_tcond, &g_tlock, &next);
    }
    return NULL;
}

u32 STDCALL k_KeInitializeDpc(KDPC *Dpc, void *DeferredRoutine, void *DeferredContext)
{
    memset(Dpc, 0, sizeof *Dpc);
    Dpc->Type = DpcObject;
    Dpc->DeferredRoutine = DeferredRoutine;
    Dpc->DeferredContext = DeferredContext;
    return 0;
}

u32 STDCALL k_KeInsertQueueDpc(KDPC *Dpc, void *Arg1, void *Arg2)
{
    pthread_mutex_lock(&g_tlock);
    u32 r = 0;
    if (!Dpc->Inserted && g_dpcq_n < 256) {
        Dpc->Inserted = 1; Dpc->SystemArgument1 = Arg1; Dpc->SystemArgument2 = Arg2;
        g_dpcq[g_dpcq_n++] = Dpc; r = 1;
        pthread_cond_signal(&g_tcond);
    }
    pthread_mutex_unlock(&g_tlock);
    return r;
}

u32 STDCALL k_KeRemoveQueueDpc(KDPC *Dpc)
{
    pthread_mutex_lock(&g_tlock);
    u32 r = 0;
    for (int i = 0; i < g_dpcq_n; i++)
        if (g_dpcq[i] == Dpc) { memmove(g_dpcq + i, g_dpcq + i + 1, (--g_dpcq_n - i) * sizeof g_dpcq[0]); r = 1; break; }
    Dpc->Inserted = 0;
    pthread_mutex_unlock(&g_tlock);
    return r;
}

u32 STDCALL k_KeInitializeTimerEx(KTIMER *Timer, u32 Type)
{
    memset(Timer, 0, sizeof *Timer);
    Timer->Header.Type = Type ? TimerSynchronizationObject : TimerNotificationObject;
    Timer->Header.Size = sizeof(KTIMER) / 4;
    Timer->Header.WaitListHead.Flink = Timer->Header.WaitListHead.Blink = &Timer->Header.WaitListHead;
    return 0;
}

static u32 cancel_locked(KTIMER *t)
{
    u32 was = 0;
    for (int i = 0; i < MAXTIMERS; i++)
        if (g_timers[i].active && g_timers[i].t == t) { g_timers[i].active = 0; was = 1; }
    t->Header.Inserted = 0;
    return was;
}

u32 STDCALL k_KeSetTimer(KTIMER *Timer, u32 DueLo, u32 DueHi, KDPC *Dpc)
{
    s64 due = (s64)(((u64)DueHi << 32) | DueLo);
    pthread_mutex_lock(&g_tlock);
    u32 was = cancel_locked(Timer);
    Timer->Dpc = Dpc;
    Timer->Header.SignalState = 0;
    Timer->Header.Inserted = 1;
    int i = 0;
    while (i < MAXTIMERS && g_timers[i].active) i++;
    if (i < MAXTIMERS) {
        struct ktimer_ent *e = &g_timers[i];
        e->t = Timer; e->active = 1; e->period_ms = Timer->Period;
        clock_gettime(CLOCK_MONOTONIC, &e->due);
        s64 ms = due < 0 ? (-due) / 10000 : 0;
        ts_add_ms(&e->due, ms);
    }
    pthread_cond_signal(&g_tcond);
    pthread_mutex_unlock(&g_tlock);
    TRACE("KeSetTimer(%p, %lld, dpc %p)\n", (void *)Timer, (long long)due, (void *)Dpc);
    return was;
}

u32 STDCALL k_KeCancelTimer(KTIMER *Timer)
{
    pthread_mutex_lock(&g_tlock);
    u32 was = cancel_locked(Timer);
    pthread_mutex_unlock(&g_tlock);
    return was;
}

void kthread_init(void)
{
    clock_gettime(CLOCK_MONOTONIC, &g_boot);
    pthread_t t;
    pthread_create(&t, NULL, ticker, NULL);
    pthread_detach(t);
    pthread_create(&t, NULL, timer_thread, NULL);
    pthread_detach(t);
}

u32 STDCALL k_KeQuerySystemTime(s64 *CurrentTime)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    *CurrentTime = ((s64)ts.tv_sec + 11644473600LL) * 10000000LL + ts.tv_nsec / 100;
    return 0;
}

/* Interrupt plumbing is only used by the hardware drivers (D3D/DSOUND), which are HLE'd. */
u32 STDCALL k_KeInitializeInterrupt(void *a, void *b, void *c, u32 d, u32 e, u32 f, u32 g)
{ (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)g; TRACE("KeInitializeInterrupt\n"); return 0; }
u32 STDCALL k_KeConnectInterrupt(void *i) { (void)i; TRACE("KeConnectInterrupt\n"); return 1; }
u32 STDCALL k_KeDisconnectInterrupt(void *i) { (void)i; return 1; }
u32 STDCALL k_HalGetInterruptVector(u32 Level, u8 *Irql) { if (Irql) *Irql = (u8)(27 - Level); return 0x30 + Level; }

typedef u32 STDCALL (*sync_routine_t)(void *);
u32 STDCALL k_KeSynchronizeExecution(void *Interrupt, void *Routine, void *Context)
{ (void)Interrupt; return ((sync_routine_t)Routine)(Context); }

u32 STDCALL k_KeSaveFloatingPointState(void *s) { (void)s; return STATUS_SUCCESS; }
u32 STDCALL k_KeRestoreFloatingPointState(void *s) { (void)s; return STATUS_SUCCESS; }
