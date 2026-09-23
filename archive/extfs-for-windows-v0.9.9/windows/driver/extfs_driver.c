// SPDX-License-Identifier: GPL-3.0-or-later
#include "extfs_driver.h"

/*
 * ExtFS 0.9.3 is a deliberately conservative synchronous native IFS.  All ext
 * on-disk decisions remain in the portable core; this file translates Windows
 * IRPs, names, synchronization and information records.  Supported writes are
 * bounded to existing-file data overwrite plus the filesystem-specific ext2,
 * ext3 and ext4 resize paths.  Namespace mutation and paging writes remain
 * fail-closed until their later qualification checkpoints.
 */

typedef struct _EXTFS_OUTPUT_BUFFER {
    PVOID Address;
    PMDL Mdl;
    BOOLEAN Unlock;
} EXTFS_OUTPUT_BUFFER, *PEXTFS_OUTPUT_BUFFER;

typedef struct _EXTFS_FILE_NAME_INFORMATION {
    ULONG FileNameLength;
    WCHAR FileName[1];
} EXTFS_FILE_NAME_INFORMATION, *PEXTFS_FILE_NAME_INFORMATION;

typedef struct _EXTFS_FS_VOLUME_INFORMATION {
    LARGE_INTEGER VolumeCreationTime;
    ULONG VolumeSerialNumber;
    ULONG VolumeLabelLength;
    BOOLEAN SupportsObjects;
    WCHAR VolumeLabel[1];
} EXTFS_FS_VOLUME_INFORMATION, *PEXTFS_FS_VOLUME_INFORMATION;

typedef struct _EXTFS_FS_SIZE_INFORMATION {
    LARGE_INTEGER TotalAllocationUnits;
    LARGE_INTEGER AvailableAllocationUnits;
    ULONG SectorsPerAllocationUnit;
    ULONG BytesPerSector;
} EXTFS_FS_SIZE_INFORMATION, *PEXTFS_FS_SIZE_INFORMATION;

typedef struct _EXTFS_FS_FULL_SIZE_INFORMATION {
    LARGE_INTEGER TotalAllocationUnits;
    LARGE_INTEGER CallerAvailableAllocationUnits;
    LARGE_INTEGER ActualAvailableAllocationUnits;
    ULONG SectorsPerAllocationUnit;
    ULONG BytesPerSector;
} EXTFS_FS_FULL_SIZE_INFORMATION, *PEXTFS_FS_FULL_SIZE_INFORMATION;

typedef struct _EXTFS_FS_ATTRIBUTE_INFORMATION {
    ULONG FileSystemAttributes;
    LONG MaximumComponentNameLength;
    ULONG FileSystemNameLength;
    WCHAR FileSystemName[1];
} EXTFS_FS_ATTRIBUTE_INFORMATION, *PEXTFS_FS_ATTRIBUTE_INFORMATION;

typedef struct _EXTFS_DIRENT_PICK {
    ULONG StartIndex;
    ULONG CurrentIndex;
    ULONG FoundIndex;
    ULONG InodeNumber;
    extfs_node_type Type;
    UCHAR Utf8Length;
    CHAR Utf8Name[EXTFS_MAX_NAME_LENGTH + 1U];
    const WCHAR *Pattern;
    USHORT PatternLength;
    BOOLEAN CaseSensitive;
    BOOLEAN Found;
    NTSTATUS NameStatus;
} EXTFS_DIRENT_PICK, *PEXTFS_DIRENT_PICK;

static PDEVICE_OBJECT ExtfsControlDevice;
/* Keep one absolute image pointer so PE/COFF emits a real base-relocation
 * record.  Kernel drivers must remain loadable away from their preferred
 * image base. */
static PDRIVER_INITIALIZE const ExtfsRelocationAnchor = DriverEntry;

static VOID ExtfsCopyMemory(PVOID Destination, const VOID *Source, ULONG Count)
{
    UCHAR *destination = (UCHAR *)Destination;
    const UCHAR *source = (const UCHAR *)Source;
    while (Count != 0U) {
        *destination++ = *source++;
        --Count;
    }
}

static VOID ExtfsZeroMemory(PVOID Destination, ULONG Count)
{
    UCHAR *destination = (UCHAR *)Destination;
    while (Count != 0U) {
        *destination++ = 0U;
        --Count;
    }
}

static BOOLEAN ExtfsBytesEqual(const UCHAR *Left, const UCHAR *Right, ULONG Count)
{
    while (Count != 0U) {
        if (*Left++ != *Right++) return FALSE;
        --Count;
    }
    return TRUE;
}

static ULONG ExtfsStringLength(const CHAR *String)
{
    ULONG length = 0U;
    while (String[length] != '\0') ++length;
    return length;
}

static PVOID ExtfsAllocate(ULONG Size)
{
#if defined(_MSC_VER)
    return ExAllocatePool2(POOL_FLAG_NON_PAGED, Size, EXTFS_POOL_TAG);
#else
    return ExAllocatePoolWithTag(NonPagedPoolNx, Size, EXTFS_POOL_TAG);
#endif
}

static NTSTATUS ExtfsCompleteIrp(PIRP Irp, NTSTATUS Status,
                                 ULONG_PTR Information)
{
    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = Information;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Status;
}

static NTSTATUS ExtfsStatusToNt(extfs_status Status)
{
    switch (Status) {
        case EXTFS_OK: return STATUS_SUCCESS;
        case EXTFS_ERR_NOT_EXT: return STATUS_UNRECOGNIZED_VOLUME;
        case EXTFS_ERR_NOT_FOUND: return STATUS_OBJECT_NAME_NOT_FOUND;
        case EXTFS_ERR_NOT_DIRECTORY: return STATUS_NOT_A_DIRECTORY;
        case EXTFS_ERR_IS_DIRECTORY: return STATUS_FILE_IS_A_DIRECTORY;
        case EXTFS_ERR_BUFFER_TOO_SMALL: return STATUS_BUFFER_TOO_SMALL;
        case EXTFS_ERR_UNSUPPORTED: return STATUS_NOT_SUPPORTED;
        case EXTFS_ERR_RANGE: return STATUS_INVALID_PARAMETER;
        case EXTFS_ERR_IO: return STATUS_IO_DEVICE_ERROR;
        case EXTFS_ERR_NO_SPACE: return STATUS_DISK_FULL;
        case EXTFS_ERR_CHECKSUM:
        case EXTFS_ERR_CORRUPT: return STATUS_FILE_CORRUPT_ERROR;
        default: return STATUS_INVALID_PARAMETER;
    }
}

static PEXTFS_VCB ExtfsVcbFromDevice(PDEVICE_OBJECT DeviceObject)
{
    PEXTFS_VCB vcb;
    if (DeviceObject == NULL || DeviceObject == ExtfsControlDevice ||
        DeviceObject->DeviceExtension == NULL) {
        return NULL;
    }
    vcb = (PEXTFS_VCB)DeviceObject->DeviceExtension;
    return vcb->Signature == EXTFS_VCB_SIGNATURE ? vcb : NULL;
}

static PEXTFS_FCB ExtfsFcbFromFile(PFILE_OBJECT FileObject)
{
    PEXTFS_FCB fcb;
    if (FileObject == NULL || FileObject->FsContext == NULL) {
        return NULL;
    }
    fcb = CONTAINING_RECORD(
        (PFSRTL_ADVANCED_FCB_HEADER)FileObject->FsContext,
        EXTFS_FCB, Header);
    return fcb->Signature == EXTFS_FCB_SIGNATURE ? fcb : NULL;
}

static PEXTFS_CCB ExtfsCcbFromFile(PFILE_OBJECT FileObject)
{
    PEXTFS_CCB ccb;
    if (FileObject == NULL || FileObject->FsContext2 == NULL) {
        return NULL;
    }
    ccb = (PEXTFS_CCB)FileObject->FsContext2;
    return ccb->Signature == EXTFS_CCB_SIGNATURE ? ccb : NULL;
}

/* FCBs are shared by inode while FILE_OBJECT references remain. The VCB
 * resource serialises cache membership, lifetime and Windows SHARE_ACCESS updates. */
static PEXTFS_FCB ExtfsFindFcbLocked(PEXTFS_VCB Vcb,
                                     const extfs_inode *Inode,
                                     BOOLEAN VolumeOpen)
{
    PLIST_ENTRY entry;
    for (entry = Vcb->FcbList.Flink; entry != &Vcb->FcbList;
         entry = entry->Flink) {
        PEXTFS_FCB fcb = CONTAINING_RECORD(entry, EXTFS_FCB, Links);
        if (fcb->VolumeOpen == VolumeOpen &&
            (VolumeOpen || fcb->Inode.number == Inode->number)) {
            return fcb;
        }
    }
    return NULL;
}

/*
 * Share-access helpers must run at PASSIVE_LEVEL.  FAST_MUTEX is unsuitable
 * here because ExAcquireFastMutex raises IRQL to APC_LEVEL, while the I/O
 * manager share-access routines are explicitly PASSIVE_LEVEL DDIs.  ERESOURCE
 * serialises the FCB/share state without raising IRQL; FsRtlEnterFileSystem
 * supplies the normal file-system critical region around resource acquisition.
 */
static VOID ExtfsAcquireFcbList(PEXTFS_VCB Vcb)
{
    FsRtlEnterFileSystem();
    (void)ExAcquireResourceExclusiveLite(&Vcb->FcbListResource, TRUE);
}

static VOID ExtfsReleaseFcbList(PEXTFS_VCB Vcb)
{
    ExReleaseResourceLite(&Vcb->FcbListResource);
    FsRtlExitFileSystem();
}

static VOID ExtfsAcquireFileData(PEXTFS_FCB Fcb, BOOLEAN Exclusive)
{
    FsRtlEnterFileSystem();
    if (Exclusive) {
        (void)ExAcquireResourceExclusiveLite(&Fcb->DataResource, TRUE);
    } else {
        (void)ExAcquireResourceSharedLite(&Fcb->DataResource, TRUE);
    }
}

static VOID ExtfsReleaseFileData(PEXTFS_FCB Fcb)
{
    ExReleaseResourceLite(&Fcb->DataResource);
    FsRtlExitFileSystem();
}

/* Allocation bitmaps and free-block counters are volume-global metadata.
 * Serialize metadata transactions independently of per-file data locking. */
static VOID ExtfsAcquireMetadata(PEXTFS_VCB Vcb)
{
    FsRtlEnterFileSystem();
    (void)ExAcquireResourceExclusiveLite(&Vcb->MetadataResource, TRUE);
}

static VOID ExtfsReleaseMetadata(PEXTFS_VCB Vcb)
{
    ExReleaseResourceLite(&Vcb->MetadataResource);
    FsRtlExitFileSystem();
}

static VOID ExtfsSyncFcbHeaderSizes(PEXTFS_FCB Fcb)
{
    ULONGLONG size;
    ULONGLONG blockSize;
    ULONGLONG allocation;

    size = Fcb->VolumeOpen ? 0ULL : Fcb->Inode.size;
    if (size > 0x7FFFFFFFFFFFFFFFULL)
        size = 0x7FFFFFFFFFFFFFFFULL;
    blockSize = Fcb->Vcb->Volume.block_size;
    if (blockSize == 0ULL ||
        size > 0x7FFFFFFFFFFFFFFFULL - (blockSize - 1ULL)) {
        allocation = size;
    } else {
        allocation = (size + blockSize - 1ULL) & ~(blockSize - 1ULL);
    }

    ExAcquireFastMutex(&Fcb->HeaderMutex);
    Fcb->Header.FileSize.QuadPart = (LONGLONG)size;
    Fcb->Header.ValidDataLength.QuadPart = (LONGLONG)size;
    Fcb->Header.AllocationSize.QuadPart = (LONGLONG)allocation;
    ExReleaseFastMutex(&Fcb->HeaderMutex);
}

static VOID ExtfsDestroyFcb(PEXTFS_FCB Fcb)
{
    FsRtlTeardownPerStreamContexts(&Fcb->Header);
    ExDeleteResourceLite(&Fcb->PagingIoResource);
    ExDeleteResourceLite(&Fcb->DataResource);
    ExFreePool(Fcb);
}

static VOID ExtfsDestroyFcbList(PLIST_ENTRY List)
{
    while (!IsListEmpty(List)) {
        PLIST_ENTRY entry = RemoveHeadList(List);
        PEXTFS_FCB fcb = CONTAINING_RECORD(entry, EXTFS_FCB, Links);
        InitializeListHead(&fcb->Links);
        ExtfsDestroyFcb(fcb);
    }
}

/*
 * Only Memory Manager can determine whether mapped data/image sections have
 * drained. SharedCacheMap must also be absent because this driver deliberately
 * does not initialise Cache Manager maps.
 */
static VOID ExtfsCollectReapableFcbsLocked(PEXTFS_VCB Vcb,
                                           PLIST_ENTRY ReapList,
                                           BOOLEAN DelaySectionClose)
{
    PLIST_ENTRY entry = Vcb->FcbList.Flink;

    while (entry != &Vcb->FcbList) {
        PLIST_ENTRY next = entry->Flink;
        PEXTFS_FCB fcb = CONTAINING_RECORD(entry, EXTFS_FCB, Links);

        if (fcb->FileObjectCount == 0 && fcb->HandleCount == 0 &&
            fcb->SectionObjectPointers.SharedCacheMap == NULL &&
            MmForceSectionClosed(&fcb->SectionObjectPointers,
                                 DelaySectionClose)) {
            RemoveEntryList(entry);
            InsertTailList(ReapList, entry);
            fcb->Signature = 0U;
        }
        entry = next;
    }
}

static NTSTATUS ExtfsAcquireFcb(PEXTFS_VCB Vcb,
                                const extfs_inode *Inode,
                                BOOLEAN VolumeOpen,
                                ACCESS_MASK DesiredAccess,
                                ULONG ShareAccess,
                                PFILE_OBJECT FileObject,
                                PEXTFS_FCB *Result)
{
    PEXTFS_FCB candidate;
    PEXTFS_FCB fcb;
    NTSTATUS status = STATUS_SUCCESS;
    LIST_ENTRY reapList;

    InitializeListHead(&reapList);
    candidate = (PEXTFS_FCB)ExtfsAllocate(sizeof(*candidate));
    if (INFILTRATR_UNLIKELY(candidate == NULL))
        return STATUS_INSUFFICIENT_RESOURCES;
    ExtfsZeroMemory(candidate, sizeof(*candidate));
    candidate->Signature = EXTFS_FCB_SIGNATURE;
    candidate->Vcb = Vcb;
    candidate->Inode = *Inode;
    candidate->VolumeOpen = VolumeOpen;
    status = ExInitializeResourceLite(&candidate->DataResource);
    if (!NT_SUCCESS(status)) {
        ExFreePool(candidate);
        return status;
    }
    status = ExInitializeResourceLite(&candidate->PagingIoResource);
    if (!NT_SUCCESS(status)) {
        ExDeleteResourceLite(&candidate->DataResource);
        ExFreePool(candidate);
        return status;
    }
    ExInitializeFastMutex(&candidate->HeaderMutex);
    FsRtlSetupAdvancedHeader(&candidate->Header, &candidate->HeaderMutex);
    candidate->Header.NodeTypeCode = EXTFS_FCB_NODE_TYPE;
    candidate->Header.NodeByteSize = (CSHORT)sizeof(*candidate);
    candidate->Header.IsFastIoPossible = FastIoIsNotPossible;
    candidate->Header.Resource = &candidate->DataResource;
    candidate->Header.PagingIoResource = &candidate->PagingIoResource;
    ExtfsSyncFcbHeaderSizes(candidate);

    ExtfsAcquireFcbList(Vcb);
    ExtfsCollectReapableFcbsLocked(Vcb, &reapList, FALSE);
    fcb = ExtfsFindFcbLocked(Vcb, Inode, VolumeOpen);
    if (fcb != NULL) {
        status = IoCheckShareAccess(DesiredAccess, ShareAccess, FileObject,
                                    &fcb->ShareAccess, TRUE);
        if (NT_SUCCESS(status)) {
            ++fcb->HandleCount;
            ++fcb->FileObjectCount;
        }
    } else {
        IoSetShareAccess(DesiredAccess, ShareAccess, FileObject,
                         &candidate->ShareAccess);
        candidate->HandleCount = 1;
        candidate->FileObjectCount = 1;
        InsertTailList(&Vcb->FcbList, &candidate->Links);
        fcb = candidate;
        candidate = NULL;
    }
    if (NT_SUCCESS(status)) {
        InterlockedIncrement(&Vcb->OpenHandleCount);
        InterlockedIncrement(&Vcb->FileObjectCount);
    }
    ExtfsReleaseFcbList(Vcb);

    ExtfsDestroyFcbList(&reapList);
    if (candidate != NULL) ExtfsDestroyFcb(candidate);
    if (!NT_SUCCESS(status)) return status;
    *Result = fcb;
    return STATUS_SUCCESS;
}

