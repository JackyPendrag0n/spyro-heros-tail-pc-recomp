/* Nt* file APIs on top of host files. */
#define _GNU_SOURCE
#include "xbox.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

char *fs_xbox_name(const char *name, OBJECT_ATTRIBUTES *oa);
void fs_add_link(const char *name, const char *target);
void fs_del_link(const char *name);
const char *fs_get_link(const char *name);

/* CreateDisposition */
#define FILE_SUPERSEDE    0
#define FILE_OPEN         1
#define FILE_CREATE       2
#define FILE_OPEN_IF      3
#define FILE_OVERWRITE    4
#define FILE_OVERWRITE_IF 5
/* CreateOptions */
#define FILE_DIRECTORY_FILE     0x01
#define FILE_NON_DIRECTORY_FILE 0x40
/* IO_STATUS_BLOCK.Information for creates */
#define FILE_SUPERSEDED  0
#define FILE_OPENED      1
#define FILE_CREATED     2
#define FILE_OVERWRITTEN 3

static char *ostr(OBJECT_STRING *s)
{
    char *r = malloc(s ? s->Length + 1 : 1);
    if (s) memcpy(r, s->Buffer, s->Length);
    r[s ? s->Length : 0] = 0;
    return r;
}

/* Unix epoch -> Xbox FILETIME (100ns since 1601) */
static s64 ft(time_t t) { return ((s64)t + 11644473600LL) * 10000000LL; }

/* "\Device\Harddisk0\PartitionN" with no trailing backslash is the raw volume
 * (used by XAPI to format cache partitions); with a backslash it is the file system. */
/* Movies play through xmv.c. With XBOX_MOVIES=0 (or if the XMV hooks didn't install)
 * .xmv files are hidden so the game skips its movies, exactly as it does when they are absent. */
int xmv_enabled(void);
static int hidden_movie(const char *name)
{
    size_t n = strlen(name);
    return !xmv_enabled() && n > 4 && !strcasecmp(name + n - 4, ".xmv");
}

static int raw_partition(const char *name)
{
    const char *p = "\\Device\\Harddisk0\\Partition";
    size_t n = strlen(p);
    if (strncasecmp(name, p, n) || name[n] < '1' || name[n] > '7' || name[n + 1]) return 0;
    return name[n] - '0';
}

