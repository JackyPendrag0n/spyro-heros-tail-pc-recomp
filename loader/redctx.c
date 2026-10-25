/* Red for every player: everything Red's code keeps about "the player" (his body, his moves, pose,
 * aim, magic, size, flight camera...) is marked RS and lands in one section, redst. The section holds
 * the state of the player being run; the others' copies are kept here and swapped in whenever the
 * game runs one of them (coop.c sw_in / sw_out: their item's update, their Red tick, their pad,
 * their view). A player's copy starts from the section as the program started (every variable's
 * initial value). */
#include "xbox.h"
#include <string.h>
#include <stdlib.h>

extern char __start_redst[], __stop_redst[];
#define NCTX 4
static char *g_pristine, *g_buf[NCTX];  /* the starting values; each player's copy (stale for the live one) */
static int g_cur;                       /* whose state is in the section (player index 0..3) */

static size_t rs_size(void) { return (size_t)(__stop_redst - __start_redst); }
__attribute__((constructor)) static void redctx_init(void)
{
    size_t n = rs_size();
    g_pristine = malloc(n); memcpy(g_pristine, __start_redst, n);
    for (int i = 0; i < NCTX; i++) { g_buf[i] = malloc(n); memcpy(g_buf[i], __start_redst, n); }
}
int red_ctx(void) { return g_cur; }
/* make player p's state the live one; returns whose it was */
int red_ctx_set(int p)
{
    int was = g_cur;
    if (p == g_cur || p < 0 || p >= NCTX || !g_buf[p]) return was;
    size_t n = rs_size();
    memcpy(g_buf[g_cur], __start_redst, n); memcpy(__start_redst, g_buf[p], n);
    g_cur = p;
    return was;
}
/* player p starts over (they left, or are someone new) */
void red_ctx_reset(int p)
{
    if (p < 0 || p >= NCTX || !g_buf[p]) return;
    memcpy(p == g_cur ? __start_redst : g_buf[p], g_pristine, rs_size());
}
size_t red_ctx_bytes(void) { return rs_size(); }
