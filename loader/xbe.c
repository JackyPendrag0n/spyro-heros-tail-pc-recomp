/* Map an XBE image at its original virtual addresses. */
#define _GNU_SOURCE
#include "xbox.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define XOR_EP_RETAIL 0xA8FC57ABu
#define XOR_KT_RETAIL 0x5B6D40B6u

int xbe_load(const char *path, struct xbe_info *out)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return -1; }
    fseek(f, 0, SEEK_END);
    long fsz = ftell(f);
    fseek(f, 0, SEEK_SET);
    u8 *file = malloc(fsz);
    if (fread(file, 1, fsz, f) != (size_t)fsz) { perror("read xbe"); return -1; }
    fclose(f);
    if (memcmp(file, "XBEH", 4)) { fprintf(stderr, "not an XBE\n"); return -1; }

    u32 base       = *(u32 *)(file + 0x104);
    u32 hdr_size   = *(u32 *)(file + 0x108);
    u32 image_size = *(u32 *)(file + 0x10C);
    u32 nsec       = *(u32 *)(file + 0x11C);
    u32 sec_hdrs   = *(u32 *)(file + 0x120);

    /* Some section VAs extend past SizeOfImage once bss is counted; compute the real end. */
    u32 end = base + image_size;
    for (u32 i = 0; i < nsec; i++) {
        u8 *sh = file + (sec_hdrs - base) + i * 0x38;
        u32 va = *(u32 *)(sh + 4), vs = *(u32 *)(sh + 8);
        if (va + vs > end) end = va + vs;
    }
    end = (end + 0xFFFF) & ~0xFFFFu;

    void *m = mmap((void *)(uintptr_t)base, end - base, PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (m != (void *)(uintptr_t)base) {
        fprintf(stderr, "cannot map XBE at %#x (got %p). Check /proc/sys/vm/mmap_min_addr <= %u\n",
                base, m, base);
        return -1;
    }
    memcpy(m, file, hdr_size);
    for (u32 i = 0; i < nsec; i++) {
        u8 *sh = file + (sec_hdrs - base) + i * 0x38;
        u32 va = *(u32 *)(sh + 4), raw = *(u32 *)(sh + 0xC), rs = *(u32 *)(sh + 0x10);
        memcpy((void *)(uintptr_t)va, file + raw, rs);
        const char *name = (const char *)file + (*(u32 *)(sh + 0x14) - base);
        klog("section %-10s va=%08x vsize=%07x raw=%07x\n", name, va, *(u32 *)(sh + 8), rs);
    }

    out->base = base;
    out->size_image = end - base;
    out->entry = *(u32 *)(file + 0x128) ^ XOR_EP_RETAIL;
    out->kthunk = *(u32 *)(file + 0x158) ^ XOR_KT_RETAIL;
    out->tls_dir = *(u32 *)(file + 0x12C);
    out->stack_commit = *(u32 *)(file + 0x130);

    /* Patch the kernel thunk table: each entry holds 0x80000000|ordinal. */
    int nimp = 0, missing = 0;
    for (u32 *t = (u32 *)(uintptr_t)out->kthunk; *t; t++, nimp++) {
        u32 ord = *t & 0x7FFFFFFF;
        const struct kexport *e = kernel_exports;
        while (e->name && e->ordinal != (int)ord) e++;
        if (!e->name) { klog("kernel ordinal %u not provided!\n", ord); missing++; *t = 0xDEAD0000 | ord; continue; }
        *t = (u32)(uintptr_t)e->addr;
    }
    klog("XBE mapped %08x-%08x, entry %08x, %d kernel imports (%d missing)\n",
         base, end, out->entry, nimp, missing);
    free(file);
    return missing ? -1 : 0;
}