static u32 do_open(u32 *FileHandle, OBJECT_ATTRIBUTES *oa, IO_STATUS_BLOCK *iosb,
                   u32 access, u32 disposition, u32 options, const char *who)
{
    char *name = ostr(oa->ObjectName);
    char *host = NULL;
    u32 r = STATUS_SUCCESS, info = FILE_OPENED;
    int rawp = (!oa->RootDirectory) ? raw_partition(name) : 0;
    if (rawp) {
        char *dir = fs_resolve(name, oa);                 /* .../PartitionN */
        host = malloc(strlen(dir) + 8);
        sprintf(host, "%s.raw", dir);
        free(dir);
        int fd = open(host, O_RDWR | O_CREAT, 0644);
        if (fd < 0) { r = STATUS_ACCESS_DENIED; goto out; }
        struct stat st;
        if (!fstat(fd, &st) && st.st_size < 0x2EE00000LL && ftruncate(fd, 0x2EE00000LL)) { /* sparse 750 MB */ }
        u32 h = handle_alloc(H_FILE);
        struct handle *p = handle_get(h);
        p->fd = fd; p->host_path = host; host = NULL;
        p->xbox_path = strdup(name);
        p->dir_restart = rawp;                            /* remember partition number */
        *FileHandle = h;
        goto out;
    }
    if (hidden_movie(name)) {
        static int said;
        if (!said++) klog("movies off: skipping .xmv files\n");
        r = STATUS_OBJECT_NAME_NOT_FOUND;
        goto out;
    }
    host = fs_resolve(name, oa);
    if (!host) { r = STATUS_OBJECT_PATH_NOT_FOUND; goto out; }

    struct stat st;
    int exists = stat(host, &st) == 0;
    int isdir = exists && S_ISDIR(st.st_mode);

    if (isdir || (options & FILE_DIRECTORY_FILE)) {
        if (!exists) {
            if (disposition == FILE_CREATE || disposition == FILE_OPEN_IF) {
                if (mkdir(host, 0755)) { r = STATUS_OBJECT_PATH_NOT_FOUND; goto out; }
                info = FILE_CREATED;
            } else { r = STATUS_OBJECT_NAME_NOT_FOUND; goto out; }
        } else if (!isdir) { r = STATUS_NOT_A_DIRECTORY; goto out; }
        else if (disposition == FILE_CREATE) { r = STATUS_OBJECT_NAME_COLLISION; goto out; }
        if (options & FILE_NON_DIRECTORY_FILE) { r = STATUS_FILE_IS_A_DIRECTORY; goto out; }
        u32 h = handle_alloc(H_DIR);
        struct handle *p = handle_get(h);
        p->host_path = host; host = NULL;
        p->xbox_path = fs_xbox_name(name, oa);
        *FileHandle = h;
        goto out;
    }

    int flags;
    int want_write = (access & 0x40000000) || (access & 0x2) || (access & 0x4) || (access & 0x10000000);
    switch (disposition) {
    case FILE_OPEN:         if (!exists) { r = STATUS_OBJECT_NAME_NOT_FOUND; goto out; } flags = 0; break;
    case FILE_CREATE:       if (exists) { r = STATUS_OBJECT_NAME_COLLISION; goto out; } flags = O_CREAT; info = FILE_CREATED; break;
    case FILE_OPEN_IF:      flags = O_CREAT; info = exists ? FILE_OPENED : FILE_CREATED; break;
    case FILE_OVERWRITE:    if (!exists) { r = STATUS_OBJECT_NAME_NOT_FOUND; goto out; } flags = O_TRUNC; info = FILE_OVERWRITTEN; break;
    case FILE_SUPERSEDE:
    case FILE_OVERWRITE_IF: flags = O_CREAT | O_TRUNC; info = exists ? FILE_OVERWRITTEN : FILE_CREATED; break;
    default: flags = 0;
    }
    if (flags & (O_CREAT | O_TRUNC)) want_write = 1;
    int fd = open(host, flags | (want_write ? O_RDWR : O_RDONLY), 0644);
    if (fd < 0 && want_write && errno == EACCES && !(flags & (O_CREAT | O_TRUNC)))
        fd = open(host, O_RDONLY);                    /* read-only media: fall back */
    if (fd < 0) {
        r = errno == ENOENT ? STATUS_OBJECT_PATH_NOT_FOUND : STATUS_ACCESS_DENIED;
        goto out;
    }
    u32 h = handle_alloc(H_FILE);
    struct handle *p = handle_get(h);
    p->fd = fd;
    p->host_path = host; host = NULL;
    p->xbox_path = fs_xbox_name(name, oa);
    *FileHandle = h;
out:
    if (iosb) { iosb->Status = r; iosb->Information = info; }
    klog("%s('%s') -> %08x%s%s\n", who, name, r, r ? "" : " handle ",
         r ? "" : (handle_get(*FileHandle) ? handle_get(*FileHandle)->host_path : "?"));
    free(name);
    free(host);
    return r;
}

u32 STDCALL k_NtCreateFile(u32 *FileHandle, u32 DesiredAccess, OBJECT_ATTRIBUTES *ObjectAttributes,
                           IO_STATUS_BLOCK *IoStatusBlock, s64 *AllocationSize, u32 FileAttributes,
                           u32 ShareAccess, u32 CreateDisposition, u32 CreateOptions)
{
    (void)AllocationSize; (void)FileAttributes; (void)ShareAccess;
    return do_open(FileHandle, ObjectAttributes, IoStatusBlock, DesiredAccess, CreateDisposition, CreateOptions, "NtCreateFile");
}

