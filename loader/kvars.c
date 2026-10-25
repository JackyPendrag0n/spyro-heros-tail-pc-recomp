/* Kernel-exported variables.  The thunk table gets the address of each. */
#include "xbox.h"

volatile u32 kv_KeTickCount[1];
char kv_LaunchDataPage[4];                    /* PLAUNCH_DATA_PAGE = NULL: cold boot */
u32 kv_HalDiskCachePartitionCount[1] = { 3 };
u32 kv_HalBootSMCVideoMode[1] = { 0 };
char kv_IoFileObjectType[0x40];
char kv_ExEventObjectType[0x40];
char kv_PsThreadObjectType[0x40];
char kv_IdexChannelObject[0x100];
u8 kv_XboxHDKey[16];
u8 kv_XboxSignatureKey[16] = { 0x53, 0x50, 0x59, 0x52, 0x4F, 0x2D, 0x50, 0x4F, 0x52, 0x54, 0x2D, 0x4B, 0x45, 0x59, 0x30, 0x31 };
u8 kv_XboxAlternateSignatureKeys[16 * 16];
/* XBOX_HARDWARE_INFO { ULONG Flags; UCHAR GpuRevision, McpRevision, unknown[2] } */
u32 kv_XboxHardwareInfo[2] = { 0x00000020, 0x0000D4D2 };
/* XBOX_KRNL_VERSION { USHORT Major, Minor, Build, Qfe } */
u16 kv_XboxKrnlVersion[4] = { 1, 0, 5838, 1 };
static char g_image_name[] = "\\Device\\CdRom0\\default.xbe";
OBJECT_STRING kv_XeImageFileName_s = { sizeof g_image_name - 1, sizeof g_image_name, g_image_name };
extern char kv_XeImageFileName[] __attribute__((alias("kv_XeImageFileName_s")));
