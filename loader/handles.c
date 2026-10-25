/* Xbox handle table.  Handles are small multiples of 4, like the real kernel. */
#include "xbox.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define MAX_HANDLES 4096
static struct handle g_handles[MAX_HANDLES];
static pthread_mutex_t g_hlock = PTHREAD_MUTEX_INITIALIZER;

u32 handle_alloc(enum htype t)
{
    pthread_mutex_lock(&g_hlock);
    for (int i = 1; i < MAX_HANDLES; i++) {
        if (g_handles[i].type == H_FREE) {
            memset(&g_handles[i], 0, sizeof g_handles[i]);
            g_handles[i].type = t;
            g_handles[i].fd = -1;
            pthread_mutex_unlock(&g_hlock);
            return (u32)i * 4;
        }
    }
    pthread_mutex_unlock(&g_hlock);
    klog("handle table full\n");
    return 0;
}

struct handle *handle_get(u32 h)
{
    if (h & 3) return NULL;
    u32 i = h / 4;
    if (i == 0 || i >= MAX_HANDLES || g_handles[i].type == H_FREE) return NULL;
    return &g_handles[i];
}

void handle_free(u32 h)
{
    struct handle *p = handle_get(h);
    if (!p) return;
    free(p->host_path);
    free(p->xbox_path);
    pthread_mutex_lock(&g_hlock);
    p->type = H_FREE;
    pthread_mutex_unlock(&g_hlock);
}