u32 STDCALL k_NtOpenFile(u32 *FileHandle, u32 DesiredAccess, OBJECT_ATTRIBUTES *ObjectAttributes,
                         IO_STATUS_BLOCK *IoStatusBlock, u32 ShareAccess, u32 OpenOptions)
{
    (void)ShareAccess;
    return do_open(FileHandle, ObjectAttributes, IoStatusBlock, DesiredAccess, FILE_OPEN, OpenOptions, "NtOpenFile");
}

/* remove everything inside dir (not dir itself) */
static void clear_dir(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char p[2300];
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        struct stat st;
        if (!lstat(p, &st) && S_ISDIR(st.st_mode)) { clear_dir(p); rmdir(p); }
        else unlink(p);
    }
    closedir(d);
}

static void complete_io(u32 Event, void *ApcRoutine, void *ApcContext, IO_STATUS_BLOCK *iosb)
{
    if (Event) {
        struct handle *e = handle_get(Event);
        if (e && e->object) ksync_signal_event(e->object);
    }
    if (ApcRoutine) {
        /* Xbox: void NTAPI ApcRoutine(PVOID ApcContext, PIO_STATUS_BLOCK, ULONG Reserved) */
        typedef void STDCALL (*apc_t)(void *, IO_STATUS_BLOCK *, u32);
        ((apc_t)ApcRoutine)(ApcContext, iosb, 0);
    }
}

u32 STDCALL k_NtReadFile(u32 FileHandle, u32 Event, void *ApcRoutine, void *ApcContext,
                         IO_STATUS_BLOCK *IoStatusBlock, void *Buffer, u32 Length, s64 *ByteOffset)
{
    struct handle *h = handle_get(FileHandle);
    if (!h || h->fd < 0) {
        klog("NtReadFile(%x %s): invalid handle\n", FileHandle, h && h->xbox_path ? h->xbox_path : "?");
        return STATUS_INVALID_HANDLE;
    }
    ssize_t n;
    if (ByteOffset) n = pread(h->fd, Buffer, Length, (off_t)*ByteOffset);
    else n = read(h->fd, Buffer, Length);
    u32 r = n < 0 ? STATUS_INVALID_PARAMETER : (n == 0 && Length ? STATUS_END_OF_FILE : STATUS_SUCCESS);
    if (ByteOffset && n > 0) lseek(h->fd, (off_t)*ByteOffset + n, SEEK_SET);
    IoStatusBlock->Status = r;
    IoStatusBlock->Information = n < 0 ? 0 : (u32)n;
    TRACE("NtReadFile(%x %s off=%lld len=%x) -> %x read %zd\n", FileHandle, h->xbox_path,
          ByteOffset ? (long long)*ByteOffset : -1LL, Length, r, n);
    complete_io(Event, ApcRoutine, ApcContext, IoStatusBlock);
    return r;
}

u32 STDCALL k_NtWriteFile(u32 FileHandle, u32 Event, void *ApcRoutine, void *ApcContext,
                          IO_STATUS_BLOCK *IoStatusBlock, void *Buffer, u32 Length, s64 *ByteOffset)
{
    struct handle *h = handle_get(FileHandle);
    if (!h || h->fd < 0) {
        klog("NtWriteFile(%x %s): invalid handle\n", FileHandle, h && h->xbox_path ? h->xbox_path : "?");
        return STATUS_INVALID_HANDLE;
    }
    if (h->dir_restart >= 3 && h->dir_restart <= 5 && ByteOffset && *ByteOffset == 0) {
        /* writing the volume header of a cache partition = format: empty its folder */
        char cmd[2200], *dir = strdup(h->host_path);
        dir[strlen(dir) - 4] = 0;                         /* strip ".raw" */
        klog("formatting cache partition %d (%s)\n", h->dir_restart, dir);
        snprintf(cmd, sizeof cmd, "%s", dir);
        clear_dir(cmd);
        free(dir);
    }
    ssize_t n = ByteOffset ? pwrite(h->fd, Buffer, Length, (off_t)*ByteOffset) : write(h->fd, Buffer, Length);
    if (ByteOffset && n > 0) lseek(h->fd, (off_t)*ByteOffset + n, SEEK_SET);
    u32 r = n < 0 ? STATUS_ACCESS_DENIED : STATUS_SUCCESS;
    IoStatusBlock->Status = r;
    IoStatusBlock->Information = n < 0 ? 0 : (u32)n;
    TRACE("NtWriteFile(%s len=%x) -> %zd\n", h->xbox_path, Length, n);
    complete_io(Event, ApcRoutine, ApcContext, IoStatusBlock);
    return r;
}