static VOID ExtfsCleanupFileObject(PFILE_OBJECT FileObject)
{
    PEXTFS_FCB fcb = ExtfsFcbFromFile(FileObject);
    PEXTFS_CCB ccb = ExtfsCcbFromFile(FileObject);
    if (fcb == NULL || ccb == NULL || ccb->CleanupComplete) return;

    ExtfsAcquireFcbList(fcb->Vcb);
    IoRemoveShareAccess(FileObject, &fcb->ShareAccess);
    if (fcb->HandleCount > 0) --fcb->HandleCount;
    if (fcb->VolumeOpen && fcb->Vcb->VolumeLockFileObject == FileObject) {
        KIRQL savedIrql;
        IoAcquireVpbSpinLock(&savedIrql);
        fcb->Vcb->Vpb->Flags &= (USHORT)~VPB_LOCKED;
        IoReleaseVpbSpinLock(savedIrql);
        fcb->Vcb->VolumeLockFileObject = NULL;
    }
    ccb->CleanupComplete = TRUE;
    InterlockedDecrement(&fcb->Vcb->OpenHandleCount);
    ExtfsReleaseFcbList(fcb->Vcb);
}

/* Drop the FILE_OBJECT lifetime reference at CLOSE. Memory Manager performs
 * the authoritative mapped-section close check; FCBs that still back a live
 * view remain cached and are reconsidered after later closes/opens. */
static VOID ExtfsReleaseFcbReference(PEXTFS_FCB Fcb)
{
    PEXTFS_VCB vcb;
    LIST_ENTRY reapList;

    if (INFILTRATR_UNLIKELY(Fcb == NULL || Fcb->Vcb == NULL)) return;
    InitializeListHead(&reapList);
    vcb = Fcb->Vcb;
    ExtfsAcquireFcbList(vcb);
    if (Fcb->FileObjectCount > 0) --Fcb->FileObjectCount;
    InterlockedDecrement(&vcb->FileObjectCount);
    ExtfsCollectReapableFcbsLocked(vcb, &reapList, TRUE);
    ExtfsReleaseFcbList(vcb);

    ExtfsDestroyFcbList(&reapList);
}

/*
 * Bridge arbitrary byte reads from ext-core onto Windows block-device reads.
 * Lower disk stacks may require sector-sized/aligned requests, so round outward
 * to whole sectors, satisfy the device alignment requirement, then copy only
 * the originally requested byte range back to the core.
 */
static int ExtfsReadAt(void *User, extfs_u64 ByteOffset,
                       void *Destination, extfs_u32 ByteCount)
{
    PEXTFS_DISK_READER reader = (PEXTFS_DISK_READER)User;
    IO_STATUS_BLOCK ioStatus;
    LARGE_INTEGER offset;
    KEVENT event;
    PIRP irp;
    NTSTATUS status;
    ULONG sectorSize;
    ULONGLONG alignedStart;
    ULONGLONG requestEnd;
    ULONGLONG alignedEnd;
    ULONG transferLength;
    ULONG alignmentMask;
    PUCHAR allocation;
    PUCHAR transfer;

    if (reader == NULL || reader->DeviceObject == NULL ||
        Destination == NULL || ByteCount == 0U) {
        return -1;
    }

    sectorSize = reader->SectorSize;
    if (sectorSize < 512U || (sectorSize & (sectorSize - 1U)) != 0U) {
        sectorSize = 512U;
    }
    requestEnd = ByteOffset + ByteCount;
    if (requestEnd < ByteOffset) {
        return -1;
    }
    alignedStart = ByteOffset & ~((ULONGLONG)sectorSize - 1ULL);
    alignedEnd = (requestEnd + sectorSize - 1ULL) &
                 ~((ULONGLONG)sectorSize - 1ULL);
    if (alignedEnd < requestEnd || alignedEnd - alignedStart > 0xFFFFFFFFULL) {
        return -1;
    }
    transferLength = (ULONG)(alignedEnd - alignedStart);
    alignmentMask = reader->DeviceObject->AlignmentRequirement;
    if (transferLength > 0xFFFFFFFFU - alignmentMask) return -1;
    allocation = (PUCHAR)ExtfsAllocate(transferLength + alignmentMask);
    if (allocation == NULL) {
        return -1;
    }
    transfer = (PUCHAR)(((ULONG_PTR)allocation + alignmentMask) &
                        ~((ULONG_PTR)alignmentMask));

    offset.QuadPart = (LONGLONG)alignedStart;
    KeInitializeEvent(&event, NotificationEvent, FALSE);
    irp = IoBuildSynchronousFsdRequest(IRP_MJ_READ,
                                       reader->DeviceObject,
                                       transfer,
                                       transferLength,
                                       &offset,
                                       &event,
                                       &ioStatus);
    if (irp == NULL) {
        ExFreePool(allocation);
        return -1;
    }
    status = IoCallDriver(reader->DeviceObject, irp);
    if (status == STATUS_PENDING) {
        (void)KeWaitForSingleObject(&event, Executive, KernelMode,
                                    FALSE, NULL);
    }
    status = ioStatus.Status;
    if (!NT_SUCCESS(status) || ioStatus.Information != transferLength) {
        ExFreePool(allocation);
        return -1;
    }
    ExtfsCopyMemory(Destination,
                    transfer + (ULONG)(ByteOffset - alignedStart),
                    ByteCount);
    ExFreePool(allocation);
    return 0;
}

/*
 * Lower disk stacks normally require sector-aligned writes.  For partial
 * sectors perform a read/modify/write so the portable core can issue arbitrary
 * byte-range data overwrites without accidentally changing neighbouring data.
 */
static int ExtfsWriteAt(void *User, extfs_u64 ByteOffset,
                        const void *Source, extfs_u32 ByteCount)
{
    PEXTFS_DISK_READER reader = (PEXTFS_DISK_READER)User;
    IO_STATUS_BLOCK ioStatus;
    LARGE_INTEGER offset;
    KEVENT event;
    PIRP irp;
    NTSTATUS status;
    ULONG sectorSize;
    ULONGLONG alignedStart;
    ULONGLONG requestEnd;
    ULONGLONG alignedEnd;
    ULONG transferLength;
    ULONG alignmentMask;
    PUCHAR allocation = NULL;
    PUCHAR transfer;
    BOOLEAN locked = FALSE;
    int result = -1;

    if (reader == NULL || reader->DeviceObject == NULL ||
        reader->WriteResource == NULL || Source == NULL || ByteCount == 0U) {
        return -1;
    }
    sectorSize = reader->SectorSize;
    if (sectorSize < 512U || (sectorSize & (sectorSize - 1U)) != 0U) {
        sectorSize = 512U;
    }
    requestEnd = ByteOffset + ByteCount;
    if (requestEnd < ByteOffset) return -1;
    alignedStart = ByteOffset & ~((ULONGLONG)sectorSize - 1ULL);
    alignedEnd = (requestEnd + sectorSize - 1ULL) &
                 ~((ULONGLONG)sectorSize - 1ULL);
    if (alignedEnd < requestEnd || alignedEnd - alignedStart > 0xFFFFFFFFULL)
        return -1;
    transferLength = (ULONG)(alignedEnd - alignedStart);
    alignmentMask = reader->DeviceObject->AlignmentRequirement;
    if (transferLength > 0xFFFFFFFFU - alignmentMask) return -1;
    allocation = (PUCHAR)ExtfsAllocate(transferLength + alignmentMask);
    if (allocation == NULL) return -1;
    transfer = (PUCHAR)(((ULONG_PTR)allocation + alignmentMask) &
                        ~((ULONG_PTR)alignmentMask));

    /* A 1 KiB ext filesystem can place several filesystem blocks inside one
     * 4 KiB physical sector.  Serialize lower-device read/modify/write cycles
     * across the whole volume so writes to different files cannot lose each
     * other's neighbouring-sector changes. */
    FsRtlEnterFileSystem();
    (void)ExAcquireResourceExclusiveLite(reader->WriteResource, TRUE);
    locked = TRUE;

    if (ByteOffset != alignedStart || requestEnd != alignedEnd) {
        if (ExtfsReadAt(User, alignedStart, transfer, transferLength) != 0) {
            goto Exit;
        }
    }
    ExtfsCopyMemory(transfer + (ULONG)(ByteOffset - alignedStart),
                    Source, ByteCount);

    offset.QuadPart = (LONGLONG)alignedStart;
    KeInitializeEvent(&event, NotificationEvent, FALSE);
    irp = IoBuildSynchronousFsdRequest(IRP_MJ_WRITE,
                                       reader->DeviceObject, transfer,
                                       transferLength, &offset, &event,
                                       &ioStatus);
    if (irp == NULL) goto Exit;
    status = IoCallDriver(reader->DeviceObject, irp);
    if (status == STATUS_PENDING) {
        (void)KeWaitForSingleObject(&event, Executive, KernelMode,
                                    FALSE, NULL);
    }
    status = ioStatus.Status;
    if (!NT_SUCCESS(status) || ioStatus.Information != transferLength) {
        goto Exit;
    }
    result = 0;

Exit:
    if (locked) {
        ExReleaseResourceLite(reader->WriteResource);
        FsRtlExitFileSystem();
    }
    if (allocation != NULL) ExFreePool(allocation);
    return result;
}

static NTSTATUS ExtfsFlushLowerDevice(PEXTFS_DISK_READER Reader)
{
    IO_STATUS_BLOCK ioStatus;
    KEVENT event;
    PIRP irp;
    NTSTATUS status;
    if (Reader == NULL || Reader->DeviceObject == NULL)
        return STATUS_INVALID_DEVICE_STATE;
    KeInitializeEvent(&event, NotificationEvent, FALSE);
    irp = IoBuildSynchronousFsdRequest(IRP_MJ_FLUSH_BUFFERS,
                                       Reader->DeviceObject, NULL, 0U,
                                       NULL, &event, &ioStatus);
    if (irp == NULL) return STATUS_INSUFFICIENT_RESOURCES;
    status = IoCallDriver(Reader->DeviceObject, irp);
    if (status == STATUS_PENDING) {
        (void)KeWaitForSingleObject(&event, Executive, KernelMode,
                                    FALSE, NULL);
    }
    return ioStatus.Status;
}

/* Portable-core durability barrier used by JBD2.  The lower storage stack's
 * flush status is collapsed to the core's zero/non-zero host contract. */
static int ExtfsFlushCore(void *User)
{
    PEXTFS_DISK_READER reader = (PEXTFS_DISK_READER)User;
    return NT_SUCCESS(ExtfsFlushLowerDevice(reader)) ? 0 : -1;
}

/* JBD2 commit-record wall clock. Windows system time counts 100 ns intervals
 * since 1601-01-01; convert it to the Unix epoch and nanoseconds. */
static int ExtfsTimeCore(void *User, extfs_u64 *Seconds,
                         extfs_u32 *Nanoseconds)
{
    LARGE_INTEGER now;
    ULONGLONG ticks;
    UNREFERENCED_PARAMETER(User);
    if (Seconds == NULL || Nanoseconds == NULL) return -1;
#if defined(_MSC_VER)
    KeQuerySystemTimePrecise(&now);
#else
    /* MinGW-w64 DDK headers do not currently declare the precise routine.
     * Keep the authoritative WDK build on KeQuerySystemTimePrecise while the
     * non-authoritative cross-build uses the universally declared fallback. */
    KeQuerySystemTime(&now);
#endif
    if (now.QuadPart < 116444736000000000LL) return -1;
    ticks = (ULONGLONG)(now.QuadPart - 116444736000000000LL);
    *Seconds = (extfs_u64)(ticks / 10000000ULL);
    *Nanoseconds = (extfs_u32)((ticks % 10000000ULL) * 100ULL);
    return 0;
}

static ULONG ExtfsQuerySectorSize(PDEVICE_OBJECT DeviceObject)
{
    DISK_GEOMETRY geometry;
    IO_STATUS_BLOCK ioStatus;
    KEVENT event;
    PIRP irp;
    NTSTATUS status;
    ExtfsZeroMemory(&geometry, sizeof(geometry));
    KeInitializeEvent(&event, NotificationEvent, FALSE);
    irp = IoBuildDeviceIoControlRequest(IOCTL_DISK_GET_DRIVE_GEOMETRY,
                                        DeviceObject,
                                        NULL, 0U,
                                        &geometry, sizeof(geometry),
                                        FALSE, &event, &ioStatus);
    if (irp == NULL) {
        return 512U;
    }
    status = IoCallDriver(DeviceObject, irp);
    if (status == STATUS_PENDING) {
        (void)KeWaitForSingleObject(&event, Executive, KernelMode,
                                    FALSE, NULL);
    }
    status = ioStatus.Status;
    if (NT_SUCCESS(status) && geometry.BytesPerSector >= 512U &&
        (geometry.BytesPerSector & (geometry.BytesPerSector - 1U)) == 0U) {
        return geometry.BytesPerSector;
    }
    return 512U;
}


/* Ask the storage stack whether it will accept writes before advertising the
 * limited writer.  A hardware/software read-only medium can still mount for
 * reading, but the ExtFS volume device remains read-only. */
static BOOLEAN ExtfsDeviceIsWritable(PDEVICE_OBJECT DeviceObject)
{
    IO_STATUS_BLOCK ioStatus;
    KEVENT event;
    PIRP irp;
    NTSTATUS status;

    if (DeviceObject == NULL ||
        (DeviceObject->Characteristics & FILE_READ_ONLY_DEVICE) != 0U) {
        return FALSE;
    }
    KeInitializeEvent(&event, NotificationEvent, FALSE);
    irp = IoBuildDeviceIoControlRequest(IOCTL_DISK_IS_WRITABLE,
                                        DeviceObject,
                                        NULL, 0U, NULL, 0U,
                                        FALSE, &event, &ioStatus);
    if (irp == NULL) return FALSE;
    status = IoCallDriver(DeviceObject, irp);
    if (status == STATUS_PENDING) {
        (void)KeWaitForSingleObject(&event, Executive, KernelMode,
                                    FALSE, NULL);
    }
    return NT_SUCCESS(ioStatus.Status) ? TRUE : FALSE;
}

/*
 * Decode on-disk ext names as strict UTF-8.  Reject overlong encodings, UTF-16
 * surrogate code points and values above U+10FFFF rather than silently
 * replacing malformed names inside the kernel.
 */
