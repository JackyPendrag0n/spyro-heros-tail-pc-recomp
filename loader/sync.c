/* Dispatcher objects (events, semaphores, mutants, timers, threads).
 *
 * Xbox code allocates these in its own memory and may read or modify the
 * header inline, so the signal state stays in the game's struct.  A single
 * global lock + condition variable serialises access; waiters re-check on
 * every broadcast.  Simple and correct; can be refined if it ever shows up
 * in profiles. */
#define _GNU_SOURCE
#include "xbox.h"
#include <errno.h>
#include <pthread.h>
#include <time.h>

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cond;

void ksync_init(void)
{
    pthread_condattr_t a;
    pthread_condattr_init(&a);
    pthread_condattr_setclock(&a, CLOCK_MONOTONIC);
    pthread_cond_init(&g_cond, &a);
}

void ksync_lock(void) { pthread_mutex_lock(&g_lock); }
void ksync_unlock(void) { pthread_mutex_unlock(&g_lock); }
void ksync_broadcast(void) { pthread_cond_broadcast(&g_cond); }
void ksync_wait_locked(void) { pthread_cond_wait(&g_cond, &g_lock); }

/* Try to satisfy a wait; called with lock held. Returns 1 if acquired. */
static int try_acquire(DISPATCHER_HEADER *h)
{
    switch (h->Type) {
    case EventNotificationObject:
    case TimerNotificationObject:
    case ThreadObject:
    case ProcessObject:
        return h->SignalState > 0;
    case EventSynchronizationObject:
    case TimerSynchronizationObject:
        if (h->SignalState > 0) { h->SignalState = 0; return 1; }
        return 0;
    case SemaphoreObject:
        if (h->SignalState > 0) { h->SignalState--; return 1; }
        return 0;
    case MutantObject: {
        KMUTANT *m = (KMUTANT *)h;
        void *me = xthread_current_ethread();
        if (h->SignalState > 0 || m->OwnerThread == me) {
            h->SignalState--;
            m->OwnerThread = me;
            return 1;
        }
        return 0;
    }
    default:
        klog("wait on unknown object type %d at %p\n", h->Type, (void *)h);
        return 1;
    }
}

/* timeout: NULL = infinite; negative = relative 100ns; positive = absolute (treated relative to now). */
u32 ksync_wait_object(void *obj, s64 *timeout, int alertable)
{
    (void)alertable;
    DISPATCHER_HEADER *h = obj;
    struct timespec dl;
    if (timeout) {
        s64 t = *timeout;
        if (t > 0) t = 0;                /* absolute times: not used by this game so far */
        s64 ns = (-t) * 100;
        clock_gettime(CLOCK_MONOTONIC, &dl);
        dl.tv_sec += ns / 1000000000LL;
        dl.tv_nsec += ns % 1000000000LL;
        if (dl.tv_nsec >= 1000000000L) { dl.tv_sec++; dl.tv_nsec -= 1000000000L; }
    }
    pthread_mutex_lock(&g_lock);
    for (;;) {
        if (try_acquire(h)) { pthread_mutex_unlock(&g_lock); return STATUS_SUCCESS; }
        if (timeout) {
            if (*timeout == 0) { pthread_mutex_unlock(&g_lock); return STATUS_TIMEOUT; }
            int r = pthread_cond_timedwait(&g_cond, &g_lock, &dl);
            if (r == ETIMEDOUT) {
                int ok = try_acquire(h);
                pthread_mutex_unlock(&g_lock);
                return ok ? STATUS_SUCCESS : STATUS_TIMEOUT;
            }
        } else {
            pthread_cond_wait(&g_cond, &g_lock);
        }
    }
}

s32 ksync_signal_event(KEVENT *ev)
{
    pthread_mutex_lock(&g_lock);
    s32 old = ev->Header.SignalState;
    ev->Header.SignalState = 1;
    pthread_cond_broadcast(&g_cond);
    pthread_mutex_unlock(&g_lock);
    return old;
}
