/* Xbox memory manager: virtual memory, contiguous memory, pool.
 *
 * All game allocations come from one reserved arena at fixed low addresses
 * (ARENA_BASE..ARENA_END), so addresses are deterministic and never collide
 * with the loader's own heap.  Pages are reserved PROT_NONE and committed
 * by mprotect. */
#define _GNU_SOURCE
#include "xbox.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define ARENA_BASE 0x10000000u
#define ARENA_END  0x70000000u
#define PAGE       0x1000u
#define NPAGES     ((ARENA_END - ARENA_BASE) / PAGE)

#define MEM_COMMIT     0x1000
#define MEM_RESERVE    0x2000
#define MEM_DECOMMIT   0x4000
#define MEM_RELEASE    0x8000
#define MEM_FREE       0x10000
#define MEM_PRIVATE    0x20000
#define MEM_TOP_DOWN   0x100000
#define PAGE_NOACCESS  0x01
#define PAGE_READWRITE 0x04

static u8 g_state[NPAGES];           /* 0 free, 1 reserved, 2 committed */
static u32 g_alloc_pages[NPAGES];    /* at allocation start: length in pages */
static pthread_mutex_t g_mlock = PTHREAD_MUTEX_INITIALIZER;
static u32 g_committed;

void kmem_init(void)
{
    void *m = mmap((void *)ARENA_BASE, ARENA_END - ARENA_BASE, PROT_NONE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
    if (m != (void *)ARENA_BASE) { fprintf(stderr, "cannot reserve arena at %#x\n", ARENA_BASE); exit(1); }
}

static inline u32 pidx(u32 a) { return (a - ARENA_BASE) / PAGE; }
static inline u32 paddr(u32 i) { return ARENA_BASE + i * PAGE; }

/* find npages free pages aligned to align (bytes); returns index or -1. lock held. */
static s32 find_free(u32 npages, u32 align, int top_down)
{
    u32 step = align / PAGE ? align / PAGE : 1;
    if (!top_down) {
        for (u32 i = 0; i + npages <= NPAGES; i += step) {
            u32 k = 0;
            while (k < npages && !g_state[i + k]) k++;
            if (k == npages) return (s32)i;
            i = ((i + k + 1 + step - 1) / step) * step - step;
        }
    } else {
        for (s32 i = (s32)((NPAGES - npages) / step * step); i >= 0; i -= (s32)step) {
            u32 k = 0;
            while (k < npages && !g_state[i + k]) k++;
            if (k == npages) return i;
        }
    }
    return -1;
}

static void commit(u32 i, u32 n)
{
    mprotect((void *)(uintptr_t)paddr(i), n * PAGE, PROT_READ | PROT_WRITE | PROT_EXEC);
    for (u32 k = 0; k < n; k++) {
        if (g_state[i + k] != 2) g_committed += PAGE;
        g_state[i + k] = 2;
    }
}

/* generic arena allocation: returns address or 0 */
u32 arena_alloc(u32 size, u32 align, int do_commit, int top_down)
{
    u32 n = (size + PAGE - 1) / PAGE;
    if (!n) n = 1;
    pthread_mutex_lock(&g_mlock);
    s32 i = find_free(n, align < PAGE ? PAGE : align, top_down);
    if (i < 0) { pthread_mutex_unlock(&g_mlock); return 0; }
    for (u32 k = 0; k < n; k++) g_state[i + k] = 1;
    g_alloc_pages[i] = n;
    if (do_commit) commit(i, n);
    pthread_mutex_unlock(&g_mlock);
    return paddr(i);
}

void arena_free(u32 addr)
{
    if (addr < ARENA_BASE || addr >= ARENA_END) return;
    pthread_mutex_lock(&g_mlock);
    u32 i = pidx(addr), n = g_alloc_pages[i];
    if (n) {
        mprotect((void *)(uintptr_t)addr, n * PAGE, PROT_NONE);
        madvise((void *)(uintptr_t)addr, n * PAGE, MADV_DONTNEED);
        for (u32 k = 0; k < n; k++) {
            if (g_state[i + k] == 2) g_committed -= PAGE;
            g_state[i + k] = 0;
        }
        g_alloc_pages[i] = 0;
    }
    pthread_mutex_unlock(&g_mlock);
}

u32 arena_size(u32 addr)
{
    if (addr < ARENA_BASE || addr >= ARENA_END) return 0;
    return g_alloc_pages[pidx(addr)] * PAGE;
}

/* ---- kernel exports ---- */

u32 STDCALL k_NtAllocateVirtualMemory(u32 *BaseAddress, u32 ZeroBits, u32 *RegionSize, u32 AllocationType, u32 Protect)
{
    (void)ZeroBits; (void)Protect;
    u32 base = *BaseAddress, size = *RegionSize, r = STATUS_SUCCESS;
    if (base == 0) {
        u32 a = arena_alloc(size, 0x10000, AllocationType & MEM_COMMIT, AllocationType & MEM_TOP_DOWN);
        if (!a) r = STATUS_NO_MEMORY;
        else { *BaseAddress = a; *RegionSize = (size + PAGE - 1) & ~(PAGE - 1); }
    } else if (base >= ARENA_BASE && base < ARENA_END) {
        u32 s = base & ~(PAGE - 1), e = (base + size + PAGE - 1) & ~(PAGE - 1);
        pthread_mutex_lock(&g_mlock);
        if (AllocationType & MEM_RESERVE) {
            for (u32 k = pidx(s); k < pidx(e); k++) if (g_state[k]) r = STATUS_OBJECT_NAME_COLLISION;
            if (!r) { for (u32 k = pidx(s); k < pidx(e); k++) g_state[k] = 1; g_alloc_pages[pidx(s)] = pidx(e) - pidx(s); }
        }
        if (!r && (AllocationType & MEM_COMMIT)) commit(pidx(s), pidx(e) - pidx(s));
        pthread_mutex_unlock(&g_mlock);
        *BaseAddress = s; *RegionSize = e - s;
    } else {
        r = STATUS_INVALID_PARAMETER;
    }
    TRACE("NtAllocateVirtualMemory(base=%08x size=%08x type=%x) -> %08x @%08x\n", base, size, AllocationType, r, *BaseAddress);
    return r;
}

u32 STDCALL k_NtFreeVirtualMemory(u32 *BaseAddress, u32 *RegionSize, u32 FreeType)
{
    u32 base = *BaseAddress;
    TRACE("NtFreeVirtualMemory(%08x, %08x, %x)\n", base, *RegionSize, FreeType);
    if (FreeType & MEM_RELEASE) {
        *RegionSize = arena_size(base);
        arena_free(base);
    } else if (FreeType & MEM_DECOMMIT) {
        u32 s = base & ~(PAGE - 1), e = (base + *RegionSize + PAGE - 1) & ~(PAGE - 1);
        madvise((void *)(uintptr_t)s, e - s, MADV_DONTNEED);
    }
    return STATUS_SUCCESS;
}

typedef struct { u32 BaseAddress, AllocationBase, AllocationProtect, RegionSize, State, Protect, Type; } MEMORY_BASIC_INFORMATION;

u32 STDCALL k_NtQueryVirtualMemory(u32 BaseAddress, MEMORY_BASIC_INFORMATION *mbi)
{
    memset(mbi, 0, sizeof *mbi);
    u32 a = BaseAddress & ~(PAGE - 1);
    mbi->BaseAddress = a;
    if (a < ARENA_BASE || a >= ARENA_END) {
        mbi->State = MEM_COMMIT; mbi->Protect = PAGE_READWRITE; mbi->RegionSize = PAGE;
        return STATUS_SUCCESS;
    }
    pthread_mutex_lock(&g_mlock);
    u32 i = pidx(a), st = g_state[i], k = i;
    while (k < NPAGES && g_state[k] == st) k++;
    mbi->RegionSize = (k - i) * PAGE;
    mbi->State = st == 0 ? MEM_FREE : st == 1 ? MEM_RESERVE : MEM_COMMIT;
    mbi->Protect = st == 2 ? PAGE_READWRITE : PAGE_NOACCESS;
    mbi->Type = st ? MEM_PRIVATE : 0;
    pthread_mutex_unlock(&g_mlock);
    return STATUS_SUCCESS;
}

u32 STDCALL k_MmAllocateContiguousMemoryEx(u32 NumberOfBytes, u32 Lowest, u32 Highest, u32 Alignment, u32 Protect)
{
    (void)Lowest; (void)Highest; (void)Protect;
    u32 a = arena_alloc(NumberOfBytes, Alignment > PAGE ? Alignment : PAGE, 1, 0);
    TRACE("MmAllocateContiguousMemoryEx(%x, align %x) -> %08x\n", NumberOfBytes, Alignment, a);
    return a;
}

u32 STDCALL k_MmAllocateContiguousMemory(u32 NumberOfBytes)
{
    return k_MmAllocateContiguousMemoryEx(NumberOfBytes, 0, 0xFFFFFFFF, 0, PAGE_READWRITE);
}

u32 STDCALL k_MmFreeContiguousMemory(u32 BaseAddress) { arena_free(BaseAddress); return 0; }
u32 STDCALL k_MmQueryAllocationSize(u32 BaseAddress) { return arena_size(BaseAddress); }
u32 STDCALL k_MmPersistContiguousMemory(u32 a, u32 b, u32 c) { (void)a; (void)b; (void)c; return 0; }
u32 STDCALL k_MmGetPhysicalAddress(u32 va) { return va & 0x7FFFFFFF; }
u32 STDCALL k_MmQueryAddressProtect(u32 va) { (void)va; return PAGE_READWRITE; }
u32 STDCALL k_MmSetAddressProtect(u32 a, u32 n, u32 p) { (void)a; (void)n; (void)p; return 0; }
u32 STDCALL k_MmLockUnlockBufferPages(u32 a, u32 n, u32 u) { (void)a; (void)n; (void)u; return 0; }
u32 STDCALL k_MmLockUnlockPhysicalPage(u32 a, u32 u) { (void)a; (void)u; return 0; }

u32 STDCALL k_MmClaimGpuInstanceMemory(u32 NumberOfBytes, u32 *NumberOfPaddingBytes)
{
    /* Only the D3D driver uses this; it is HLE'd.  Hand back the end of a block. */
    if (NumberOfPaddingBytes) *NumberOfPaddingBytes = 0;
    static u32 inst;
    if (!inst) inst = arena_alloc(0x10000 + NumberOfBytes, PAGE, 1, 1);
    return inst + 0x10000 + NumberOfBytes;
}

/* pool: small header holds the size */
u32 STDCALL k_ExAllocatePoolWithTag(u32 NumberOfBytes, u32 Tag)
{
    (void)Tag;
    u32 *p = malloc(NumberOfBytes + 16);
    if (!p) return 0;
    p[0] = NumberOfBytes;
    memset(p + 4, 0, NumberOfBytes);
    return (u32)(uintptr_t)(p + 4);
}
u32 STDCALL k_ExFreePool(u32 P) { if (P) free((u32 *)(uintptr_t)P - 4); return 0; }
u32 STDCALL k_ExQueryPoolBlockSize(u32 P) { return P ? ((u32 *)(uintptr_t)P - 4)[0] : 0; }

/* research tools: is this guest page committed (readable)? */
int kmem_committed(u32 a)
{
    if (a < ARENA_BASE || a >= ARENA_END) return 0;
    return g_state[pidx(a)] == 2;
}