static NTSTATUS ExtfsUtf8ToUnicode(const CHAR *Input, ULONG InputLength,
                                   WCHAR *Output, ULONG OutputCapacity,
                                   PULONG OutputLength)
{
    ULONG input = 0U;
    ULONG output = 0U;
    while (input < InputLength) {
        ULONG code;
        UCHAR first = (UCHAR)Input[input++];
        ULONG continuation;
        ULONG minimum;
        if (first < 0x80U) {
            code = first;
            continuation = 0U;
            minimum = 0U;
        } else if ((first & 0xE0U) == 0xC0U) {
            code = first & 0x1FU;
            continuation = 1U;
            minimum = 0x80U;
        } else if ((first & 0xF0U) == 0xE0U) {
            code = first & 0x0FU;
            continuation = 2U;
            minimum = 0x800U;
        } else if ((first & 0xF8U) == 0xF0U) {
            code = first & 0x07U;
            continuation = 3U;
            minimum = 0x10000U;
        } else {
            return STATUS_OBJECT_NAME_INVALID;
        }
        if (input + continuation > InputLength) {
            return STATUS_OBJECT_NAME_INVALID;
        }
        while (continuation != 0U) {
            UCHAR next = (UCHAR)Input[input++];
            if ((next & 0xC0U) != 0x80U) {
                return STATUS_OBJECT_NAME_INVALID;
            }
            code = (code << 6) | (next & 0x3FU);
            --continuation;
        }
        if (code < minimum || code > 0x10FFFFU ||
            (code >= 0xD800U && code <= 0xDFFFU)) {
            return STATUS_OBJECT_NAME_INVALID;
        }
        if (code <= 0xFFFFU) {
            if (output >= OutputCapacity) {
                return STATUS_BUFFER_TOO_SMALL;
            }
            Output[output++] = (WCHAR)code;
        } else {
            if (output + 2U > OutputCapacity) {
                return STATUS_BUFFER_TOO_SMALL;
            }
            code -= 0x10000U;
            Output[output++] = (WCHAR)(0xD800U + (code >> 10));
            Output[output++] = (WCHAR)(0xDC00U + (code & 0x3FFU));
        }
    }
    *OutputLength = output;
    return STATUS_SUCCESS;
}

/* Encode a Windows UTF-16 path component to strict UTF-8 for ext lookup. */
static NTSTATUS ExtfsUnicodeToUtf8(const WCHAR *Input, ULONG InputLength,
                                   CHAR *Output, ULONG OutputCapacity,
                                   PUCHAR OutputLength)
{
    ULONG input = 0U;
    ULONG output = 0U;
    while (input < InputLength) {
        ULONG code = Input[input++];
        if (code >= 0xD800U && code <= 0xDBFFU) {
            ULONG low;
            if (input >= InputLength) {
                return STATUS_OBJECT_NAME_INVALID;
            }
            low = Input[input++];
            if (low < 0xDC00U || low > 0xDFFFU) {
                return STATUS_OBJECT_NAME_INVALID;
            }
            code = 0x10000U + ((code - 0xD800U) << 10) + (low - 0xDC00U);
        } else if (code >= 0xDC00U && code <= 0xDFFFU) {
            return STATUS_OBJECT_NAME_INVALID;
        }
        if (code < 0x80U) {
            if (output + 1U > OutputCapacity) return STATUS_NAME_TOO_LONG;
            Output[output++] = (CHAR)code;
        } else if (code < 0x800U) {
            if (output + 2U > OutputCapacity) return STATUS_NAME_TOO_LONG;
            Output[output++] = (CHAR)(0xC0U | (code >> 6));
            Output[output++] = (CHAR)(0x80U | (code & 0x3FU));
        } else if (code < 0x10000U) {
            if (output + 3U > OutputCapacity) return STATUS_NAME_TOO_LONG;
            Output[output++] = (CHAR)(0xE0U | (code >> 12));
            Output[output++] = (CHAR)(0x80U | ((code >> 6) & 0x3FU));
            Output[output++] = (CHAR)(0x80U | (code & 0x3FU));
        } else {
            if (output + 4U > OutputCapacity) return STATUS_NAME_TOO_LONG;
            Output[output++] = (CHAR)(0xF0U | (code >> 18));
            Output[output++] = (CHAR)(0x80U | ((code >> 12) & 0x3FU));
            Output[output++] = (CHAR)(0x80U | ((code >> 6) & 0x3FU));
            Output[output++] = (CHAR)(0x80U | (code & 0x3FU));
        }
    }
    if (output > 255U) {
        return STATUS_NAME_TOO_LONG;
    }
    *OutputLength = (UCHAR)output;
    return STATUS_SUCCESS;
}

static WCHAR ExtfsFoldChar(WCHAR Character)
{
    return RtlUpcaseUnicodeChar(Character);
}

/*
 * Match the wildcard language used by Windows directory queries without
 * calling FsRtlIsNameInExpression.  That kernel helper can raise an SEH
 * exception under low-memory conditions, which the MinGW cross-build cannot
 * catch safely.  The small NFA below follows the MS-FSA rules for *, ?,
 * DOS_STAR, DOS_QM and DOS_DOT using fixed-size state arrays on the stack.
 */
static BOOLEAN ExtfsWildcardMatch(const WCHAR *Pattern, ULONG PatternLength,
                                  const WCHAR *Name, ULONG NameLength,
                                  BOOLEAN CaseSensitive)
{
    UCHAR current[EXTFS_MAX_PATTERN_CHARS + 1U];
    UCHAR next[EXTFS_MAX_PATTERN_CHARS + 1U];
    ULONG nameIndex;
    ULONG patternIndex;
    ULONG finalDot = 0xFFFFFFFFU;

    if (Pattern == NULL || Name == NULL ||
        PatternLength > EXTFS_MAX_PATTERN_CHARS) {
        return FALSE;
    }
    ExtfsZeroMemory(current, sizeof(current));
    current[0] = 1U;
    for (nameIndex = 0U; nameIndex < NameLength; ++nameIndex) {
        if (Name[nameIndex] == L'.') finalDot = nameIndex;
    }

    for (nameIndex = 0U; nameIndex <= NameLength; ++nameIndex) {
        /* Epsilon transitions only move forward, so one left-to-right pass
         * computes their complete closure for this name position. */
        for (patternIndex = 0U; patternIndex < PatternLength; ++patternIndex) {
            WCHAR p;
            if (current[patternIndex] == 0U) continue;
            p = Pattern[patternIndex];
            if (p == L'*' || p == DOS_STAR ||
                (p == DOS_QM &&
                 (nameIndex == NameLength || Name[nameIndex] == L'.')) ||
                (p == DOS_DOT && nameIndex == NameLength)) {
                current[patternIndex + 1U] = 1U;
            }
        }
        if (nameIndex == NameLength) {
            return current[PatternLength] != 0U;
        }

        ExtfsZeroMemory(next, sizeof(next));
        for (patternIndex = 0U; patternIndex < PatternLength; ++patternIndex) {
            WCHAR p;
            WCHAR n;
            if (current[patternIndex] == 0U) continue;
            p = Pattern[patternIndex];
            n = Name[nameIndex];
            if (p == L'*') {
                next[patternIndex] = 1U;
            } else if (p == DOS_STAR) {
                /* DOS_STAR may consume periods except the final period. */
                if (nameIndex != finalDot) next[patternIndex] = 1U;
            } else if (p == DOS_QM) {
                if (n != L'.') next[patternIndex + 1U] = 1U;
            } else if (p == DOS_DOT) {
                if (n == L'.') next[patternIndex + 1U] = 1U;
            } else if (p == L'?' ||
                       (CaseSensitive ? p == n
                                      : ExtfsFoldChar(p) == ExtfsFoldChar(n))) {
                next[patternIndex + 1U] = 1U;
            }
        }
        ExtfsCopyMemory(current, next, sizeof(current));
    }
    return FALSE;
}

typedef struct _EXTFS_CASE_LOOKUP {
    WCHAR Name[EXTFS_MAX_NAME_LENGTH];
    ULONG Length;
    ULONG InodeNumber;
    ULONG Matches;
} EXTFS_CASE_LOOKUP, *PEXTFS_CASE_LOOKUP;

static int ExtfsCaseLookupCallback(void *User, extfs_u32 InodeNumber,
                                   extfs_node_type Type, const char *Name,
                                   extfs_u8 NameLength)
{
    PEXTFS_CASE_LOOKUP context = (PEXTFS_CASE_LOOKUP)User;
    WCHAR unicode[EXTFS_MAX_NAME_LENGTH];
    ULONG unicodeLength = 0U;
    ULONG index;
    NTSTATUS status;
    (void)Type;

    status = ExtfsUtf8ToUnicode(Name, NameLength, unicode,
                                EXTFS_MAX_NAME_LENGTH, &unicodeLength);
    if (!NT_SUCCESS(status) || unicodeLength != context->Length) return 0;
    for (index = 0U; index < unicodeLength; ++index) {
        if (ExtfsFoldChar(unicode[index]) != ExtfsFoldChar(context->Name[index]))
            return 0;
    }
    context->InodeNumber = InodeNumber;
    ++context->Matches;
    return context->Matches > 1U ? 1 : 0;
}

/*
 * Preserve ext's exact byte-name semantics first.  Windows normally requests a
 * case-insensitive lookup, so only if the exact lookup misses do we enumerate
 * and apply Windows' Unicode single-character case folding.  If two ext names
 * collapse to the same Windows case-insensitive spelling, fail the lookup rather
 * than choosing an arbitrary inode.
 */
static extfs_status ExtfsLookupComponent(PEXTFS_VCB Vcb,
                                         const extfs_inode *Directory,
                                         const CHAR *Name, UCHAR NameLength,
                                         BOOLEAN CaseSensitive,
                                         PULONG InodeNumber,
                                         PVOID Scratch)
{
    extfs_u32 inodeNumber = 0U;
    extfs_status status = extfs_lookup(&Vcb->Volume, Directory, Name,
                                       NameLength, &inodeNumber,
                                       Scratch, Vcb->Volume.block_size);
    EXTFS_CASE_LOOKUP context;
    if (status != EXTFS_ERR_NOT_FOUND || CaseSensitive) {
        if (status == EXTFS_OK) *InodeNumber = (ULONG)inodeNumber;
        return status;
    }
    context.InodeNumber = 0U;
    context.Matches = 0U;
    {
        ULONG unicodeLength = 0U;
        NTSTATUS ntStatus = ExtfsUtf8ToUnicode(Name, NameLength, context.Name,
                                                EXTFS_MAX_NAME_LENGTH,
                                                &unicodeLength);
        if (!NT_SUCCESS(ntStatus)) return EXTFS_ERR_NOT_FOUND;
        context.Length = unicodeLength;
    }
    status = extfs_iterate_directory(&Vcb->Volume, Directory,
                                     ExtfsCaseLookupCallback, &context,
                                     Scratch, Vcb->Volume.block_size);
    if (status != EXTFS_OK && status != EXTFS_STOP) return status;
    if (context.Matches != 1U) return EXTFS_ERR_NOT_FOUND;
    *InodeNumber = (ULONG)context.InodeNumber;
    return EXTFS_OK;
}

/*
 * Resolve a FILE_OBJECT name either from the volume root or relative to the
 * related FILE_OBJECT supplied by the I/O manager.  A zero-length name on the
 * filesystem device represents a volume open rather than inode 2.
 */
static NTSTATUS ExtfsResolveFileName(PEXTFS_VCB Vcb,
                                     PFILE_OBJECT FileObject,
                                     BOOLEAN CaseSensitive,
                                     extfs_inode *Inode,
                                     PVOID Scratch,
                                     PBOOLEAN VolumeOpen)
{
    const WCHAR *cursor;
    ULONG characters;
    extfs_status extStatus;
    PEXTFS_FCB related;
    *VolumeOpen = FALSE;

    if ((FileObject->FileName.Length & (sizeof(WCHAR) - 1U)) != 0U ||
        (FileObject->FileName.Length != 0U &&
         FileObject->FileName.Buffer == NULL)) {
        return STATUS_OBJECT_NAME_INVALID;
    }

    cursor = FileObject->FileName.Buffer;
    characters = FileObject->FileName.Length / sizeof(WCHAR);
    related = ExtfsFcbFromFile(FileObject->RelatedFileObject);
    if (FileObject->RelatedFileObject != NULL && characters != 0U &&
        cursor[0] != L'\\') {
        if (related == NULL || related->Vcb != Vcb || related->VolumeOpen) {
            return STATUS_OBJECT_PATH_INVALID;
        }
        *Inode = related->Inode;
    } else {
        extStatus = extfs_read_inode(&Vcb->Volume, EXTFS_ROOT_INODE, Inode,
                                     Scratch, Vcb->Volume.block_size);
        if (extStatus != EXTFS_OK) return ExtfsStatusToNt(extStatus);
    }
    while (characters != 0U && (*cursor == L'\\' || *cursor == L'/')) {
        ++cursor;
        --characters;
    }
    if (characters == 0U && FileObject->FileName.Length == 0U &&
        FileObject->RelatedFileObject == NULL) {
        *VolumeOpen = TRUE;
        return STATUS_SUCCESS;
    }
    while (characters != 0U) {
        const WCHAR *start = cursor;
        ULONG componentChars = 0U;
        CHAR utf8[EXTFS_MAX_NAME_LENGTH];
        UCHAR utf8Length;
        ULONG nextInode;
        NTSTATUS status;
        while (componentChars < characters &&
               cursor[componentChars] != L'\\' &&
               cursor[componentChars] != L'/') {
            if (cursor[componentChars] == L':' ||
                cursor[componentChars] == 0U) {
                return STATUS_OBJECT_NAME_INVALID;
            }
            ++componentChars;
        }
        if (componentChars == 0U) {
            ++cursor;
            --characters;
            continue;
        }
        if (extfs_inode_type(Inode) != EXTFS_NODE_DIRECTORY) {
            return STATUS_NOT_A_DIRECTORY;
        }
        status = ExtfsUnicodeToUtf8(start, componentChars, utf8,
                                    sizeof(utf8), &utf8Length);
        if (!NT_SUCCESS(status)) return status;
        extStatus = ExtfsLookupComponent(Vcb, Inode, utf8, utf8Length,
                                         CaseSensitive, &nextInode, Scratch);
        if (extStatus != EXTFS_OK) return ExtfsStatusToNt(extStatus);
        extStatus = extfs_read_inode(&Vcb->Volume, nextInode, Inode,
                                     Scratch, Vcb->Volume.block_size);
        if (extStatus != EXTFS_OK) return ExtfsStatusToNt(extStatus);
        cursor += componentChars;
        characters -= componentChars;
        while (characters != 0U && (*cursor == L'\\' || *cursor == L'/')) {
            ++cursor;
            --characters;
        }
    }
    return STATUS_SUCCESS;
}

static ULONG ExtfsFileAttributes(PEXTFS_VCB Vcb, const extfs_inode *Inode)
{
    ULONG attributes = 0U;
    if (extfs_inode_type(Inode) == EXTFS_NODE_DIRECTORY)
        attributes |= FILE_ATTRIBUTE_DIRECTORY;
    if (Vcb == NULL || !Vcb->WriteEnabled ||
        extfs_inode_write_assess(&Vcb->Volume, Inode) != EXTFS_OK)
        attributes |= FILE_ATTRIBUTE_READONLY;
    if (attributes == 0U) attributes = FILE_ATTRIBUTE_NORMAL;
    return attributes;
}

static LARGE_INTEGER ExtfsNtTime(extfs_s64 UnixSeconds, ULONG Nanoseconds)
{
    LARGE_INTEGER value;
    const extfs_s64 epoch = 11644473600LL;
    extfs_s64 shifted;
    ULONGLONG ticks;
    if (Nanoseconds >= 1000000000U || UnixSeconds < -epoch) {
        value.QuadPart = 0;
        return value;
    }
    shifted = UnixSeconds + epoch;
    if ((extfs_u64)shifted >
        (0x7FFFFFFFFFFFFFFFULL - Nanoseconds / 100U) / 10000000ULL) {
        value.QuadPart = 0;
        return value;
    }
    ticks = (ULONGLONG)shifted * 10000000ULL + Nanoseconds / 100U;
    value.QuadPart = (LONGLONG)ticks;
    return value;
}

