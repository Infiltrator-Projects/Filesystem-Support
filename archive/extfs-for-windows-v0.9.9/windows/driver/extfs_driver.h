// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef EXTFS_WINDOWS_DRIVER_H
#define EXTFS_WINDOWS_DRIVER_H

#include <ntifs.h>
#include <ntdddisk.h>
#include "extfs/extfs.h"
#include "infiltratr/compiler.h"

#define EXTFS_POOL_TAG 0x53465845U
#define EXTFS_VCB_SIGNATURE 0x42435645U /* EVCB */
#define EXTFS_FCB_SIGNATURE 0x42434645U /* EFCB */
#define EXTFS_FCB_NODE_TYPE ((CSHORT)0x2F01)
#define EXTFS_CCB_SIGNATURE 0x42434345U /* ECCB */
#define EXTFS_MAX_PATTERN_CHARS 260U

/* Adapter state passed to the portable core's read_at callback. */
typedef struct _EXTFS_DISK_READER {
    PDEVICE_OBJECT DeviceObject;
    ULONG SectorSize;
    PERESOURCE WriteResource;
} EXTFS_DISK_READER, *PEXTFS_DISK_READER;

/*
 * Per-mounted-volume state.  The I/O manager owns the device object containing
 * this extension; the VCB owns the shared per-inode FCB cache, while each FCB
 * references its containing VCB. OpenHandleCount tracks handles before CLEANUP;
 * FileObjectCount tracks FILE_OBJECT references until CLOSE.
 */
typedef struct _EXTFS_VCB {
    ULONG Signature;
    PDEVICE_OBJECT VolumeDeviceObject;
    PDEVICE_OBJECT TargetDeviceObject;
    PVPB Vpb;
    EXTFS_DISK_READER Reader;
    extfs_volume Volume;
    ERESOURCE FcbListResource;
    ERESOURCE WriteResource;
    ERESOURCE MetadataResource;
    LIST_ENTRY FcbList;
    volatile LONG OpenHandleCount;
    volatile LONG FileObjectCount;
    PFILE_OBJECT VolumeLockFileObject;
    BOOLEAN Dismounted;
    BOOLEAN WriteEnabled;
} EXTFS_VCB, *PEXTFS_VCB;

/* Shared per-inode state. Every FILE_OBJECT for the same inode points at the
 * same FCB so Windows share access and section-object state remain coherent.
 * FCBs are reclaimed after the final FILE_OBJECT closes. */
typedef struct _EXTFS_FCB {
    /* Must remain first: FILE_OBJECT.FsContext points at this header. */
    FSRTL_ADVANCED_FCB_HEADER Header;
    ULONG Signature;
    LIST_ENTRY Links;
    PEXTFS_VCB Vcb;
    extfs_inode Inode;
    SECTION_OBJECT_POINTERS SectionObjectPointers;
    ERESOURCE DataResource;
    ERESOURCE PagingIoResource;
    FAST_MUTEX HeaderMutex;
    SHARE_ACCESS ShareAccess;
    LONG HandleCount;
    LONG FileObjectCount;
    BOOLEAN VolumeOpen;
} EXTFS_FCB, *PEXTFS_FCB;

/* Per-handle state stored in FILE_OBJECT.FsContext2 and freed at CLOSE. */
typedef struct _EXTFS_CCB {
    ULONG Signature;
    ULONG DirectoryIndex;
    ACCESS_MASK GrantedAccess;
    ULONG CreateOptions;
    BOOLEAN CleanupComplete;
    USHORT PatternLength;
    WCHAR Pattern[EXTFS_MAX_PATTERN_CHARS];
} EXTFS_CCB, *PEXTFS_CCB;

DRIVER_INITIALIZE DriverEntry;
/* Role-type annotations let WDK Code Analysis reason about each IRP entry
 * point using the contract for that specific major function.  A single
 * common rejection routine serves the metadata-mutating IRPs that remain unsupported. */
_Dispatch_type_(IRP_MJ_CREATE)
DRIVER_DISPATCH ExtfsDispatchCreate;
_Dispatch_type_(IRP_MJ_CLOSE)
DRIVER_DISPATCH ExtfsDispatchClose;
_Dispatch_type_(IRP_MJ_CLEANUP)
DRIVER_DISPATCH ExtfsDispatchCleanup;
_Dispatch_type_(IRP_MJ_FLUSH_BUFFERS)
DRIVER_DISPATCH ExtfsDispatchFlushBuffers;
_Dispatch_type_(IRP_MJ_READ)
DRIVER_DISPATCH ExtfsDispatchRead;
_Dispatch_type_(IRP_MJ_WRITE)
DRIVER_DISPATCH ExtfsDispatchWrite;
_Dispatch_type_(IRP_MJ_QUERY_INFORMATION)
DRIVER_DISPATCH ExtfsDispatchQueryInformation;
_Dispatch_type_(IRP_MJ_SET_INFORMATION)
DRIVER_DISPATCH ExtfsDispatchSetInformation;
_Dispatch_type_(IRP_MJ_QUERY_VOLUME_INFORMATION)
DRIVER_DISPATCH ExtfsDispatchQueryVolumeInformation;
_Dispatch_type_(IRP_MJ_DIRECTORY_CONTROL)
DRIVER_DISPATCH ExtfsDispatchDirectoryControl;
_Dispatch_type_(IRP_MJ_FILE_SYSTEM_CONTROL)
DRIVER_DISPATCH ExtfsDispatchFileSystemControl;
_Dispatch_type_(IRP_MJ_DEVICE_CONTROL)
DRIVER_DISPATCH ExtfsDispatchDeviceControl;
_Dispatch_type_(IRP_MJ_SET_VOLUME_INFORMATION)
DRIVER_DISPATCH ExtfsDispatchReadOnly;

#endif
