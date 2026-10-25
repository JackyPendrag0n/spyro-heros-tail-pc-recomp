/* Xbox kernel environment for running an XBE natively on 32-bit Linux. */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
/* per-player Red state (redctx.c swaps the whole section when the game runs player 2) */
#define RS __attribute__((section("redst")))
typedef int32_t  s32;
typedef int16_t  s16;
typedef uint64_t u64;
typedef int64_t  s64;

#define STDCALL  __attribute__((stdcall))
#define FASTCALL __attribute__((fastcall))
#define CDECLX   __attribute__((cdecl))

/* NTSTATUS */
#define STATUS_SUCCESS               0x00000000u
#define STATUS_PENDING               0x00000103u
#define STATUS_TIMEOUT               0x00000102u
#define STATUS_ALERTED               0x00000101u
#define STATUS_NO_MORE_FILES         0x80000006u
#define STATUS_END_OF_FILE           0xC0000011u
#define STATUS_INVALID_HANDLE        0xC0000008u
#define STATUS_INVALID_PARAMETER     0xC000000Du
#define STATUS_NO_SUCH_FILE          0xC000000Fu
#define STATUS_ACCESS_DENIED         0xC0000022u
#define STATUS_OBJECT_NAME_NOT_FOUND 0xC0000034u
#define STATUS_OBJECT_NAME_COLLISION 0xC0000035u
#define STATUS_OBJECT_PATH_NOT_FOUND 0xC000003Au
#define STATUS_NO_MEMORY             0xC0000017u
#define STATUS_NOT_IMPLEMENTED       0xC0000002u
#define STATUS_INFO_LENGTH_MISMATCH  0xC0000004u
#define STATUS_BUFFER_TOO_SMALL      0xC0000023u
#define STATUS_NOT_A_DIRECTORY       0xC0000103u
#define STATUS_FILE_IS_A_DIRECTORY   0xC00000BAu
#define STATUS_OBJECT_TYPE_MISMATCH  0xC0000024u

/* ---- structures (layouts must match the Xbox exactly) ---- */
typedef struct { u16 Length, MaximumLength; char *Buffer; } ANSI_STRING, OBJECT_STRING;
typedef struct { u16 Length, MaximumLength; u16 *Buffer; } UNICODE_STRING;
typedef struct { void *RootDirectory; OBJECT_STRING *ObjectName; u32 Attributes; } OBJECT_ATTRIBUTES;
typedef struct { u32 Status; u32 Information; } IO_STATUS_BLOCK;
typedef struct LIST_ENTRY { struct LIST_ENTRY *Flink, *Blink; } LIST_ENTRY;

typedef struct {
    u8 Type, Absolute, Size, Inserted;
    s32 SignalState;
    LIST_ENTRY WaitListHead;
} DISPATCHER_HEADER;                                  /* 0x10 */

enum {
    EventNotificationObject = 0, EventSynchronizationObject = 1, MutantObject = 2,
    ProcessObject = 3, QueueObject = 4, SemaphoreObject = 5, ThreadObject = 6,
    TimerNotificationObject = 8, TimerSynchronizationObject = 9,
    DpcObject = 0x13,
};

typedef struct { DISPATCHER_HEADER Header; } KEVENT;
typedef struct { DISPATCHER_HEADER Header; s32 Limit; } KSEMAPHORE;
typedef struct { DISPATCHER_HEADER Header; LIST_ENTRY MutantListEntry; void *OwnerThread; u8 Abandoned; } KMUTANT;

typedef struct {
    u8 Type, Inserted, Padding[2];
    LIST_ENTRY DpcListEntry;
    void *DeferredRoutine;                  /* STDCALL void(KDPC*, ctx, a1, a2) */
    void *DeferredContext;
    void *SystemArgument1, *SystemArgument2;
} KDPC;                                               /* 0x1C */

typedef struct {
    DISPATCHER_HEADER Header;                         /* 0x00 */
    u64 DueTime;                                      /* 0x10 */
    LIST_ENTRY TimerListEntry;                        /* 0x18 */
    KDPC *Dpc;                                        /* 0x20 */
    s32 Period;                                       /* 0x24 */
} KTIMER;                                             /* 0x28 */

typedef struct {
    s32 LockCount;
    s32 RecursionCount;
    void *OwningThread;
    /* Xbox RTL_CRITICAL_SECTION is 0x1C: KEVENT Synchronization (0x10) + 3 longs. */
} RTL_CS_TAIL;

/* NT_TIB / KPCR / KPRCB / KTHREAD: only fields the game touches matter. */
typedef struct {
    void *ExceptionList;   /* 0x00 */
    void *StackBase;       /* 0x04 */
    void *StackLimit;      /* 0x08 */
    void *SubSystemTib;    /* 0x0C */
    void *FiberData;       /* 0x10 */
    void *ArbitraryUserPointer; /* 0x14 */
    void *Self;            /* 0x18 */
} NT_TIB;