u32 STDCALL k_NtClose(u32 Handle)
{
    struct handle *h = handle_get(Handle);
    if (!h) return STATUS_INVALID_HANDLE;
    TRACE("NtClose(%x %s)\n", Handle, h->xbox_path ? h->xbox_path : "");
    if (h->fd >= 0) close(h->fd);
    if (h->dir) closedir(h->dir);
    if (h->type == H_DIR) free(h->object);           /* directory search mask */
    /* thread/event objects stay alive: the game owns their memory */
    handle_free(Handle);
    return STATUS_SUCCESS;
}

u32 STDCALL k_NtFlushBuffersFile(u32 FileHandle, IO_STATUS_BLOCK *iosb)
{
    struct handle *h = handle_get(FileHandle);
    if (h && h->fd >= 0) fsync(h->fd);
    if (iosb) { iosb->Status = 0; iosb->Information = 0; }
    return STATUS_SUCCESS;
}

static void fill_times(s64 *t4, struct stat *st)
{
    t4[0] = ft(st->st_ctime); t4[1] = ft(st->st_atime); t4[2] = ft(st->st_mtime); t4[3] = ft(st->st_mtime);
}

u32 STDCALL k_NtQueryInformationFile(u32 FileHandle, IO_STATUS_BLOCK *iosb, void *Info, u32 Length, u32 Class)
{
    struct handle *h = handle_get(FileHandle);
    if (!h) return STATUS_INVALID_HANDLE;
    struct stat st;
    if (h->fd >= 0) fstat(h->fd, &st); else stat(h->host_path, &st);
    u32 attr = S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
    u32 r = STATUS_SUCCESS, info = 0;
    memset(Info, 0, Length);
    switch (Class) {
    case 4: { /* FileBasicInformation */
        FILE_BASIC_INFORMATION *b = Info; fill_times(&b->CreationTime, &st); b->FileAttributes = attr; info = 40; break; }
    case 5: { /* FileStandardInformation */
        FILE_STANDARD_INFORMATION *s = Info;
        s->AllocationSize = (st.st_size + 0x3FFF) & ~0x3FFFLL; s->EndOfFile = st.st_size;
        s->NumberOfLinks = 1; s->Directory = S_ISDIR(st.st_mode); info = 24; break; }
    case 14: /* FilePositionInformation */
        *(s64 *)Info = h->fd >= 0 ? lseek(h->fd, 0, SEEK_CUR) : 0; info = 8; break;
    case 34: { /* FileNetworkOpenInformation */
        FILE_NETWORK_OPEN_INFORMATION *n = Info; fill_times(&n->CreationTime, &st);
        n->AllocationSize = (st.st_size + 0x3FFF) & ~0x3FFFLL; n->EndOfFile = st.st_size;
        n->FileAttributes = attr; info = 56; break; }
    case 6: /* FileInternalInformation */
        *(s64 *)Info = st.st_ino; info = 8; break;
    default:
        klog("NtQueryInformationFile: class %u not implemented\n", Class);
        r = STATUS_INVALID_PARAMETER;
    }
    if (iosb) { iosb->Status = r; iosb->Information = info; }
    TRACE("NtQueryInformationFile(%s, class %u) -> %x\n", h->xbox_path, Class, r);
    return r;
}