static ULONG ExtfsFileModeFromCreateOptions(ULONG Options)
{
    return Options & (FILE_WRITE_THROUGH | FILE_SEQUENTIAL_ONLY |
                      FILE_NO_INTERMEDIATE_BUFFERING |
                      FILE_SYNCHRONOUS_IO_ALERT |
                      FILE_SYNCHRONOUS_IO_NONALERT | FILE_DELETE_ON_CLOSE);
}

static VOID ExtfsFillBasicInformation(PEXTFS_VCB Vcb,
                                      const extfs_inode *Inode,
                                      PFILE_BASIC_INFORMATION Buffer)
{
    Buffer->CreationTime = ExtfsNtTime(Inode->creation_time,
                                       Inode->creation_time_nanoseconds);
    Buffer->LastAccessTime = ExtfsNtTime(Inode->access_time,
                                         Inode->access_time_nanoseconds);
    Buffer->LastWriteTime = ExtfsNtTime(Inode->modification_time,
                                        Inode->modification_time_nanoseconds);
    Buffer->ChangeTime = ExtfsNtTime(Inode->change_time,
                                     Inode->change_time_nanoseconds);
    Buffer->FileAttributes = ExtfsFileAttributes(Vcb, Inode);
}

static VOID ExtfsFillStandardInformation(PEXTFS_VCB Vcb,
                                         const extfs_inode *Inode,
                                         PFILE_STANDARD_INFORMATION Buffer)
{
    ULONGLONG blockSize = Vcb->Volume.block_size;
    ULONGLONG rounded;
    Buffer->EndOfFile.QuadPart = Inode->size > 0x7FFFFFFFFFFFFFFFULL
        ? 0x7FFFFFFFFFFFFFFFLL : (LONGLONG)Inode->size;
    if (Inode->size > 0x7FFFFFFFFFFFFFFFULL - (blockSize - 1ULL)) {
        rounded = 0x7FFFFFFFFFFFFFFFULL;
    } else {
        rounded = (Inode->size + blockSize - 1ULL) & ~(blockSize - 1ULL);
        if (rounded > 0x7FFFFFFFFFFFFFFFULL)
            rounded = 0x7FFFFFFFFFFFFFFFULL;
    }
    Buffer->AllocationSize.QuadPart = (LONGLONG)rounded;
    Buffer->NumberOfLinks = Inode->links_count;
    Buffer->DeletePending = FALSE;
    Buffer->Directory = extfs_inode_type(Inode) == EXTFS_NODE_DIRECTORY;
}

/*
 * Normalise buffered, direct and neither-style user output into one kernel VA.
 * Existing MDLs are borrowed; MDLs created here are unlocked/freed by the
 * matching ExtfsUnlockOutputBuffer call.
 */
static NTSTATUS ExtfsLockBuffer(PIRP Irp, ULONG Length,
                                LOCK_OPERATION Operation,
                                PEXTFS_OUTPUT_BUFFER Output)
{
    Output->Address = NULL;
    Output->Mdl = NULL;
    Output->Unlock = FALSE;
    if (Length == 0U) return STATUS_SUCCESS;
    if (Irp->MdlAddress != NULL) {
        Output->Address = MmGetSystemAddressForMdlSafe(
            Irp->MdlAddress, NormalPagePriority);
        return Output->Address != NULL ? STATUS_SUCCESS
                                      : STATUS_INSUFFICIENT_RESOURCES;
    }
    if (Irp->AssociatedIrp.SystemBuffer != NULL) {
        Output->Address = Irp->AssociatedIrp.SystemBuffer;
        return STATUS_SUCCESS;
    }
    if (Irp->UserBuffer == NULL) return STATUS_INVALID_USER_BUFFER;
    if (Irp->RequestorMode == KernelMode) {
        Output->Address = Irp->UserBuffer;
        return STATUS_SUCCESS;
    }
    Output->Mdl = IoAllocateMdl(Irp->UserBuffer, Length,
                                FALSE, FALSE, NULL);
    if (Output->Mdl == NULL) return STATUS_INSUFFICIENT_RESOURCES;
#if defined(_MSC_VER)
    __try {
        MmProbeAndLockPages(Output->Mdl, Irp->RequestorMode, Operation);
        Output->Unlock = TRUE;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        IoFreeMdl(Output->Mdl);
        Output->Mdl = NULL;
        return GetExceptionCode();
    }
#else
    UNREFERENCED_PARAMETER(Operation);
    /* MinGW cannot express the kernel SEH required around MmProbeAndLockPages.
     * Never make an unguarded probe: normal filesystem I/O arrives through an
     * MDL or system buffer, and an unexpected METHOD_NEITHER user pointer is
     * failed safely instead of risking a kernel exception/bugcheck. */
    IoFreeMdl(Output->Mdl);
    Output->Mdl = NULL;
    return STATUS_INVALID_USER_BUFFER;
#endif
    Output->Address = MmGetSystemAddressForMdlSafe(Output->Mdl,
                                                   NormalPagePriority);
    if (Output->Address == NULL) {
        MmUnlockPages(Output->Mdl);
        IoFreeMdl(Output->Mdl);
        Output->Mdl = NULL;
        Output->Unlock = FALSE;
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS ExtfsLockOutputBuffer(PIRP Irp, ULONG Length,
                                      PEXTFS_OUTPUT_BUFFER Output)
{
    return ExtfsLockBuffer(Irp, Length, IoWriteAccess, Output);
}

static NTSTATUS ExtfsLockInputBuffer(PIRP Irp, ULONG Length,
                                     PEXTFS_OUTPUT_BUFFER Input)
{
    return ExtfsLockBuffer(Irp, Length, IoReadAccess, Input);
}

static VOID ExtfsUnlockOutputBuffer(PEXTFS_OUTPUT_BUFFER Output)
{
    if (Output->Mdl != NULL) {
        if (Output->Unlock) MmUnlockPages(Output->Mdl);
        IoFreeMdl(Output->Mdl);
    }
    Output->Address = NULL;
    Output->Mdl = NULL;
    Output->Unlock = FALSE;
}

static ACCESS_MASK ExtfsNormalizeDesiredAccess(ACCESS_MASK DesiredAccess)
{
    GENERIC_MAPPING mapping;
    mapping.GenericRead = FILE_GENERIC_READ;
    mapping.GenericWrite = FILE_GENERIC_WRITE;
    mapping.GenericExecute = FILE_GENERIC_EXECUTE;
    mapping.GenericAll = FILE_ALL_ACCESS;
    if ((DesiredAccess & MAXIMUM_ALLOWED) != 0U) {
        DesiredAccess &= ~MAXIMUM_ALLOWED;
        DesiredAccess |= FILE_GENERIC_READ | FILE_GENERIC_EXECUTE;
    }
    RtlMapGenericMask(&DesiredAccess, &mapping);
    return DesiredAccess;
}

/*
 * CREATE resolves the ext inode, then acquires the volume's shared FCB for that
 * inode and a private CCB for this handle.  Windows share-access accounting is
 * attached to the shared FCB and released at CLEANUP, not CLOSE.
 */
NTSTATUS ExtfsDispatchCreate(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);
    PEXTFS_VCB vcb = ExtfsVcbFromDevice(DeviceObject);
    PFILE_OBJECT fileObject = stack->FileObject;
    ACCESS_MASK access;
    ULONG disposition;
    ULONG options;
    PVOID scratch = NULL;
    PEXTFS_FCB fcb = NULL;
    PEXTFS_CCB ccb = NULL;
    extfs_inode resolved;
    extfs_node_type nodeType;
    NTSTATUS status;
    BOOLEAN volumeOpen;
    BOOLEAN caseSensitive;

    if (vcb == NULL) {
        return ExtfsCompleteIrp(Irp, STATUS_SUCCESS, FILE_OPENED);
    }
    if (vcb->Dismounted) {
        return ExtfsCompleteIrp(Irp, STATUS_VOLUME_DISMOUNTED, 0U);
    }
    access = stack->Parameters.Create.SecurityContext != NULL
        ? stack->Parameters.Create.SecurityContext->DesiredAccess : 0U;
    access = ExtfsNormalizeDesiredAccess(access);
    disposition = (stack->Parameters.Create.Options >> 24) & 0xFFU;
    options = stack->Parameters.Create.Options & 0x00FFFFFFU;
    if ((access & (DELETE | WRITE_DAC | WRITE_OWNER)) != 0U ||
        (options & FILE_DELETE_ON_CLOSE) != 0U ||
        disposition == FILE_SUPERSEDE || disposition == FILE_CREATE ||
        disposition == FILE_OVERWRITE || disposition == FILE_OVERWRITE_IF) {
        return ExtfsCompleteIrp(Irp, STATUS_MEDIA_WRITE_PROTECTED, 0U);
    }
    if (!vcb->WriteEnabled &&
        (access & (FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_EA |
                   FILE_WRITE_ATTRIBUTES)) != 0U) {
        return ExtfsCompleteIrp(Irp, STATUS_MEDIA_WRITE_PROTECTED, 0U);
    }
    if ((options & FILE_OPEN_BY_FILE_ID) != 0U) {
        return ExtfsCompleteIrp(Irp, STATUS_NOT_SUPPORTED, 0U);
    }

    scratch = ExtfsAllocate(vcb->Volume.block_size);
    ccb = (PEXTFS_CCB)ExtfsAllocate(sizeof(*ccb));
    if (scratch == NULL || ccb == NULL) {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto Exit;
    }
    ExtfsZeroMemory(ccb, sizeof(*ccb));
    ExtfsZeroMemory(&resolved, sizeof(resolved));
    caseSensitive = (stack->Flags & SL_CASE_SENSITIVE) != 0U;
    status = ExtfsResolveFileName(vcb, fileObject, caseSensitive,
                                  &resolved, scratch, &volumeOpen);
    if (!NT_SUCCESS(status)) {
        if (status == STATUS_OBJECT_NAME_NOT_FOUND && disposition == FILE_OPEN_IF)
            status = STATUS_MEDIA_WRITE_PROTECTED;
        goto Exit;
    }

    if (!volumeOpen) {
        nodeType = extfs_inode_type(&resolved);
        if (nodeType != EXTFS_NODE_REGULAR &&
            nodeType != EXTFS_NODE_DIRECTORY &&
            nodeType != EXTFS_NODE_SYMLINK) {
            status = STATUS_NOT_SUPPORTED;
            goto Exit;
        }
        if (resolved.size > 0x7FFFFFFFFFFFFFFFULL) {
            status = STATUS_FILE_TOO_LARGE;
            goto Exit;
        }
        if ((options & FILE_DIRECTORY_FILE) != 0U &&
            nodeType != EXTFS_NODE_DIRECTORY) {
            status = STATUS_NOT_A_DIRECTORY;
            goto Exit;
        }
        if ((options & FILE_NON_DIRECTORY_FILE) != 0U &&
            nodeType == EXTFS_NODE_DIRECTORY) {
            status = STATUS_FILE_IS_A_DIRECTORY;
            goto Exit;
        }
        if ((access & (FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_EA |
                       FILE_WRITE_ATTRIBUTES)) != 0U &&
            extfs_inode_write_assess(&vcb->Volume, &resolved) != EXTFS_OK) {
            status = STATUS_MEDIA_WRITE_PROTECTED;
            goto Exit;
        }
    }

    status = ExtfsAcquireFcb(vcb, &resolved, volumeOpen, access,
                             stack->Parameters.Create.ShareAccess,
                             fileObject, &fcb);
    if (!NT_SUCCESS(status)) goto Exit;

    ccb->Signature = EXTFS_CCB_SIGNATURE;
    ccb->GrantedAccess = access;
    ccb->CreateOptions = options;
    fileObject->FsContext = &fcb->Header;
    fileObject->FsContext2 = ccb;
    fileObject->SectionObjectPointer = &fcb->SectionObjectPointers;
    fileObject->Vpb = vcb->Vpb;
    ccb = NULL;
    status = STATUS_SUCCESS;
Exit:
    if (scratch != NULL) ExFreePool(scratch);
    if (ccb != NULL) ExFreePool(ccb);
    return ExtfsCompleteIrp(Irp, status,
                            NT_SUCCESS(status) ? FILE_OPENED : 0U);
}

/* CLEANUP releases handle-level share state while the FILE_OBJECT/FCB remain
 * valid for the later CLOSE IRP. */
NTSTATUS ExtfsDispatchCleanup(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PFILE_OBJECT fileObject = IoGetCurrentIrpStackLocation(Irp)->FileObject;
    (void)DeviceObject;
    if (fileObject != NULL) {
        ExtfsCleanupFileObject(fileObject);
        fileObject->Flags |= FO_CLEANUP_COMPLETE;
    }
    return ExtfsCompleteIrp(Irp, STATUS_SUCCESS, 0U);
}

/* FLUSH is independent of CLEANUP.  Writable volumes pass the request to the
 * lower storage stack so completed in-place writes can be made durable. */
NTSTATUS ExtfsDispatchFlushBuffers(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PEXTFS_VCB vcb = ExtfsVcbFromDevice(DeviceObject);
    NTSTATUS status = STATUS_SUCCESS;
    if (vcb != NULL && vcb->Dismounted)
        return ExtfsCompleteIrp(Irp, STATUS_VOLUME_DISMOUNTED, 0U);
    if (vcb != NULL && vcb->WriteEnabled)
        status = ExtfsFlushLowerDevice(&vcb->Reader);
    return ExtfsCompleteIrp(Irp, status, 0U);
}

/* CLOSE releases the per-handle CCB and the FILE_OBJECT reference on the
 * shared FCB. The final CLOSE reclaims the FCB once CLEANUP has removed the
 * last share-access handle. */
NTSTATUS ExtfsDispatchClose(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PFILE_OBJECT fileObject = IoGetCurrentIrpStackLocation(Irp)->FileObject;
    PEXTFS_FCB fcb = ExtfsFcbFromFile(fileObject);
    PEXTFS_CCB ccb = ExtfsCcbFromFile(fileObject);
    (void)DeviceObject;
    if (fileObject != NULL) {
        /* The I/O manager sends CLEANUP in the handle-closing thread context;
         * share-access removal belongs there. CLOSE can run in a different
         * context, so it only tears down the per-file-object pointers/CCB. */
        if (ccb != NULL && !ccb->CleanupComplete) {
            DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                       "ExtFS: CLOSE observed without completed CLEANUP\n");
        }
        fileObject->FsContext = NULL;
        fileObject->FsContext2 = NULL;
        fileObject->SectionObjectPointer = NULL;
    }
    if (ccb != NULL) {
        ccb->Signature = 0U;
        ExFreePool(ccb);
    }
    if (fcb != NULL) ExtfsReleaseFcbReference(fcb);
    return ExtfsCompleteIrp(Irp, STATUS_SUCCESS, 0U);
}

