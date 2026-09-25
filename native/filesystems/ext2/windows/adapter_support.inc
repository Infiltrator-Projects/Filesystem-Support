// SPDX-License-Identifier: GPL-3.0-or-later
/* Windows object lifetime, synchronization and block-I/O bridge.
 *
 * Included by ext2_driver.c so the Windows adapter remains one WDK translation
 * unit while its source is cut by responsibility. Filesystem semantics stay
 * in ../core; this fragment contains Windows integration only.
 */

/*
 * Filesystem Support EXT2 is a conservative synchronous native IFS adapter.
 * All EXT2 on-disk decisions are delegated to the canonical shared engine;
 * this file owns only Windows IRPs, object lifetime, naming, synchronization
 * and Windows information records. Unsupported metadata mutation remains
 * fail-closed until the canonical engine exposes and qualifies it.
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
    IfsExt2NodeType Type;
    UCHAR Utf8Length;
    CHAR Utf8Name[IFS_EXT2_MAX_NAME_LENGTH + 1U];
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

static NTSTATUS ExtfsStatusToNt(IfsExt2Status Status)
{
    switch (Status) {
        case IFS_EXT2_OK: return STATUS_SUCCESS;
        case IFS_EXT2_ERROR_MAGIC: return STATUS_UNRECOGNIZED_VOLUME;
        case IFS_EXT2_ERROR_NOT_FOUND: return STATUS_OBJECT_NAME_NOT_FOUND;
        case IFS_EXT2_ERROR_NOT_DIRECTORY: return STATUS_NOT_A_DIRECTORY;
        case IFS_EXT2_ERROR_IS_DIRECTORY: return STATUS_FILE_IS_A_DIRECTORY;
        case IFS_EXT2_ERROR_BUFFER_TOO_SMALL: return STATUS_BUFFER_TOO_SMALL;
        case IFS_EXT2_ERROR_UNSUPPORTED: return STATUS_NOT_SUPPORTED;
        case IFS_EXT2_ERROR_RANGE: return STATUS_INVALID_PARAMETER;
        case IFS_EXT2_ERROR_IO: return STATUS_IO_DEVICE_ERROR;
        case IFS_EXT2_ERROR_NO_SPACE: return STATUS_DISK_FULL;
        case IFS_EXT2_ERROR_CORRUPT: return STATUS_FILE_CORRUPT_ERROR;
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
                                     const IfsExt2Inode *Inode,
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
                                const IfsExt2Inode *Inode,
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
    if ((candidate == NULL))
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

    if (Fcb == NULL || Fcb->Vcb == NULL) return;
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
static int ExtfsReadAt(void *User, ifs_ext2_u64 ByteOffset,
                       void *Destination, ifs_ext2_u32 ByteCount)
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
static int ExtfsWriteAt(void *User, ifs_ext2_u64 ByteOffset,
                        const void *Source, ifs_ext2_u32 ByteCount)
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