u32 STDCALL k_NtSetInformationFile(u32 FileHandle, IO_STATUS_BLOCK *iosb, void *Info, u32 Length, u32 Class)
{
    (void)Length;
    struct handle *h = handle_get(FileHandle);
    if (!h) return STATUS_INVALID_HANDLE;
    u32 r = STATUS_SUCCESS;
    switch (Class) {
    case 14: lseek(h->fd, (off_t)*(s64 *)Info, SEEK_SET); break;           /* position */
    case 19: case 20: if (ftruncate(h->fd, (off_t)*(s64 *)Info)) r = STATUS_ACCESS_DENIED; break; /* allocation / EOF */
    case 4: break;                                                         /* basic: ignore times/attrs */
    case 13: /* FileDispositionInformation: delete on close */
        if (*(u8 *)Info) { if (h->dir || h->type == H_DIR) rmdir(h->host_path); else unlink(h->host_path); }
        break;
    case 10: { /* FileRenameInformation {ReplaceIfExists, RootDirectory, OBJECT_STRING FileName} */
        struct { u8 Replace; u8 pad[3]; void *Root; OBJECT_STRING Name; } *ri = Info;
        OBJECT_ATTRIBUTES oa = { ri->Root, &ri->Name, 0 };
        char *n = ostr(&ri->Name);
        char *dst = fs_resolve(n, &oa);
        if (!dst || rename(h->host_path, dst)) r = STATUS_ACCESS_DENIED;
        else { free(h->host_path); h->host_path = dst; dst = NULL; }
        free(n); free(dst);
        break; }
    default:
        klog("NtSetInformationFile: class %u not implemented\n", Class);
        r = STATUS_INVALID_PARAMETER;
    }
    if (iosb) { iosb->Status = r; iosb->Information = 0; }
    TRACE("NtSetInformationFile(%s, class %u) -> %x\n", h->xbox_path, Class, r);
    return r;
}

u32 STDCALL k_NtQueryFullAttributesFile(OBJECT_ATTRIBUTES *oa, FILE_NETWORK_OPEN_INFORMATION *Info)
{
    char *name = ostr(oa->ObjectName);
    char *host = fs_resolve(name, oa);
    struct stat st;
    u32 r = STATUS_SUCCESS;
    if (!host || stat(host, &st) || hidden_movie(name)) r = STATUS_OBJECT_NAME_NOT_FOUND;
    else {
        memset(Info, 0, sizeof *Info);
        fill_times(&Info->CreationTime, &st);
        Info->AllocationSize = Info->EndOfFile = st.st_size;
        Info->FileAttributes = S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
    }
    klog("NtQueryFullAttributesFile('%s') -> %08x\n", name, r);
    free(name); free(host);
    return r;
}

/* FILE_DIRECTORY_INFORMATION (Xbox, ANSI name) */
typedef struct {
    u32 NextEntryOffset, FileIndex;
    s64 CreationTime, LastAccessTime, LastWriteTime, ChangeTime, EndOfFile, AllocationSize;
    u32 FileAttributes, FileNameLength;
    char FileName[1];
} FILE_DIRECTORY_INFORMATION;