NTSTATUS ExtfsDispatchRead(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);
    PEXTFS_FCB fcb = ExtfsFcbFromFile(stack->FileObject);
    ULONG length = stack->Parameters.Read.Length;
    LONGLONG offset = stack->Parameters.Read.ByteOffset.QuadPart;
    EXTFS_OUTPUT_BUFFER output;
    PVOID scratch;
    extfs_u32 bytesRead = 0U;
    extfs_status extStatus;
    NTSTATUS status;
    (void)DeviceObject;
    if (fcb == NULL || fcb->VolumeOpen) {
        return ExtfsCompleteIrp(Irp, STATUS_INVALID_DEVICE_REQUEST, 0U);
    }
    if (fcb->Vcb->Dismounted)
        return ExtfsCompleteIrp(Irp, STATUS_VOLUME_DISMOUNTED, 0U);
    if (extfs_inode_type(&fcb->Inode) == EXTFS_NODE_DIRECTORY) {
        return ExtfsCompleteIrp(Irp, STATUS_FILE_IS_A_DIRECTORY, 0U);
    }
    if (length == 0U) return ExtfsCompleteIrp(Irp, STATUS_SUCCESS, 0U);
    if (offset == FILE_USE_FILE_POINTER_POSITION &&
        stack->FileObject != NULL) {
        offset = stack->FileObject->CurrentByteOffset.QuadPart;
    }
    if (offset < 0) return ExtfsCompleteIrp(Irp, STATUS_INVALID_PARAMETER, 0U);
    status = ExtfsLockOutputBuffer(Irp, length, &output);
    if (!NT_SUCCESS(status)) return ExtfsCompleteIrp(Irp, status, 0U);
    scratch = ExtfsAllocate(fcb->Vcb->Volume.block_size);
    if (scratch == NULL) {
        ExtfsUnlockOutputBuffer(&output);
        return ExtfsCompleteIrp(Irp, STATUS_INSUFFICIENT_RESOURCES, 0U);
    }
    ExtfsAcquireFileData(fcb, FALSE);
    extStatus = extfs_read_file(&fcb->Vcb->Volume, &fcb->Inode,
                                (extfs_u64)offset, output.Address, length,
                                scratch, fcb->Vcb->Volume.block_size,
                                &bytesRead);
    ExtfsReleaseFileData(fcb);
    ExFreePool(scratch);
    ExtfsUnlockOutputBuffer(&output);
    status = ExtfsStatusToNt(extStatus);
    if (NT_SUCCESS(status) && stack->FileObject != NULL &&
        (stack->FileObject->Flags & FO_SYNCHRONOUS_IO) != 0U) {
        stack->FileObject->CurrentByteOffset.QuadPart = offset + bytesRead;
    }
    return ExtfsCompleteIrp(Irp, status,
                            NT_SUCCESS(status) ? bytesRead : 0U);
}

/*
 * Keep the Windows IRP layer ignorant of ext-family metadata mechanics.  The
 * portable core owns the distinction between unjournaled ext2 mutation,
 * journaled ext3 mutation and the bounded checksum-aware ext4 extent-tree path.
 */
static extfs_status ExtfsResizeRegularFile(PEXTFS_FCB Fcb,
                                           extfs_u64 NewSize,
                                           PVOID Scratch,
                                           extfs_u32 ScratchSize)
{
    if (Fcb == NULL) return EXTFS_ERR_INVALID_ARGUMENT;
    if (Fcb->Vcb->Volume.kind == EXTFS_KIND_EXT2) {
        return extfs_resize_file_ext2_direct(&Fcb->Vcb->Volume, &Fcb->Inode,
                                             NewSize, Scratch, ScratchSize);
    }
    if (Fcb->Vcb->Volume.kind == EXTFS_KIND_EXT3) {
        return extfs_resize_file_ext3_journaled_direct(
            &Fcb->Vcb->Volume, &Fcb->Inode, NewSize, Scratch, ScratchSize);
    }
    if (Fcb->Vcb->Volume.kind == EXTFS_KIND_EXT4) {
        return extfs_resize_file_ext4_journaled_extent_tree(
            &Fcb->Vcb->Volume, &Fcb->Inode, NewSize, Scratch, ScratchSize);
    }
    return EXTFS_ERR_UNSUPPORTED;
}

static VOID ExtfsDisableWritesIfUnsafe(PEXTFS_VCB Vcb)
{
    extfs_u32 risks = 0U;
    if (Vcb == NULL) return;
    if (extfs_write_assess(&Vcb->Volume, &risks) != EXTFS_OK) {
        Vcb->WriteEnabled = FALSE;
        if (Vcb->VolumeDeviceObject != NULL) {
            Vcb->VolumeDeviceObject->Characteristics |= FILE_READ_ONLY_DEVICE;
        }
    }
}

NTSTATUS ExtfsDispatchWrite(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);
    PEXTFS_FCB fcb = ExtfsFcbFromFile(stack->FileObject);
    PEXTFS_CCB ccb = ExtfsCcbFromFile(stack->FileObject);
    ULONG length = stack->Parameters.Write.Length;
    LONGLONG offset = stack->Parameters.Write.ByteOffset.QuadPart;
    EXTFS_OUTPUT_BUFFER input;
    PVOID scratch;
    extfs_u32 bytesWritten = 0U;
    extfs_status extStatus;
    NTSTATUS status;
    ULONGLONG endOffset;
    BOOLEAN metadataLocked = FALSE;
    (void)DeviceObject;

    if (fcb == NULL || ccb == NULL || fcb->VolumeOpen)
        return ExtfsCompleteIrp(Irp, STATUS_INVALID_DEVICE_REQUEST, 0U);
    if (fcb->Vcb->Dismounted)
        return ExtfsCompleteIrp(Irp, STATUS_VOLUME_DISMOUNTED, 0U);
    if (!fcb->Vcb->WriteEnabled)
        return ExtfsCompleteIrp(Irp, STATUS_MEDIA_WRITE_PROTECTED, 0U);
    if (extfs_inode_type(&fcb->Inode) != EXTFS_NODE_REGULAR)
        return ExtfsCompleteIrp(Irp, STATUS_MEDIA_WRITE_PROTECTED, 0U);
    if ((ccb->GrantedAccess & (FILE_WRITE_DATA | FILE_APPEND_DATA)) == 0U)
        return ExtfsCompleteIrp(Irp, STATUS_ACCESS_DENIED, 0U);
    if ((Irp->Flags & IRP_PAGING_IO) != 0U)
        return ExtfsCompleteIrp(Irp, STATUS_NOT_SUPPORTED, 0U);
    if (length == 0U) return ExtfsCompleteIrp(Irp, STATUS_SUCCESS, 0U);

    /* FILE_APPEND_DATA without FILE_WRITE_DATA is append-only regardless of
     * the caller's supplied byte offset. */
    if (offset == FILE_WRITE_TO_END_OF_FILE ||
        ((ccb->GrantedAccess & FILE_WRITE_DATA) == 0U &&
         (ccb->GrantedAccess & FILE_APPEND_DATA) != 0U)) {
        offset = (LONGLONG)fcb->Inode.size;
    } else if (offset == FILE_USE_FILE_POINTER_POSITION &&
               stack->FileObject != NULL) {
        offset = stack->FileObject->CurrentByteOffset.QuadPart;
    }
    if (offset < 0)
        return ExtfsCompleteIrp(Irp, STATUS_INVALID_PARAMETER, 0U);
    endOffset = (ULONGLONG)offset + length;
    if (endOffset < (ULONGLONG)offset)
        return ExtfsCompleteIrp(Irp, STATUS_INVALID_PARAMETER, 0U);

    status = ExtfsLockInputBuffer(Irp, length, &input);
    if (!NT_SUCCESS(status)) return ExtfsCompleteIrp(Irp, status, 0U);
    scratch = ExtfsAllocate(fcb->Vcb->Volume.block_size * 8U);
    if (scratch == NULL) {
        ExtfsUnlockOutputBuffer(&input);
        return ExtfsCompleteIrp(Irp, STATUS_INSUFFICIENT_RESOURCES, 0U);
    }

    ExtfsAcquireFileData(fcb, TRUE);
    if (endOffset > fcb->Inode.size) {
        /* ext2 mutates legacy metadata directly; ext3 commits its bounded
         * direct-file resize through JBD2; ext4 routes only the checksum-aware
         * bounded extent-tree checkpoint. */
        ExtfsAcquireMetadata(fcb->Vcb);
        metadataLocked = TRUE;
        extStatus = ExtfsResizeRegularFile(
            fcb, (extfs_u64)endOffset, scratch,
            fcb->Vcb->Volume.block_size * 8U);
        ExtfsReleaseMetadata(fcb->Vcb);
        metadataLocked = FALSE;
        if (extStatus != EXTFS_OK) {
            ExtfsDisableWritesIfUnsafe(fcb->Vcb);
            ExtfsReleaseFileData(fcb);
            ExFreePool(scratch);
            ExtfsUnlockOutputBuffer(&input);
            status = ExtfsStatusToNt(extStatus);
            return ExtfsCompleteIrp(Irp, status, 0U);
        }
        ExtfsSyncFcbHeaderSizes(fcb);
    }

    extStatus = extfs_write_file_existing(&fcb->Vcb->Volume, &fcb->Inode,
                                          (extfs_u64)offset, input.Address,
                                          length, scratch,
                                          fcb->Vcb->Volume.block_size * 2U,
                                          &bytesWritten);
    if (metadataLocked) ExtfsReleaseMetadata(fcb->Vcb);
    ExtfsReleaseFileData(fcb);
    ExFreePool(scratch);
    ExtfsUnlockOutputBuffer(&input);
    status = ExtfsStatusToNt(extStatus);
    if (extStatus == EXTFS_ERR_UNSUPPORTED) status = STATUS_NOT_SUPPORTED;
    if (NT_SUCCESS(status) &&
        (ccb->CreateOptions & FILE_WRITE_THROUGH) != 0U) {
        NTSTATUS flushStatus = ExtfsFlushLowerDevice(&fcb->Vcb->Reader);
        if (!NT_SUCCESS(flushStatus)) status = flushStatus;
    }
    if (bytesWritten != 0U && stack->FileObject != NULL &&
        (stack->FileObject->Flags & FO_SYNCHRONOUS_IO) != 0U)
        stack->FileObject->CurrentByteOffset.QuadPart = offset + bytesWritten;
    /* A failed payload write after successful ext2/ext3/ext4 growth can leave
     * a larger, zero-filled file. The transferred count still reflects only
     * user bytes that actually reached the data blocks. */
    return ExtfsCompleteIrp(Irp, status, bytesWritten);
}

NTSTATUS ExtfsDispatchSetInformation(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);
    PEXTFS_FCB fcb = ExtfsFcbFromFile(stack->FileObject);
    PEXTFS_CCB ccb = ExtfsCcbFromFile(stack->FileObject);
    FILE_INFORMATION_CLASS informationClass =
        stack->Parameters.SetFile.FileInformationClass;
    PVOID buffer = Irp->AssociatedIrp.SystemBuffer;
    PVOID scratch;
    LONGLONG requestedSize;
    extfs_status extStatus;
    NTSTATUS status;
    (void)DeviceObject;

    if (fcb == NULL || ccb == NULL || fcb->VolumeOpen)
        return ExtfsCompleteIrp(Irp, STATUS_INVALID_DEVICE_REQUEST, 0U);
    if (fcb->Vcb->Dismounted)
        return ExtfsCompleteIrp(Irp, STATUS_VOLUME_DISMOUNTED, 0U);
    if (informationClass != FileEndOfFileInformation)
        return ExtfsCompleteIrp(Irp, STATUS_MEDIA_WRITE_PROTECTED, 0U);
    if (!fcb->Vcb->WriteEnabled)
        return ExtfsCompleteIrp(Irp, STATUS_MEDIA_WRITE_PROTECTED, 0U);
    if ((ccb->GrantedAccess & FILE_WRITE_DATA) == 0U)
        return ExtfsCompleteIrp(Irp, STATUS_ACCESS_DENIED, 0U);
    if (extfs_inode_type(&fcb->Inode) != EXTFS_NODE_REGULAR)
        return ExtfsCompleteIrp(Irp, STATUS_INVALID_DEVICE_REQUEST, 0U);
    if (buffer == NULL || stack->Parameters.SetFile.Length <
                          sizeof(FILE_END_OF_FILE_INFORMATION)) {
        return ExtfsCompleteIrp(Irp, STATUS_INFO_LENGTH_MISMATCH, 0U);
    }
    requestedSize =
        ((PFILE_END_OF_FILE_INFORMATION)buffer)->EndOfFile.QuadPart;
    if (requestedSize < 0)
        return ExtfsCompleteIrp(Irp, STATUS_INVALID_PARAMETER, 0U);
    if ((ULONGLONG)requestedSize == fcb->Inode.size)
        return ExtfsCompleteIrp(Irp, STATUS_SUCCESS, 0U);

    scratch = ExtfsAllocate(fcb->Vcb->Volume.block_size * 8U);
    if (scratch == NULL)
        return ExtfsCompleteIrp(Irp, STATUS_INSUFFICIENT_RESOURCES, 0U);

    ExtfsAcquireFileData(fcb, TRUE);
    ExtfsAcquireMetadata(fcb->Vcb);
    extStatus = ExtfsResizeRegularFile(
        fcb, (extfs_u64)requestedSize, scratch,
        fcb->Vcb->Volume.block_size * 8U);
    ExtfsReleaseMetadata(fcb->Vcb);
    if (extStatus == EXTFS_OK) ExtfsSyncFcbHeaderSizes(fcb);
    ExtfsReleaseFileData(fcb);
    ExFreePool(scratch);

    if (extStatus != EXTFS_OK) ExtfsDisableWritesIfUnsafe(fcb->Vcb);
    status = ExtfsStatusToNt(extStatus);
    if (NT_SUCCESS(status) &&
        (ccb->CreateOptions & FILE_WRITE_THROUGH) != 0U) {
        NTSTATUS flushStatus = ExtfsFlushLowerDevice(&fcb->Vcb->Reader);
        if (!NT_SUCCESS(flushStatus)) status = flushStatus;
    }
    return ExtfsCompleteIrp(Irp, status, 0U);
}