typedef struct KPCR {
    NT_TIB NtTib;          /* 0x00 */
    struct KPCR *SelfPcr;  /* 0x1C */
    void *Prcb;            /* 0x20 -> &PrcbData */
    u8 Irql;               /* 0x24 */
    u8 pad[3];
    /* KPRCB at 0x28 */
    void *CurrentThread;   /* 0x28 */
    void *NextThread;      /* 0x2C */
    void *IdleThread;      /* 0x30 */
    u32 Unknown1[7];       /* 0x34 */
    LIST_ENTRY DpcListHead;/* 0x50 */
    u32 DpcRoutineActive;  /* 0x58 */
    u8 Unknown[0x224];     /* 0x5C .. 0x280 (prcb+0x250 read by XAPI: fiber ptr, keep 0) */
} KPCR;

/* ETHREAD as seen by the game.  KTHREAD fields at fixed offsets. */
#define KTHREAD_SIZE        0x200
#define KT_STACKBASE        0x1C
#define KT_STACKLIMIT       0x20
#define KT_TLSDATA          0x28
#define KT_PRIORITY         0x32
#define ET_UNIQUETHREAD     0x12C

/* file information */
typedef struct { s64 CreationTime, LastAccessTime, LastWriteTime, ChangeTime; u32 FileAttributes; } FILE_BASIC_INFORMATION;
typedef struct { s64 AllocationSize, EndOfFile; u32 NumberOfLinks; u8 DeletePending, Directory; } FILE_STANDARD_INFORMATION;
typedef struct { s64 CreationTime, LastAccessTime, LastWriteTime, ChangeTime, AllocationSize, EndOfFile; u32 FileAttributes; } FILE_NETWORK_OPEN_INFORMATION;

#define FILE_ATTRIBUTE_READONLY  0x01
#define FILE_ATTRIBUTE_DIRECTORY 0x10
#define FILE_ATTRIBUTE_NORMAL    0x80

/* ---- loader internals ---- */
struct kexport { int ordinal; void *addr; const char *name; };
extern const struct kexport kernel_exports[];

extern int g_trace;               /* XBOX_TRACE=1 logs every kernel call */
void kstub_log(int ordinal, const char *name, const char *fmt, ...);
void klog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#define TRACE(...) do { if (g_trace) klog(__VA_ARGS__); } while (0)

/* thread.c */
struct xthread;
KPCR *xthread_setup_current(void *ethread, void *stack_base, void *stack_limit);
void  xthread_init_gdt(void);
KPCR *xthread_pcr(void);
void *xthread_current_ethread(void);

/* sync.c */
void ksync_lock(void);
void ksync_unlock(void);
void ksync_broadcast(void);
u32  ksync_wait_object(void *obj, s64 *timeout100ns, int alertable);
s32  ksync_signal_event(KEVENT *ev);
void ksync_init(void);

/* handles.c */
enum htype { H_FREE = 0, H_FILE, H_DIR, H_THREAD, H_EVENT, H_SYMLINK, H_OTHER };
struct handle {
    enum htype type;
    int fd;                  /* H_FILE / H_DIR */
    char *host_path;
    char *xbox_path;
    void *object;            /* dispatcher object for waits (thread, event) */
    void *dir;               /* DIR* for H_DIR */
    int dir_restart;
};
u32  handle_alloc(enum htype t);
struct handle *handle_get(u32 h);
void handle_free(u32 h);

/* fs.c */
void fs_init(const char *game_root);
char *fs_resolve(const char *xbox_path, OBJECT_ATTRIBUTES *oa);

/* xbe.c */
struct xbe_info {
    u32 base, entry, kthunk, tls_dir, size_image;
    u32 stack_commit;
};
int xbe_load(const char *path, struct xbe_info *out);

/* hle.c */
void hle_install(const char *symbols_path);

/* misc */
/* Local number parser: avoids glibc 2.38's C23 strtol/strtoul symbol versions,
 * so the binary runs on any glibc >= 2.34. Accepts decimal or 0x-prefixed hex
 * (or bare hex when base == 16). */
static inline u32 parse_u32(const char *s, int base)
{
    u32 v = 0;
    if (!s) return 0;
    while (*s == ' ' || *s == '\t') s++;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { base = 16; s += 2; }
    for (;; s++) {
        int d;
        if (*s >= '0' && *s <= '9') d = *s - '0';
        else if (base == 16 && *s >= 'a' && *s <= 'f') d = *s - 'a' + 10;
        else if (base == 16 && *s >= 'A' && *s <= 'F') d = *s - 'A' + 10;
        else break;
        v = v * (u32)base + (u32)d;
    }
    return v;
}
extern u32 g_tls_data_size;
static inline u32 rd32(u32 a) { return *(volatile u32 *)(uintptr_t)a; }
static inline void wr32(u32 a, u32 v) { *(volatile u32 *)(uintptr_t)a = v; }