u32 STDCALL k_NtQueryDirectoryFile(u32 FileHandle, u32 Event, void *ApcRoutine, void *ApcContext,
                                   IO_STATUS_BLOCK *iosb, void *FileInformation, u32 Length,
                                   u32 Class, OBJECT_STRING *FileMask, u32 RestartScan)
{
    struct handle *h = handle_get(FileHandle);
    if (!h || h->type != H_DIR) return STATUS_INVALID_HANDLE;
    /* The mask is fixed by the first query on a handle (or a restart); for
     * directory handles h->object holds the malloc'd mask string. */
    if (!h->dir || RestartScan) {
        if (h->dir) closedir(h->dir);
        h->dir = opendir(h->host_path);
        if (FileMask && FileMask->Length) { free(h->object); h->object = ostr(FileMask); }
        else if (!h->object) h->object = strdup("*");
    }
    const char *mask = h->object ? h->object : "*";
    if (Class != 1) klog("NtQueryDirectoryFile: class %u (only 1 supported)\n", Class);
    struct dirent *e;
    u32 r = STATUS_NO_MORE_FILES;
    while (h->dir && (e = readdir(h->dir))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (fnmatch(mask, e->d_name, FNM_CASEFOLD) != 0) continue;
        if (hidden_movie(e->d_name)) continue;
        char p[2048];
        snprintf(p, sizeof p, "%s/%s", h->host_path, e->d_name);
        struct stat st;
        if (stat(p, &st)) continue;
        u32 nl = strlen(e->d_name);
        if (Length < offsetof(FILE_DIRECTORY_INFORMATION, FileName) + nl) { r = STATUS_BUFFER_TOO_SMALL; break; }
        FILE_DIRECTORY_INFORMATION *fi = FileInformation;
        memset(fi, 0, offsetof(FILE_DIRECTORY_INFORMATION, FileName));
        fill_times(&fi->CreationTime, &st);
        fi->EndOfFile = st.st_size;
        fi->AllocationSize = (st.st_size + 0x3FFF) & ~0x3FFFLL;
        fi->FileAttributes = S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
        fi->FileNameLength = nl;
        memcpy(fi->FileName, e->d_name, nl);
        r = STATUS_SUCCESS;
        TRACE("NtQueryDirectoryFile(%s mask %s) -> %s\n", h->xbox_path, mask, e->d_name);
        break;
    }
    if (r == STATUS_NO_MORE_FILES) TRACE("NtQueryDirectoryFile(%s mask %s) -> no more\n", h->xbox_path, mask);
    iosb->Status = r;
    iosb->Information = r ? 0 : Length;
    complete_io(Event, ApcRoutine, ApcContext, iosb);
    return r;
}

u32 STDCALL k_NtQueryVolumeInformationFile(u32 FileHandle, IO_STATUS_BLOCK *iosb, void *Info, u32 Length, u32 Class)
{
    (void)FileHandle;
    memset(Info, 0, Length);
    u32 r = STATUS_SUCCESS, info = 0;
    if (Class == 3 || Class == 7) {       /* FileFsSizeInformation / FullSize */
        s64 *v = Info;
        /* 16 KB clusters. Keep byte counts below 4 GB: titles often keep free space in 32 bits. */
        v[0] = 0x3E000; v[1] = 0x10000;     /* total ~3.9 GB / available 1 GB */
        if (Class == 7) { v[2] = 0x10000; ((u32 *)Info)[6] = 32; ((u32 *)Info)[7] = 512; info = 32; }
        else { ((u32 *)Info)[4] = 32; ((u32 *)Info)[5] = 512; info = 24; }
    } else if (Class == 1) {             /* FileFsVolumeInformation */
        info = 18;
    } else if (Class == 4) {             /* FileFsDeviceInformation */
        ((u32 *)Info)[0] = 2; info = 8;   /* FILE_DEVICE_CD_ROM? keep simple */
    } else {
        klog("NtQueryVolumeInformationFile: class %u\n", Class);
    }
    if (iosb) { iosb->Status = r; iosb->Information = info; }
    TRACE("NtQueryVolumeInformationFile(class %u)\n", Class);
    return r;
}