NTSTATUS ExtfsDispatchQueryInformation(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);
    PEXTFS_FCB fcb = ExtfsFcbFromFile(stack->FileObject);
    PEXTFS_CCB ccb = ExtfsCcbFromFile(stack->FileObject);
    ULONG length = stack->Parameters.QueryFile.Length;
    FILE_INFORMATION_CLASS informationClass =
        stack->Parameters.QueryFile.FileInformationClass;
    PUCHAR buffer = (PUCHAR)Irp->AssociatedIrp.SystemBuffer;
    ULONG used = 0U;
    NTSTATUS status = STATUS_SUCCESS;
    (void)DeviceObject;
    if (fcb == NULL || ccb == NULL || buffer == NULL) {
        return ExtfsCompleteIrp(Irp, STATUS_INVALID_PARAMETER, 0U);
    }
    if (fcb->Vcb->Dismounted)
        return ExtfsCompleteIrp(Irp, STATUS_VOLUME_DISMOUNTED, 0U);
    ExtfsZeroMemory(buffer, length);
    switch (informationClass) {
        case FileAllInformation:
        {
            ULONG base = FIELD_OFFSET(FILE_ALL_INFORMATION,
                                      NameInformation.FileName);
            ULONG nameBytes = stack->FileObject->FileName.Length;
            ULONG copyBytes;
            PFILE_ALL_INFORMATION info = (PFILE_ALL_INFORMATION)buffer;
            if (length < base) { status = STATUS_INFO_LENGTH_MISMATCH; break; }
            ExtfsFillBasicInformation(fcb->Vcb, &fcb->Inode, &info->BasicInformation);
            ExtfsFillStandardInformation(fcb->Vcb, &fcb->Inode,
                                         &info->StandardInformation);
            info->InternalInformation.IndexNumber.QuadPart = fcb->Inode.number;
            info->EaInformation.EaSize = 0U;
            info->AccessInformation.AccessFlags = ccb->GrantedAccess;
            info->PositionInformation.CurrentByteOffset =
                stack->FileObject->CurrentByteOffset;
            info->ModeInformation.Mode =
                ExtfsFileModeFromCreateOptions(ccb->CreateOptions);
            info->AlignmentInformation.AlignmentRequirement =
                fcb->Vcb->VolumeDeviceObject->AlignmentRequirement;
            info->NameInformation.FileNameLength = nameBytes;
            copyBytes = length - base < nameBytes ? length - base : nameBytes;
            if (copyBytes != 0U) {
                ExtfsCopyMemory(info->NameInformation.FileName,
                                stack->FileObject->FileName.Buffer, copyBytes);
            }
            used = base + copyBytes;
            if (copyBytes != nameBytes) status = STATUS_BUFFER_OVERFLOW;
            break;
        }
        case FileBasicInformation:
            if (length < sizeof(FILE_BASIC_INFORMATION)) {
                status = STATUS_INFO_LENGTH_MISMATCH; break;
            }
            ExtfsFillBasicInformation(fcb->Vcb, &fcb->Inode,
                (PFILE_BASIC_INFORMATION)buffer);
            used = sizeof(FILE_BASIC_INFORMATION);
            break;
        case FileStandardInformation:
            if (length < sizeof(FILE_STANDARD_INFORMATION)) {
                status = STATUS_INFO_LENGTH_MISMATCH; break;
            }
            ExtfsFillStandardInformation(fcb->Vcb, &fcb->Inode,
                (PFILE_STANDARD_INFORMATION)buffer);
            used = sizeof(FILE_STANDARD_INFORMATION);
            break;
        case FileInternalInformation:
            if (length < sizeof(FILE_INTERNAL_INFORMATION)) {
                status = STATUS_INFO_LENGTH_MISMATCH; break;
            }
            ((PFILE_INTERNAL_INFORMATION)buffer)->IndexNumber.QuadPart =
                fcb->Inode.number;
            used = sizeof(FILE_INTERNAL_INFORMATION);
            break;
        case FileEaInformation:
            if (length < sizeof(FILE_EA_INFORMATION)) {
                status = STATUS_INFO_LENGTH_MISMATCH; break;
            }
            ((PFILE_EA_INFORMATION)buffer)->EaSize = 0U;
            used = sizeof(FILE_EA_INFORMATION);
            break;
        case FilePositionInformation:
            if (length < sizeof(FILE_POSITION_INFORMATION)) {
                status = STATUS_INFO_LENGTH_MISMATCH; break;
            }
            ((PFILE_POSITION_INFORMATION)buffer)->CurrentByteOffset =
                stack->FileObject->CurrentByteOffset;
            used = sizeof(FILE_POSITION_INFORMATION);
            break;
        case FileAccessInformation:
            if (length < sizeof(FILE_ACCESS_INFORMATION)) {
                status = STATUS_INFO_LENGTH_MISMATCH; break;
            }
            ((PFILE_ACCESS_INFORMATION)buffer)->AccessFlags =
                ccb->GrantedAccess;
            used = sizeof(FILE_ACCESS_INFORMATION);
            break;
        case FileModeInformation:
            if (length < sizeof(FILE_MODE_INFORMATION)) {
                status = STATUS_INFO_LENGTH_MISMATCH; break;
            }
            ((PFILE_MODE_INFORMATION)buffer)->Mode =
                ExtfsFileModeFromCreateOptions(ccb->CreateOptions);
            used = sizeof(FILE_MODE_INFORMATION);
            break;
        case FileAlignmentInformation:
            if (length < sizeof(FILE_ALIGNMENT_INFORMATION)) {
                status = STATUS_INFO_LENGTH_MISMATCH; break;
            }
            ((PFILE_ALIGNMENT_INFORMATION)buffer)->AlignmentRequirement =
                fcb->Vcb->VolumeDeviceObject->AlignmentRequirement;
            used = sizeof(FILE_ALIGNMENT_INFORMATION);
            break;
        case FileNetworkOpenInformation:
            if (length < sizeof(FILE_NETWORK_OPEN_INFORMATION)) {
                status = STATUS_INFO_LENGTH_MISMATCH; break;
            } else {
                PFILE_NETWORK_OPEN_INFORMATION info =
                    (PFILE_NETWORK_OPEN_INFORMATION)buffer;
                FILE_STANDARD_INFORMATION standard;
                info->CreationTime = ExtfsNtTime(fcb->Inode.creation_time, fcb->Inode.creation_time_nanoseconds);
                info->LastAccessTime = ExtfsNtTime(fcb->Inode.access_time, fcb->Inode.access_time_nanoseconds);
                info->LastWriteTime = ExtfsNtTime(fcb->Inode.modification_time, fcb->Inode.modification_time_nanoseconds);
                info->ChangeTime = ExtfsNtTime(fcb->Inode.change_time, fcb->Inode.change_time_nanoseconds);
                ExtfsFillStandardInformation(fcb->Vcb, &fcb->Inode, &standard);
                info->AllocationSize = standard.AllocationSize;
                info->EndOfFile = standard.EndOfFile;
                info->FileAttributes = ExtfsFileAttributes(fcb->Vcb, &fcb->Inode);
                used = sizeof(FILE_NETWORK_OPEN_INFORMATION);
            }
            break;
        case FileAttributeTagInformation:
            if (length < sizeof(FILE_ATTRIBUTE_TAG_INFORMATION)) {
                status = STATUS_INFO_LENGTH_MISMATCH; break;
            }
            ((PFILE_ATTRIBUTE_TAG_INFORMATION)buffer)->FileAttributes =
                ExtfsFileAttributes(fcb->Vcb, &fcb->Inode);
            ((PFILE_ATTRIBUTE_TAG_INFORMATION)buffer)->ReparseTag = 0U;
            used = sizeof(FILE_ATTRIBUTE_TAG_INFORMATION);
            break;
        case FileNormalizedNameInformation:
            status = STATUS_NOT_SUPPORTED;
            break;
        case FileNameInformation:
        {
            ULONG base = sizeof(ULONG);
            ULONG nameBytes = stack->FileObject->FileName.Length;
            ULONG copyBytes;
            PEXTFS_FILE_NAME_INFORMATION info =
                (PEXTFS_FILE_NAME_INFORMATION)buffer;
            if (length < base) { status = STATUS_INFO_LENGTH_MISMATCH; break; }
            info->FileNameLength = nameBytes;
            copyBytes = length - base < nameBytes ? length - base : nameBytes;
            if (copyBytes != 0U) {
                ExtfsCopyMemory(info->FileName,
                                stack->FileObject->FileName.Buffer, copyBytes);
            }
            used = base + copyBytes;
            if (copyBytes != nameBytes) status = STATUS_BUFFER_OVERFLOW;
            break;
        }
        default:
            status = STATUS_INVALID_INFO_CLASS;
            break;
    }
    return ExtfsCompleteIrp(Irp, status,
                            NT_SUCCESS(status) || status == STATUS_BUFFER_OVERFLOW
                                ? used : 0U);
}

static VOID ExtfsVolumeAllocationGeometry(PEXTFS_VCB Vcb,
                                           PLARGE_INTEGER TotalUnits,
                                           PLARGE_INTEGER FreeUnits,
                                           PULONG SectorsPerUnit,
                                           PULONG BytesPerSector)
{
    ULONGLONG blockSize = Vcb->Volume.block_size;
    ULONGLONG sectorSize = Vcb->Reader.SectorSize;
    ULONGLONG ratio;
    *BytesPerSector = (ULONG)sectorSize;
    if (blockSize >= sectorSize) {
        *SectorsPerUnit = (ULONG)(blockSize / sectorSize);
        if (*SectorsPerUnit == 0U) *SectorsPerUnit = 1U;
        TotalUnits->QuadPart = (LONGLONG)Vcb->Volume.total_blocks;
        FreeUnits->QuadPart = (LONGLONG)Vcb->Volume.free_blocks;
        return;
    }
    /* Windows cannot express an allocation unit smaller than one sector.
     * Coalesce ext blocks so TotalUnits * sector size still reports the real
     * byte capacity on unusual 4Kn devices carrying a smaller ext block size. */
    ratio = sectorSize / blockSize;
    if (ratio == 0U) ratio = 1U;
    *SectorsPerUnit = 1U;
    TotalUnits->QuadPart = (LONGLONG)(Vcb->Volume.total_blocks / ratio);
    FreeUnits->QuadPart = (LONGLONG)(Vcb->Volume.free_blocks / ratio);
}

NTSTATUS ExtfsDispatchQueryVolumeInformation(PDEVICE_OBJECT DeviceObject,
                                              PIRP Irp)
{
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);
    PEXTFS_VCB vcb = ExtfsVcbFromDevice(DeviceObject);
    ULONG length = stack->Parameters.QueryVolume.Length;
    FS_INFORMATION_CLASS informationClass =
        stack->Parameters.QueryVolume.FsInformationClass;
    PUCHAR buffer = (PUCHAR)Irp->AssociatedIrp.SystemBuffer;
    ULONG used = 0U;
    NTSTATUS status = STATUS_SUCCESS;
    if (vcb == NULL || buffer == NULL) {
        return ExtfsCompleteIrp(Irp, STATUS_INVALID_PARAMETER, 0U);
    }
    if (vcb->Dismounted)
        return ExtfsCompleteIrp(Irp, STATUS_VOLUME_DISMOUNTED, 0U);
    ExtfsZeroMemory(buffer, length);
    switch (informationClass) {
        case FileFsVolumeInformation:
        {
            WCHAR label[17];
            ULONG labelChars = 0U;
            ULONG base = FIELD_OFFSET(EXTFS_FS_VOLUME_INFORMATION, VolumeLabel);
            ULONG labelBytes;
            PEXTFS_FS_VOLUME_INFORMATION info =
                (PEXTFS_FS_VOLUME_INFORMATION)buffer;
            status = ExtfsUtf8ToUnicode(vcb->Volume.label,
                ExtfsStringLength(vcb->Volume.label), label, 17U, &labelChars);
            if (!NT_SUCCESS(status)) { labelChars = 0U; status = STATUS_SUCCESS; }
            labelBytes = labelChars * sizeof(WCHAR);
            if (length < base) { status = STATUS_INFO_LENGTH_MISMATCH; break; }
            info->VolumeCreationTime.QuadPart = 0;
            info->VolumeSerialNumber = vcb->Vpb->SerialNumber;
            info->VolumeLabelLength = labelBytes;
            info->SupportsObjects = FALSE;
            if (labelBytes > length - base) {
                labelBytes = length - base;
                status = STATUS_BUFFER_OVERFLOW;
            }
            ExtfsCopyMemory(info->VolumeLabel, label, labelBytes);
            used = base + labelBytes;
            break;
        }
        case FileFsSizeInformation:
            if (length < sizeof(EXTFS_FS_SIZE_INFORMATION)) {
                status = STATUS_INFO_LENGTH_MISMATCH; break;
            } else {
                PEXTFS_FS_SIZE_INFORMATION info =
                    (PEXTFS_FS_SIZE_INFORMATION)buffer;
                ExtfsVolumeAllocationGeometry(
                    vcb, &info->TotalAllocationUnits,
                    &info->AvailableAllocationUnits,
                    &info->SectorsPerAllocationUnit, &info->BytesPerSector);
                used = sizeof(*info);
            }
            break;
        case FileFsFullSizeInformation:
            if (length < sizeof(EXTFS_FS_FULL_SIZE_INFORMATION)) {
                status = STATUS_INFO_LENGTH_MISMATCH; break;
            } else {
                PEXTFS_FS_FULL_SIZE_INFORMATION info =
                    (PEXTFS_FS_FULL_SIZE_INFORMATION)buffer;
                ExtfsVolumeAllocationGeometry(
                    vcb, &info->TotalAllocationUnits,
                    &info->ActualAvailableAllocationUnits,
                    &info->SectorsPerAllocationUnit, &info->BytesPerSector);
                info->CallerAvailableAllocationUnits =
                    info->ActualAvailableAllocationUnits;
                used = sizeof(*info);
            }
            break;
        case FileFsDeviceInformation:
            if (length < sizeof(FILE_FS_DEVICE_INFORMATION)) {
                status = STATUS_INFO_LENGTH_MISMATCH; break;
            }
            ((PFILE_FS_DEVICE_INFORMATION)buffer)->DeviceType = FILE_DEVICE_DISK;
            ((PFILE_FS_DEVICE_INFORMATION)buffer)->Characteristics =
                vcb->Vpb->RealDevice->Characteristics;
            used = sizeof(FILE_FS_DEVICE_INFORMATION);
            break;
        case FileFsAttributeInformation:
        {
            static const WCHAR fsName[] = L"ExtFS";
            ULONG nameBytes = 5U * sizeof(WCHAR);
            ULONG base = FIELD_OFFSET(EXTFS_FS_ATTRIBUTE_INFORMATION,
                                      FileSystemName);
            PEXTFS_FS_ATTRIBUTE_INFORMATION info =
                (PEXTFS_FS_ATTRIBUTE_INFORMATION)buffer;
            if (length < base) { status = STATUS_INFO_LENGTH_MISMATCH; break; }
            info->FileSystemAttributes = FILE_CASE_SENSITIVE_SEARCH |
                FILE_CASE_PRESERVED_NAMES | FILE_UNICODE_ON_DISK |
                FILE_SUPPORTS_SPARSE_FILES;
            if (!vcb->WriteEnabled)
                info->FileSystemAttributes |= FILE_READ_ONLY_VOLUME;
            info->MaximumComponentNameLength = EXTFS_MAX_NAME_LENGTH;
            info->FileSystemNameLength = nameBytes;
            if (nameBytes > length - base) {
                nameBytes = length - base;
                status = STATUS_BUFFER_OVERFLOW;
            }
            ExtfsCopyMemory(info->FileSystemName, fsName, nameBytes);
            used = base + nameBytes;
            break;
        }
        default:
            status = STATUS_INVALID_INFO_CLASS;
            break;
    }
    return ExtfsCompleteIrp(Irp, status,
                            NT_SUCCESS(status) || status == STATUS_BUFFER_OVERFLOW
                                ? used : 0U);
}

static int ExtfsPickDirectoryEntry(void *User, extfs_u32 InodeNumber,
                                   extfs_node_type Type, const char *Name,
                                   extfs_u8 NameLength)
{
    PEXTFS_DIRENT_PICK pick = (PEXTFS_DIRENT_PICK)User;
    WCHAR unicode[EXTFS_MAX_NAME_LENGTH];
    ULONG unicodeLength = 0U;
    NTSTATUS status;
    if ((NameLength == 1U && Name[0] == '.') ||
        (NameLength == 2U && Name[0] == '.' && Name[1] == '.')) {
        return 0;
    }
    if (pick->CurrentIndex++ < pick->StartIndex) return 0;
    status = ExtfsUtf8ToUnicode(Name, NameLength, unicode,
                                EXTFS_MAX_NAME_LENGTH, &unicodeLength);
    if (!NT_SUCCESS(status)) {
        /* ext directory names are raw bytes. Never make a live entry silently
         * disappear merely because Windows cannot represent it as UTF-16. */
        pick->NameStatus = status;
        return 1;
    }
    if (!ExtfsWildcardMatch(pick->Pattern, pick->PatternLength,
                            unicode, unicodeLength, pick->CaseSensitive)) {
        return 0;
    }
    pick->Found = TRUE;
    pick->FoundIndex = pick->CurrentIndex - 1U;
    pick->InodeNumber = InodeNumber;
    pick->Type = Type;
    pick->Utf8Length = NameLength;
    ExtfsCopyMemory(pick->Utf8Name, Name, NameLength);
    pick->Utf8Name[NameLength] = '\0';
    return 1;
}

static NTSTATUS ExtfsFindDirectoryEntry(PEXTFS_FCB Fcb, PEXTFS_CCB Ccb,
                                        BOOLEAN CaseSensitive,
                                        PEXTFS_DIRENT_PICK Pick,
                                        PVOID Scratch)
{
    extfs_status status;
    ExtfsZeroMemory(Pick, sizeof(*Pick));
    Pick->StartIndex = Ccb->DirectoryIndex;
    Pick->Pattern = Ccb->Pattern;
    Pick->PatternLength = Ccb->PatternLength;
    Pick->CaseSensitive = CaseSensitive;
    status = extfs_iterate_directory(&Fcb->Vcb->Volume, &Fcb->Inode,
                                     ExtfsPickDirectoryEntry, Pick,
                                     Scratch, Fcb->Vcb->Volume.block_size);
    if (status != EXTFS_OK && status != EXTFS_STOP)
        return ExtfsStatusToNt(status);
    if (!NT_SUCCESS(Pick->NameStatus)) return Pick->NameStatus;
    return Pick->Found ? STATUS_SUCCESS : STATUS_NO_MORE_FILES;
}

