/* Xbox object-namespace paths -> host paths.
 *
 *   \??\X:            symbolic links (D: is the game disc, others created by
 *                     the game through IoCreateSymbolicLink)
 *   \Device\CdRom0    -> <game_root>
 *   \Device\Harddisk0\PartitionN -> <save_root>/PartitionN
 *
 * Lookup is case-insensitive per path component, like FATX/ISO on the Xbox. */
#define _GNU_SOURCE
#include "xbox.h"
#include <ctype.h>
#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

struct link { char name[64]; char target[256]; };
static struct link g_links[64];
static int g_nlinks;
static pthread_mutex_t g_fslock = PTHREAD_MUTEX_INITIALIZER;
static char g_game_root[512], g_save_root[512];

/* Filename index of the game folder: lets the disc layout of a dump differ
 * from the paths baked into the executable (e.g. dev "d:\\Spyro\\Binary\\..."). */
struct fent { char *name; char *path; };
static struct fent *g_index;
static int g_nindex, g_capindex;

static void index_dir(const char *dir, int depth)
{
    if (depth > 8) return;
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char p[2048];
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(p, &st)) continue;
        if (S_ISDIR(st.st_mode)) { index_dir(p, depth + 1); continue; }
        if (g_nindex == g_capindex) { g_capindex = g_capindex ? g_capindex * 2 : 256; g_index = realloc(g_index, g_capindex * sizeof *g_index); }
        g_index[g_nindex].name = strdup(e->d_name);
        g_index[g_nindex].path = strdup(p);
        g_nindex++;
    }
    closedir(d);
}

static const char *index_lookup(const char *name)
{
    for (int i = 0; i < g_nindex; i++)
        if (!strcasecmp(g_index[i].name, name)) return g_index[i].path;
    return NULL;
}

const char *fs_find(const char *name) { return index_lookup(name); }
const char *fs_save_dir(void) { return g_save_root; }   /* host path of a game file */

void fs_add_link(const char *name, const char *target)
{
    pthread_mutex_lock(&g_fslock);
    int i;
    for (i = 0; i < g_nlinks; i++)
        if (!strcasecmp(g_links[i].name, name)) break;
    if (i == g_nlinks && g_nlinks < 64) g_nlinks++;
    snprintf(g_links[i].name, sizeof g_links[i].name, "%s", name);
    snprintf(g_links[i].target, sizeof g_links[i].target, "%s", target);
    pthread_mutex_unlock(&g_fslock);
    klog("symlink %s -> %s\n", name, target);
}

void fs_del_link(const char *name)
{
    pthread_mutex_lock(&g_fslock);
    for (int i = 0; i < g_nlinks; i++)
        if (!strcasecmp(g_links[i].name, name)) { g_links[i] = g_links[--g_nlinks]; break; }
    pthread_mutex_unlock(&g_fslock);
}

const char *fs_get_link(const char *name)
{
    for (int i = 0; i < g_nlinks; i++)
        if (!strcasecmp(g_links[i].name, name)) return g_links[i].target;
    return NULL;
}

void fs_init(const char *game_root)
{
    snprintf(g_game_root, sizeof g_game_root, "%s", game_root);
    const char *sr = getenv("XBOX_SAVE_ROOT");
    if (sr) snprintf(g_save_root, sizeof g_save_root, "%s", sr);
    else snprintf(g_save_root, sizeof g_save_root, "%s/../xbox_hdd", game_root);
    for (int p = 1; p <= 7; p++) {
        char d[600];
        snprintf(d, sizeof d, "%s/Partition%d", g_save_root, p);
        mkdir(g_save_root, 0755);
        mkdir(d, 0755);
    }
    char img[600];
    snprintf(img, sizeof img, "%s/Partition0.img", g_save_root);
    struct stat st;
    if (stat(img, &st) || st.st_size < 0x100000) {
        FILE *f = fopen(img, "ab");
        if (f) fclose(f);
        if (truncate(img, 0x100000)) perror(img);
    }
    fs_add_link("\\??\\D:", "\\Device\\CdRom0");
    index_dir(g_game_root, 0);
    klog("game root %s (%d files indexed), save root %s\n", g_game_root, g_nindex, g_save_root);
}