u32 STDCALL k_NtDeviceIoControlFile(u32 h, u32 ev, void *apc, void *ctx, IO_STATUS_BLOCK *iosb,
                                    u32 code, void *in, u32 inlen, void *out, u32 outlen)
{
    (void)ev; (void)apc; (void)ctx; (void)in; (void)inlen;
    struct handle *hd = handle_get(h);
    u32 info = 0;
    if (out) memset(out, 0, outlen);
    switch (code) {
    case 0x00070000: {   /* IOCTL_DISK_GET_DRIVE_GEOMETRY -> DISK_GEOMETRY */
        u32 *g = out;
        *(s64 *)g = 0x2EE00000LL / (512 * 63 * 255) + 1;  /* Cylinders */
        g[2] = 12;                                        /* FixedMedia */
        g[3] = 255; g[4] = 63; g[5] = 512;                /* tracks/cyl, sectors/track, bytes/sector */
        info = 24; break; }
    case 0x00074004: {   /* IOCTL_DISK_GET_PARTITION_INFO -> PARTITION_INFORMATION */
        s64 *p = out;
        int part = 0;
        if (hd && hd->xbox_path) { const char *s = strcasestr(hd->xbox_path, "Partition"); if (s) part = s[9] - '0'; }
        p[0] = 0x80000LL + (s64)part * 0x2EE00000LL;      /* StartingOffset (nominal) */
        p[1] = 0x2EE00000LL;                              /* PartitionLength: 750 MB */
        ((u32 *)out)[5] = part;                           /* PartitionNumber */
        ((u8 *)out)[24] = 0xE0;                           /* PartitionType: FATX-ish */
        ((u8 *)out)[26] = 1;                              /* RecognizedPartition */
        info = 32; break; }
    default:
        klog("NtDeviceIoControlFile(code %08x) -> success (stub)\n", code);
    }
    TRACE("NtDeviceIoControlFile(%s, code %08x)\n", hd && hd->xbox_path ? hd->xbox_path : "?", code);
    if (iosb) { iosb->Status = 0; iosb->Information = info; }
    return STATUS_SUCCESS;
}

u32 STDCALL k_NtFsControlFile(u32 h, u32 ev, void *apc, void *ctx, IO_STATUS_BLOCK *iosb,
                              u32 code, void *in, u32 inlen, void *out, u32 outlen)
{
    return k_NtDeviceIoControlFile(h, ev, apc, ctx, iosb, code, in, inlen, out, outlen);
}

/* ---- symbolic links ---- */
u32 STDCALL k_IoCreateSymbolicLink(OBJECT_STRING *Link, OBJECT_STRING *Device)
{
    char *l = ostr(Link), *d = ostr(Device);
    fs_add_link(l, d);
    free(l); free(d);
    return STATUS_SUCCESS;
}

u32 STDCALL k_IoDeleteSymbolicLink(OBJECT_STRING *Link)
{
    char *l = ostr(Link);
    fs_del_link(l);
    klog("IoDeleteSymbolicLink(%s)\n", l);
    free(l);
    return STATUS_SUCCESS;
}

u32 STDCALL k_NtOpenSymbolicLinkObject(u32 *LinkHandle, OBJECT_ATTRIBUTES *oa)
{
    char *n = ostr(oa->ObjectName);
    const char *t = fs_get_link(n);
    klog("NtOpenSymbolicLinkObject(%s) -> %s\n", n, t ? t : "(none)");
    if (!t) { free(n); return STATUS_OBJECT_NAME_NOT_FOUND; }
    u32 h = handle_alloc(H_SYMLINK);
    handle_get(h)->xbox_path = strdup(t);
    *LinkHandle = h;
    free(n);
    return STATUS_SUCCESS;
}

u32 STDCALL k_NtQuerySymbolicLinkObject(u32 LinkHandle, OBJECT_STRING *Target, u32 *ReturnedLength)
{
    struct handle *h = handle_get(LinkHandle);
    if (!h || h->type != H_SYMLINK) return STATUS_INVALID_HANDLE;
    u32 n = strlen(h->xbox_path);
    if (ReturnedLength) *ReturnedLength = n;
    if (n > Target->MaximumLength) return STATUS_BUFFER_TOO_SMALL;
    memcpy(Target->Buffer, h->xbox_path, n);
    Target->Length = n;
    return STATUS_SUCCESS;
}