static ULONG ExtfsDirectoryBaseLength(FILE_INFORMATION_CLASS InformationClass)
{
    switch (InformationClass) {
        case FileDirectoryInformation:
            return FIELD_OFFSET(FILE_DIRECTORY_INFORMATION, FileName);
        case FileFullDirectoryInformation:
            return FIELD_OFFSET(FILE_FULL_DIR_INFORMATION, FileName);
        case FileIdFullDirectoryInformation:
            return FIELD_OFFSET(FILE_ID_FULL_DIR_INFORMATION, FileName);
        case FileBothDirectoryInformation:
            return FIELD_OFFSET(FILE_BOTH_DIR_INFORMATION, FileName);
        case FileIdBothDirectoryInformation:
            return FIELD_OFFSET(FILE_ID_BOTH_DIR_INFORMATION, FileName);
        case FileNamesInformation:
            return FIELD_OFFSET(FILE_NAMES_INFORMATION, FileName);
        default:
            return 0U;
    }
}

static NTSTATUS ExtfsFillDirectoryRecord(FILE_INFORMATION_CLASS InformationClass,
                                         PUCHAR Record, ULONG Available,
                                         PEXTFS_FCB Directory,
                                         PEXTFS_DIRENT_PICK Pick,
                                         const extfs_inode *Inode,
                                         PULONG RecordLength)
{
    WCHAR name[EXTFS_MAX_NAME_LENGTH];
    ULONG nameChars = 0U;
    ULONG nameBytes;
    ULONG base = ExtfsDirectoryBaseLength(InformationClass);
    NTSTATUS status;
    FILE_STANDARD_INFORMATION standard;
    ULONG attributes = ExtfsFileAttributes(Directory->Vcb, Inode);
    if (base == 0U) return STATUS_INVALID_INFO_CLASS;
    status = ExtfsUtf8ToUnicode(Pick->Utf8Name, Pick->Utf8Length,
                                name, EXTFS_MAX_NAME_LENGTH, &nameChars);
    if (!NT_SUCCESS(status)) return status;
    nameBytes = nameChars * sizeof(WCHAR);
    if (base + nameBytes > Available) return STATUS_BUFFER_OVERFLOW;
    ExtfsZeroMemory(Record, base);
    ExtfsFillStandardInformation(Directory->Vcb, Inode, &standard);
    if (InformationClass == FileNamesInformation) {
        PFILE_NAMES_INFORMATION info = (PFILE_NAMES_INFORMATION)Record;
        info->FileIndex = Pick->FoundIndex;
        info->FileNameLength = nameBytes;
        ExtfsCopyMemory(info->FileName, name, nameBytes);
    } else {
        PFILE_DIRECTORY_INFORMATION info = (PFILE_DIRECTORY_INFORMATION)Record;
        info->FileIndex = Pick->FoundIndex;
        info->CreationTime = ExtfsNtTime(Inode->creation_time, Inode->creation_time_nanoseconds);
        info->LastAccessTime = ExtfsNtTime(Inode->access_time, Inode->access_time_nanoseconds);
        info->LastWriteTime = ExtfsNtTime(Inode->modification_time, Inode->modification_time_nanoseconds);
        info->ChangeTime = ExtfsNtTime(Inode->change_time, Inode->change_time_nanoseconds);
        info->EndOfFile = standard.EndOfFile;
        info->AllocationSize = standard.AllocationSize;
        info->FileAttributes = attributes;
        info->FileNameLength = nameBytes;
        if (InformationClass == FileFullDirectoryInformation) {
            PFILE_FULL_DIR_INFORMATION full = (PFILE_FULL_DIR_INFORMATION)Record;
            full->EaSize = 0U;
            ExtfsCopyMemory(full->FileName, name, nameBytes);
        } else if (InformationClass == FileIdFullDirectoryInformation) {
            PFILE_ID_FULL_DIR_INFORMATION id = (PFILE_ID_FULL_DIR_INFORMATION)Record;
            id->EaSize = 0U;
            id->FileId.QuadPart = Inode->number;
            ExtfsCopyMemory(id->FileName, name, nameBytes);
        } else if (InformationClass == FileBothDirectoryInformation) {
            PFILE_BOTH_DIR_INFORMATION both = (PFILE_BOTH_DIR_INFORMATION)Record;
            both->EaSize = 0U;
            both->ShortNameLength = 0;
            ExtfsCopyMemory(both->FileName, name, nameBytes);
        } else if (InformationClass == FileIdBothDirectoryInformation) {
            PFILE_ID_BOTH_DIR_INFORMATION both =
                (PFILE_ID_BOTH_DIR_INFORMATION)Record;
            both->EaSize = 0U;
            both->ShortNameLength = 0;
            both->FileId.QuadPart = Inode->number;
            ExtfsCopyMemory(both->FileName, name, nameBytes);
        } else {
            ExtfsCopyMemory(info->FileName, name, nameBytes);
        }
    }
    *RecordLength = base + nameBytes;
    return STATUS_SUCCESS;
}

/*
 * Windows directory enumeration is resumable state held in CCB.DirectoryIndex.
 * SL_RESTART_SCAN and SL_INDEX_SPECIFIED reset that cursor; the search pattern
 * persists on the handle.  Records are emitted in the caller's requested FILE_*
 * information class, 8-byte aligned and linked through NextEntryOffset.
 */
NTSTATUS ExtfsDispatchDirectoryControl(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);
    PEXTFS_FCB fcb = ExtfsFcbFromFile(stack->FileObject);
    PEXTFS_CCB ccb = ExtfsCcbFromFile(stack->FileObject);
    ULONG length = stack->Parameters.QueryDirectory.Length;
    FILE_INFORMATION_CLASS informationClass =
        stack->Parameters.QueryDirectory.FileInformationClass;
    EXTFS_OUTPUT_BUFFER output;
    PUCHAR buffer;
    PVOID scratch;
    ULONG used = 0U;
    ULONG previousOffset = 0U;
    NTSTATUS status;
    BOOLEAN returnSingle;
    BOOLEAN initial;
    BOOLEAN caseSensitive;
    (void)DeviceObject;
    if (stack->MinorFunction != IRP_MN_QUERY_DIRECTORY) {
        return ExtfsCompleteIrp(Irp, STATUS_NOT_SUPPORTED, 0U);
    }
    if (fcb != NULL && fcb->Vcb->Dismounted)
        return ExtfsCompleteIrp(Irp, STATUS_VOLUME_DISMOUNTED, 0U);
    if (fcb == NULL || ccb == NULL ||
        extfs_inode_type(&fcb->Inode) != EXTFS_NODE_DIRECTORY ||
        ExtfsDirectoryBaseLength(informationClass) == 0U) {
        return ExtfsCompleteIrp(Irp, STATUS_INVALID_INFO_CLASS, 0U);
    }
    if ((stack->Flags & SL_RESTART_SCAN) != 0U) ccb->DirectoryIndex = 0U;
    if ((stack->Flags & SL_INDEX_SPECIFIED) != 0U)
        ccb->DirectoryIndex = stack->Parameters.QueryDirectory.FileIndex;
    if (stack->Parameters.QueryDirectory.FileName != NULL) {
        PUNICODE_STRING pattern = stack->Parameters.QueryDirectory.FileName;
        ULONG chars = pattern->Length / sizeof(WCHAR);
        if ((pattern->Length & (sizeof(WCHAR) - 1U)) != 0U ||
            (pattern->Length != 0U && pattern->Buffer == NULL)) {
            return ExtfsCompleteIrp(Irp, STATUS_OBJECT_NAME_INVALID, 0U);
        }
        if (chars >= EXTFS_MAX_PATTERN_CHARS) {
            return ExtfsCompleteIrp(Irp, STATUS_NAME_TOO_LONG, 0U);
        }
        ccb->PatternLength = (USHORT)chars;
        ExtfsCopyMemory(ccb->Pattern, pattern->Buffer, pattern->Length);
    } else if (ccb->PatternLength == 0U) {
        ccb->Pattern[0] = L'*';
        ccb->PatternLength = 1U;
    }
    initial = ccb->DirectoryIndex == 0U;
    returnSingle = (stack->Flags & SL_RETURN_SINGLE_ENTRY) != 0U;
    caseSensitive = (stack->Flags & SL_CASE_SENSITIVE) != 0U;
    status = ExtfsLockOutputBuffer(Irp, length, &output);
    if (!NT_SUCCESS(status)) return ExtfsCompleteIrp(Irp, status, 0U);
    buffer = (PUCHAR)output.Address;
    scratch = ExtfsAllocate(fcb->Vcb->Volume.block_size);
    if (scratch == NULL) {
        ExtfsUnlockOutputBuffer(&output);
        return ExtfsCompleteIrp(Irp, STATUS_INSUFFICIENT_RESOURCES, 0U);
    }
    status = STATUS_SUCCESS;
    for (;;) {
        EXTFS_DIRENT_PICK pick;
        extfs_inode inode;
        extfs_status extStatus;
        ULONG recordLength;
        ULONG alignedLength;
        status = ExtfsFindDirectoryEntry(fcb, ccb, caseSensitive,
                                         &pick, scratch);
        if (!NT_SUCCESS(status)) {
            if (used != 0U) status = STATUS_SUCCESS;
            else if (initial && status == STATUS_NO_MORE_FILES)
                status = STATUS_NO_SUCH_FILE;
            break;
        }
        extStatus = extfs_read_inode(&fcb->Vcb->Volume, pick.InodeNumber,
                                     &inode, scratch,
                                     fcb->Vcb->Volume.block_size);
        if (extStatus != EXTFS_OK) {
            status = ExtfsStatusToNt(extStatus);
            break;
        }
        status = ExtfsFillDirectoryRecord(informationClass,
                                          buffer + used, length - used,
                                          fcb, &pick, &inode, &recordLength);
        if (status == STATUS_BUFFER_OVERFLOW) {
            if (used != 0U) status = STATUS_SUCCESS;
            break;
        }
        if (!NT_SUCCESS(status)) break;
        ccb->DirectoryIndex = pick.FoundIndex + 1U;
        if (used != 0U) {
            *(PULONG)(buffer + previousOffset) = used - previousOffset;
        }
        previousOffset = used;
        used += recordLength;
        if (returnSingle) break;
        alignedLength = (used + 7U) & ~7U;
        if (alignedLength > length ||
            length - alignedLength < ExtfsDirectoryBaseLength(informationClass))
            break;
        ExtfsZeroMemory(buffer + used, alignedLength - used);
        used = alignedLength;
    }
    ExFreePool(scratch);
    ExtfsUnlockOutputBuffer(&output);
    return ExtfsCompleteIrp(Irp, status,
                            NT_SUCCESS(status) || status == STATUS_BUFFER_OVERFLOW
                                ? used : 0U);
}

/*
 * Mount transaction: create a volume device/VCB, bind the lower disk reader,
 * parse ext metadata, apply the strict read policy and stricter write policy, then publish the VPB.
 * Until the VPB points at volumeDevice and DO_DEVICE_INITIALIZING is cleared,
 * failure simply deletes the new device and leaves the target unclaimed.
 */
static NTSTATUS ExtfsMountVolume(PIRP Irp)
{
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);
    PDEVICE_OBJECT target = stack->Parameters.MountVolume.DeviceObject;
    PVPB vpb = stack->Parameters.MountVolume.Vpb;
    PDEVICE_OBJECT volumeDevice = NULL;
    PEXTFS_VCB vcb;
    extfs_io io;
    extfs_status extStatus;
    extfs_u32 risks;
    NTSTATUS status;
    ULONG index;
    ULONG labelChars = 0U;

    if (target == NULL || vpb == NULL || vpb->RealDevice == NULL ||
        (vpb->RealDevice->DeviceType != FILE_DEVICE_DISK &&
         vpb->RealDevice->DeviceType != FILE_DEVICE_VIRTUAL_DISK)) {
        return STATUS_UNRECOGNIZED_VOLUME;
    }
    status = IoCreateDevice(ExtfsControlDevice->DriverObject,
                            sizeof(EXTFS_VCB), NULL,
                            FILE_DEVICE_DISK_FILE_SYSTEM,
                            0U, FALSE,
                            &volumeDevice);
    if (!NT_SUCCESS(status)) return status;
    vcb = (PEXTFS_VCB)volumeDevice->DeviceExtension;
    ExtfsZeroMemory(vcb, sizeof(*vcb));
    vcb->Signature = EXTFS_VCB_SIGNATURE;
    status = ExInitializeResourceLite(&vcb->FcbListResource);
    if (!NT_SUCCESS(status)) {
        IoDeleteDevice(volumeDevice);
        return status;
    }
    status = ExInitializeResourceLite(&vcb->WriteResource);
    if (!NT_SUCCESS(status)) {
        ExDeleteResourceLite(&vcb->FcbListResource);
        IoDeleteDevice(volumeDevice);
        return status;
    }
    status = ExInitializeResourceLite(&vcb->MetadataResource);
    if (!NT_SUCCESS(status)) {
        ExDeleteResourceLite(&vcb->WriteResource);
        ExDeleteResourceLite(&vcb->FcbListResource);
        IoDeleteDevice(volumeDevice);
        return status;
    }
    InitializeListHead(&vcb->FcbList);
    vcb->VolumeDeviceObject = volumeDevice;
    vcb->TargetDeviceObject = target;
    vcb->Vpb = vpb;
    vcb->Reader.DeviceObject = target;
    vcb->Reader.SectorSize = ExtfsQuerySectorSize(target);
    vcb->Reader.WriteResource = &vcb->WriteResource;
    io.read_at = ExtfsReadAt;
    io.write_at = ExtfsWriteAt;
    io.flush = ExtfsFlushCore;
    io.time_now = ExtfsTimeCore;
    io.user = &vcb->Reader;
    extStatus = extfs_open(&vcb->Volume, &io);
    if (extStatus != EXTFS_OK) {
        status = ExtfsStatusToNt(extStatus);
        if (status != STATUS_UNRECOGNIZED_VOLUME) status = STATUS_UNRECOGNIZED_VOLUME;
        ExDeleteResourceLite(&vcb->MetadataResource);
        ExDeleteResourceLite(&vcb->WriteResource);
        ExDeleteResourceLite(&vcb->FcbListResource);
        IoDeleteDevice(volumeDevice);
        return status;
    }
    extStatus = extfs_readonly_assess(&vcb->Volume, &risks);
    if (extStatus != EXTFS_OK) {
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_WARNING_LEVEL,
                   "ExtFS: volume refused by read policy (risks=0x%08x)\n",
                   risks);
        ExDeleteResourceLite(&vcb->MetadataResource);
        ExDeleteResourceLite(&vcb->WriteResource);
        ExDeleteResourceLite(&vcb->FcbListResource);
        IoDeleteDevice(volumeDevice);
        return STATUS_UNRECOGNIZED_VOLUME;
    }
    vcb->WriteEnabled =
        ExtfsDeviceIsWritable(target) &&
        extfs_write_assess(&vcb->Volume, &risks) == EXTFS_OK ? TRUE : FALSE;
    if (!vcb->WriteEnabled)
        volumeDevice->Characteristics |= FILE_READ_ONLY_DEVICE;
    volumeDevice->AlignmentRequirement = target->AlignmentRequirement;
    volumeDevice->StackSize = (CCHAR)(target->StackSize + 1);
    volumeDevice->Flags |= DO_DIRECT_IO;
    if ((target->Characteristics & FILE_REMOVABLE_MEDIA) != 0U)
        volumeDevice->Characteristics |= FILE_REMOVABLE_MEDIA;
    /*
     * 0.9.3 is deliberately resident for the entire boot: DriverEntry does not
     * publish DriverUnload.  That conservative policy avoids any possibility of
     * executable code disappearing while a mounted VCB/FCB is still reachable.
     */
    vpb->DeviceObject = volumeDevice;
    vpb->SerialNumber = 0U;
    for (index = 0U; index < 4U; ++index)
        vpb->SerialNumber |= (ULONG)vcb->Volume.uuid[index] << (index * 8U);
    status = ExtfsUtf8ToUnicode(vcb->Volume.label,
        ExtfsStringLength(vcb->Volume.label), vpb->VolumeLabel,
        sizeof(vpb->VolumeLabel) / sizeof(vpb->VolumeLabel[0]), &labelChars);
    if (!NT_SUCCESS(status)) labelChars = 0U;
    vpb->VolumeLabelLength = (USHORT)(labelChars * sizeof(WCHAR));
    volumeDevice->Flags &= ~DO_DEVICE_INITIALIZING;
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
               "ExtFS: mounted clean %s volume %s\n",
               extfs_kind_string(vcb->Volume.kind),
               vcb->WriteEnabled ? "limited-write" : "read-only");
    return STATUS_SUCCESS;
}