/* Append path component case-insensitively; create=1 keeps the name if missing. */
static void append_ci(char *host, size_t cap, const char *comp)
{
    size_t n = strlen(host);
    DIR *d = opendir(host);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (!strcasecmp(e->d_name, comp)) {
                snprintf(host + n, cap - n, "/%s", e->d_name);
                closedir(d);
                return;
            }
        }
        closedir(d);
    }
    snprintf(host + n, cap - n, "/%s", comp);
}

/* Build the full Xbox object path from OBJECT_ATTRIBUTES (root dir handle + name). */
static void full_xbox_path(char *out, size_t cap, const char *name, OBJECT_ATTRIBUTES *oa)
{
    out[0] = 0;
    if (oa && oa->RootDirectory) {
        u32 rd = (u32)(uintptr_t)oa->RootDirectory;
        if (rd == 0xFFFFFFFD) {                      /* ObDosDevicesDirectory() */
            snprintf(out, cap, "\\??\\%s", name);
            return;
        }
        struct handle *h = handle_get(rd);
        if (h && h->xbox_path) {
            snprintf(out, cap, "%s%s%s", h->xbox_path,
                     (name[0] == '\\' || !name[0]) ? "" : "\\", name);
            return;
        }
    }
    snprintf(out, cap, "%s", name);
}

/* Returns malloc'd host path, or NULL if the device is unknown. */
char *fs_resolve(const char *name, OBJECT_ATTRIBUTES *oa)
{
    char xp[1024], tmp[1024];
    full_xbox_path(xp, sizeof xp, name, oa);

    /* expand symbolic links (\??\X: and anything registered) */
    for (int guard = 0; guard < 8; guard++) {
        int hit = 0;
        pthread_mutex_lock(&g_fslock);
        for (int i = 0; i < g_nlinks; i++) {
            size_t ln = strlen(g_links[i].name);
            if (!strncasecmp(xp, g_links[i].name, ln) && (xp[ln] == '\\' || xp[ln] == 0)) {
                snprintf(tmp, sizeof tmp, "%s%s", g_links[i].target, xp + ln);
                strcpy(xp, tmp);
                hit = 1;
                break;
            }
        }
        pthread_mutex_unlock(&g_fslock);
        if (!hit) break;
    }

    char host[2048];
    const char *rest;
    int is_disc = 0;
    if (!strncasecmp(xp, "\\Device\\CdRom0", 14)) {
        snprintf(host, sizeof host, "%s", g_game_root);
        rest = xp + 14;
        is_disc = 1;
    } else if (!strncasecmp(xp, "\\Device\\Harddisk0\\Partition0", 28) && (!xp[28] || xp[28] == '\\')) {
        /* raw disk: a file holding the partition-table / config sectors */
        snprintf(host, sizeof host, "%s/Partition0.img", g_save_root);
        return strdup(host);
    } else if (!strncasecmp(xp, "\\Device\\Harddisk0\\Partition", 27) && isdigit((u8)xp[27])) {
        snprintf(host, sizeof host, "%s/Partition%c", g_save_root, xp[27]);
        rest = xp + 28;
    } else {
        klog("fs: unknown device in path '%s'\n", xp);
        return NULL;
    }
    /* walk components */
    char comp[256];
    while (*rest) {
        while (*rest == '\\' || *rest == '/') rest++;
        if (!*rest) break;
        size_t k = 0;
        while (*rest && *rest != '\\' && *rest != '/' && k < sizeof comp - 1) comp[k++] = *rest++;
        comp[k] = 0;
        if (!strcmp(comp, ".")) continue;
        if (!strcmp(comp, "..")) { char *s = strrchr(host, '/'); if (s) *s = 0; continue; }
        append_ci(host, sizeof host, comp);
    }
    struct stat st;
    if (is_disc && stat(host, &st) != 0) {
        const char *base = strrchr(xp, '\\');
        const char *alt = base ? index_lookup(base + 1) : NULL;
        if (alt) { klog("fs: %s not at its disc path, using %s\n", xp, alt); return strdup(alt); }
    }
    return strdup(host);
}

/* The Xbox path with symlinks expanded, used as the "name" of a directory handle. */
char *fs_xbox_name(const char *name, OBJECT_ATTRIBUTES *oa)
{
    char xp[1024];
    full_xbox_path(xp, sizeof xp, name, oa);
    return strdup(xp);
}