static NTSTATUS ExtfsVerifyMountedVolume(PEXTFS_VCB Vcb)
{
    extfs_volume observed;
    extfs_io io;
    extfs_u32 risks;
    extfs_status extStatus;

    io.read_at = ExtfsReadAt;
    io.write_at = ExtfsWriteAt;
    io.flush = ExtfsFlushCore;
    io.time_now = ExtfsTimeCore;
    io.user = &Vcb->Reader;
    extStatus = extfs_open(&observed, &io);
    if (extStatus != EXTFS_OK ||
        extfs_readonly_assess(&observed, &risks) != EXTFS_OK) {
        return STATUS_WRONG_VOLUME;
    }
    if (!ExtfsBytesEqual(observed.uuid, Vcb->Volume.uuid, 16U) ||
        observed.state != Vcb->Volume.state ||
        observed.total_blocks != Vcb->Volume.total_blocks ||
        observed.total_inodes != Vcb->Volume.total_inodes ||
        observed.block_size != Vcb->Volume.block_size ||
        observed.blocks_per_group != Vcb->Volume.blocks_per_group ||
        observed.inodes_per_group != Vcb->Volume.inodes_per_group ||
        observed.inode_size != Vcb->Volume.inode_size ||
        observed.descriptor_size != Vcb->Volume.descriptor_size ||
        observed.feature_compat != Vcb->Volume.feature_compat ||
        observed.feature_incompat != Vcb->Volume.feature_incompat ||
        observed.feature_ro_compat != Vcb->Volume.feature_ro_compat ||
        observed.journal_inode != Vcb->Volume.journal_inode ||
        !ExtfsBytesEqual(observed.journal_uuid, Vcb->Volume.journal_uuid, 16U)) {
        return STATUS_WRONG_VOLUME;
    }
    /* Existing FCBs contain decoded inode state.  A removable medium may have
     * been modified while absent even if its UUID and geometry are unchanged.
     * Do not validate such handles against stale inode snapshots; only refresh
     * the volume after all opens have drained, then discard the inert FCB cache. */
    {
        LIST_ENTRY reapList;
        InitializeListHead(&reapList);
        ExtfsAcquireFcbList(Vcb);
        if (Vcb->OpenHandleCount != 0 || Vcb->FileObjectCount != 0) {
            ExtfsReleaseFcbList(Vcb);
            return STATUS_WRONG_VOLUME;
        }

        /*
         * A zero FILE_OBJECT count is not sufficient: mapped sections can
         * outlive their originating handle. Detach only FCBs for which Memory
         * Manager confirms that data/image sections are closed.
         */
        ExtfsCollectReapableFcbsLocked(Vcb, &reapList, FALSE);
        if (!IsListEmpty(&Vcb->FcbList)) {
            ExtfsReleaseFcbList(Vcb);
            ExtfsDestroyFcbList(&reapList);
            return STATUS_WRONG_VOLUME;
        }

        Vcb->Volume = observed;
        Vcb->WriteEnabled =
            ExtfsDeviceIsWritable(Vcb->TargetDeviceObject) &&
            extfs_write_assess(&Vcb->Volume, &risks) == EXTFS_OK ? TRUE : FALSE;
        if (Vcb->VolumeDeviceObject != NULL) {
            if (Vcb->WriteEnabled)
                Vcb->VolumeDeviceObject->Characteristics &= ~FILE_READ_ONLY_DEVICE;
            else
                Vcb->VolumeDeviceObject->Characteristics |= FILE_READ_ONLY_DEVICE;
        }
        ExtfsReleaseFcbList(Vcb);
        ExtfsDestroyFcbList(&reapList);
    }
    Vcb->Vpb->RealDevice->Flags &= ~DO_VERIFY_VOLUME;
    return STATUS_SUCCESS;
}

static NTSTATUS ExtfsLockMountedVolume(PEXTFS_VCB Vcb, PFILE_OBJECT FileObject)
{
    PEXTFS_FCB ownerFcb = ExtfsFcbFromFile(FileObject);
    LIST_ENTRY reapList;
    PLIST_ENTRY entry;
    NTSTATUS status = STATUS_ACCESS_DENIED;
    KIRQL savedIrql;

    if (ownerFcb == NULL || ownerFcb->Vcb != Vcb || !ownerFcb->VolumeOpen)
        return STATUS_INVALID_PARAMETER;

    InitializeListHead(&reapList);
    ExtfsAcquireFcbList(Vcb);
    ExtfsCollectReapableFcbsLocked(Vcb, &reapList, FALSE);
    if (Vcb->Dismounted) {
        status = STATUS_VOLUME_DISMOUNTED;
        goto ExitLocked;
    }
    if (Vcb->VolumeLockFileObject != NULL ||
        Vcb->OpenHandleCount != 1 || Vcb->FileObjectCount != 1)
        goto ExitLocked;

    for (entry = Vcb->FcbList.Flink; entry != &Vcb->FcbList; entry = entry->Flink) {
        PEXTFS_FCB fcb = CONTAINING_RECORD(entry, EXTFS_FCB, Links);
        if (fcb != ownerFcb) goto ExitLocked;
    }

    IoAcquireVpbSpinLock(&savedIrql);
    if ((Vcb->Vpb->Flags & VPB_LOCKED) == 0U) {
        Vcb->Vpb->Flags |= VPB_LOCKED;
        Vcb->VolumeLockFileObject = FileObject;
        status = STATUS_SUCCESS;
    }
    IoReleaseVpbSpinLock(savedIrql);

ExitLocked:
    ExtfsReleaseFcbList(Vcb);
    ExtfsDestroyFcbList(&reapList);

    if (NT_SUCCESS(status) && Vcb->WriteEnabled) {
        NTSTATUS flushStatus = ExtfsFlushLowerDevice(&Vcb->Reader);
        if (!NT_SUCCESS(flushStatus)) {
            ExtfsAcquireFcbList(Vcb);
            if (Vcb->VolumeLockFileObject == FileObject) {
                IoAcquireVpbSpinLock(&savedIrql);
                Vcb->Vpb->Flags &= (USHORT)~VPB_LOCKED;
                IoReleaseVpbSpinLock(savedIrql);
                Vcb->VolumeLockFileObject = NULL;
            }
            ExtfsReleaseFcbList(Vcb);
            status = flushStatus;
        }
    }
    return status;
}

static NTSTATUS ExtfsUnlockMountedVolume(PEXTFS_VCB Vcb, PFILE_OBJECT FileObject)
{
    PEXTFS_FCB ownerFcb = ExtfsFcbFromFile(FileObject);
    NTSTATUS status = STATUS_NOT_LOCKED;
    KIRQL savedIrql;

    if (ownerFcb == NULL || ownerFcb->Vcb != Vcb || !ownerFcb->VolumeOpen)
        return STATUS_INVALID_PARAMETER;

    ExtfsAcquireFcbList(Vcb);
    if (Vcb->VolumeLockFileObject == FileObject) {
        IoAcquireVpbSpinLock(&savedIrql);
        Vcb->Vpb->Flags &= (USHORT)~VPB_LOCKED;
        IoReleaseVpbSpinLock(savedIrql);
        Vcb->VolumeLockFileObject = NULL;
        status = STATUS_SUCCESS;
    }
    ExtfsReleaseFcbList(Vcb);
    return status;
}

static NTSTATUS ExtfsDismountLockedVolume(PEXTFS_VCB Vcb, PFILE_OBJECT FileObject)
{
    PEXTFS_FCB ownerFcb = ExtfsFcbFromFile(FileObject);
    NTSTATUS status;
    KIRQL savedIrql;

    if (ownerFcb == NULL || ownerFcb->Vcb != Vcb || !ownerFcb->VolumeOpen)
        return STATUS_INVALID_PARAMETER;

    ExtfsAcquireFcbList(Vcb);
    if (Vcb->VolumeLockFileObject != FileObject) {
        ExtfsReleaseFcbList(Vcb);
        return STATUS_ACCESS_DENIED;
    }
    if (Vcb->Dismounted) {
        ExtfsReleaseFcbList(Vcb);
        return STATUS_VOLUME_DISMOUNTED;
    }
    ExtfsReleaseFcbList(Vcb);

    status = Vcb->WriteEnabled ? ExtfsFlushLowerDevice(&Vcb->Reader) : STATUS_SUCCESS;
    if (!NT_SUCCESS(status)) return status;

    ExtfsAcquireFcbList(Vcb);
    if (Vcb->VolumeLockFileObject != FileObject) {
        ExtfsReleaseFcbList(Vcb);
        return STATUS_ACCESS_DENIED;
    }
    Vcb->Dismounted = TRUE;
    IoAcquireVpbSpinLock(&savedIrql);
    Vcb->Vpb->Flags &= (USHORT)~VPB_MOUNTED;
    IoReleaseVpbSpinLock(savedIrql);
    ExtfsReleaseFcbList(Vcb);
    return STATUS_SUCCESS;
}

NTSTATUS ExtfsDispatchFileSystemControl(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);
    PEXTFS_VCB vcb = ExtfsVcbFromDevice(DeviceObject);
    NTSTATUS status;
    ULONG_PTR information = 0U;
    if (stack->MinorFunction == IRP_MN_MOUNT_VOLUME &&
        DeviceObject == ExtfsControlDevice) {
        status = ExtfsMountVolume(Irp);
        return ExtfsCompleteIrp(Irp, status, 0U);
    }
    if (vcb == NULL) return ExtfsCompleteIrp(Irp,
                                             STATUS_INVALID_DEVICE_REQUEST, 0U);
    if (stack->MinorFunction == IRP_MN_VERIFY_VOLUME) {
        status = ExtfsVerifyMountedVolume(vcb);
        return ExtfsCompleteIrp(Irp, status, 0U);
    }
    if (stack->MinorFunction != IRP_MN_USER_FS_REQUEST) {
        return ExtfsCompleteIrp(Irp, STATUS_INVALID_DEVICE_REQUEST, 0U);
    }
    switch (stack->Parameters.FileSystemControl.FsControlCode) {
        case FSCTL_IS_VOLUME_MOUNTED:
        case FSCTL_ALLOW_EXTENDED_DASD_IO:
            status = vcb->Dismounted ? STATUS_VOLUME_DISMOUNTED : STATUS_SUCCESS;
            break;
        case FSCTL_IS_VOLUME_DIRTY:
            if (Irp->AssociatedIrp.SystemBuffer != NULL &&
                stack->Parameters.FileSystemControl.OutputBufferLength >=
                    sizeof(ULONG)) {
                *(PULONG)Irp->AssociatedIrp.SystemBuffer =
                    (vcb->Volume.state & 0x0001U) != 0U ? 0U : VOLUME_IS_DIRTY;
                information = sizeof(ULONG);
                status = STATUS_SUCCESS;
            } else {
                status = STATUS_BUFFER_TOO_SMALL;
            }
            break;
        case FSCTL_LOCK_VOLUME:
            status = ExtfsLockMountedVolume(vcb, stack->FileObject);
            break;
        case FSCTL_UNLOCK_VOLUME:
            status = ExtfsUnlockMountedVolume(vcb, stack->FileObject);
            break;
        case FSCTL_DISMOUNT_VOLUME:
            status = ExtfsDismountLockedVolume(vcb, stack->FileObject);
            break;
        default:
            status = STATUS_NOT_SUPPORTED;
            break;
    }
    return ExtfsCompleteIrp(Irp, status, information);
}

NTSTATUS ExtfsDispatchDeviceControl(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PEXTFS_VCB vcb = ExtfsVcbFromDevice(DeviceObject);
    if (vcb == NULL) return ExtfsCompleteIrp(Irp,
                                             STATUS_INVALID_DEVICE_REQUEST, 0U);
    if (vcb->Dismounted)
        return ExtfsCompleteIrp(Irp, STATUS_VOLUME_DISMOUNTED, 0U);
    IoSkipCurrentIrpStackLocation(Irp);
    return IoCallDriver(vcb->TargetDeviceObject, Irp);
}

NTSTATUS ExtfsDispatchReadOnly(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    (void)DeviceObject;
    return ExtfsCompleteIrp(Irp, STATUS_MEDIA_WRITE_PROTECTED, 0U);
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    UNICODE_STRING deviceName;
    NTSTATUS status;
    (void)RegistryPath;
    (void)ExtfsRelocationAnchor;
    ExtfsControlDevice = NULL;
    /* Leave unimplemented MajorFunction slots at the I/O manager's default
     * IopInvalidDeviceRequest handler; install only the IRPs ExtFS supports. */
    /*
     * Data reads/writes and the ext2 direct-file EOF mutation receive specialised
     * handlers. Other metadata-mutating requests remain write-protected until
     * inode/directory allocation and journal transactions are available.
     */
    DriverObject->MajorFunction[IRP_MJ_CREATE] = ExtfsDispatchCreate;
    DriverObject->MajorFunction[IRP_MJ_CLOSE] = ExtfsDispatchClose;
    DriverObject->MajorFunction[IRP_MJ_CLEANUP] = ExtfsDispatchCleanup;
    DriverObject->MajorFunction[IRP_MJ_READ] = ExtfsDispatchRead;
    DriverObject->MajorFunction[IRP_MJ_WRITE] = ExtfsDispatchWrite;
    DriverObject->MajorFunction[IRP_MJ_QUERY_INFORMATION] =
        ExtfsDispatchQueryInformation;
    DriverObject->MajorFunction[IRP_MJ_SET_INFORMATION] = ExtfsDispatchSetInformation;
    DriverObject->MajorFunction[IRP_MJ_QUERY_VOLUME_INFORMATION] =
        ExtfsDispatchQueryVolumeInformation;
    DriverObject->MajorFunction[IRP_MJ_SET_VOLUME_INFORMATION] =
        ExtfsDispatchReadOnly;
    DriverObject->MajorFunction[IRP_MJ_DIRECTORY_CONTROL] =
        ExtfsDispatchDirectoryControl;
    DriverObject->MajorFunction[IRP_MJ_FILE_SYSTEM_CONTROL] =
        ExtfsDispatchFileSystemControl;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] =
        ExtfsDispatchDeviceControl;
    DriverObject->MajorFunction[IRP_MJ_FLUSH_BUFFERS] =
        ExtfsDispatchFlushBuffers;
    RtlInitUnicodeString(&deviceName, L"\\FileSystem\\ExtFS");
    status = IoCreateDevice(DriverObject, 0U, &deviceName,
                            FILE_DEVICE_DISK_FILE_SYSTEM,
                            FILE_DEVICE_SECURE_OPEN, FALSE,
                            &ExtfsControlDevice);
    if (!NT_SUCCESS(status)) return status;
#ifdef DO_LOW_PRIORITY_FILESYSTEM
    ExtfsControlDevice->Flags |= DO_LOW_PRIORITY_FILESYSTEM;
#endif
    ExtfsControlDevice->Flags &= ~DO_DEVICE_INITIALIZING;
    IoRegisterFileSystem(ExtfsControlDevice);
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
               "ExtFS: experimental ext4-depth1-extent-resize IFS 0.9.3 loaded\n");
    return STATUS_SUCCESS;
}
