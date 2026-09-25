// SPDX-License-Identifier: GPL-3.0-or-later
/* Windows create/cleanup/close/read/write and file-information dispatch.
 *
 * Included by ext2_driver.c so the Windows adapter remains one WDK translation
 * unit while its source is cut by responsibility. Filesystem semantics stay
 * in ../core; this fragment contains Windows integration only.
 */

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
    IfsExt2Inode resolved;
    IfsExt2NodeType nodeType;
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
        nodeType = ifs_ext2_inode_type(&resolved);
        if (nodeType != IFS_EXT2_NODE_REGULAR &&
            nodeType != IFS_EXT2_NODE_DIRECTORY &&
            nodeType != IFS_EXT2_NODE_SYMLINK) {
            status = STATUS_NOT_SUPPORTED;
            goto Exit;
        }
        if (resolved.size > 0x7FFFFFFFFFFFFFFFULL) {
            status = STATUS_FILE_TOO_LARGE;
            goto Exit;
        }
        if ((options & FILE_DIRECTORY_FILE) != 0U &&
            nodeType != IFS_EXT2_NODE_DIRECTORY) {
            status = STATUS_NOT_A_DIRECTORY;
            goto Exit;
        }
        if ((options & FILE_NON_DIRECTORY_FILE) != 0U &&
            nodeType == IFS_EXT2_NODE_DIRECTORY) {
            status = STATUS_FILE_IS_A_DIRECTORY;
            goto Exit;
        }
        if ((access & (FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_EA |
                       FILE_WRITE_ATTRIBUTES)) != 0U &&
            ifs_ext2_inode_write_assess(&vcb->Volume, &resolved) != IFS_EXT2_OK) {
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
                       "Filesystem Support EXT2: CLOSE observed without completed CLEANUP\n");
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
    ifs_ext2_u32 bytesRead = 0U;
    IfsExt2Status extStatus;
    NTSTATUS status;
    (void)DeviceObject;
    if (fcb == NULL || fcb->VolumeOpen) {
        return ExtfsCompleteIrp(Irp, STATUS_INVALID_DEVICE_REQUEST, 0U);
    }
    if (fcb->Vcb->Dismounted)
        return ExtfsCompleteIrp(Irp, STATUS_VOLUME_DISMOUNTED, 0U);
    if (ifs_ext2_inode_type(&fcb->Inode) == IFS_EXT2_NODE_DIRECTORY) {
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
    extStatus = ifs_ext2_read_file(&fcb->Vcb->Volume, &fcb->Inode,
                                (ifs_ext2_u64)offset, output.Address, length,
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
static IfsExt2Status ExtfsResizeRegularFile(PEXTFS_FCB Fcb,
                                           ifs_ext2_u64 NewSize,
                                           PVOID Scratch,
                                           ifs_ext2_u32 ScratchSize)
{
    (void)Fcb;
    (void)NewSize;
    (void)Scratch;
    (void)ScratchSize;
    return IFS_EXT2_ERROR_UNSUPPORTED;
}

static VOID ExtfsDisableWritesIfUnsafe(PEXTFS_VCB Vcb)
{
    ifs_ext2_u32 risks = 0U;
    if (Vcb == NULL) return;
    if (ifs_ext2_write_assess(&Vcb->Volume, &risks) != IFS_EXT2_OK) {
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
    ifs_ext2_u32 bytesWritten = 0U;
    IfsExt2Status extStatus;
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
    if (ifs_ext2_inode_type(&fcb->Inode) != IFS_EXT2_NODE_REGULAR)
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
            fcb, (ifs_ext2_u64)endOffset, scratch,
            fcb->Vcb->Volume.block_size * 8U);
        ExtfsReleaseMetadata(fcb->Vcb);
        metadataLocked = FALSE;
        if (extStatus != IFS_EXT2_OK) {
            ExtfsDisableWritesIfUnsafe(fcb->Vcb);
            ExtfsReleaseFileData(fcb);
            ExFreePool(scratch);
            ExtfsUnlockOutputBuffer(&input);
            status = ExtfsStatusToNt(extStatus);
            return ExtfsCompleteIrp(Irp, status, 0U);
        }
        ExtfsSyncFcbHeaderSizes(fcb);
    }

    extStatus = ifs_ext2_write_file_existing(&fcb->Vcb->Volume, &fcb->Inode,
                                          (ifs_ext2_u64)offset, input.Address,
                                          length, scratch,
                                          fcb->Vcb->Volume.block_size * 2U,
                                          &bytesWritten);
    if (metadataLocked) ExtfsReleaseMetadata(fcb->Vcb);
    ExtfsReleaseFileData(fcb);
    ExFreePool(scratch);
    ExtfsUnlockOutputBuffer(&input);
    status = ExtfsStatusToNt(extStatus);
    if (extStatus == IFS_EXT2_ERROR_UNSUPPORTED) status = STATUS_NOT_SUPPORTED;
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
    IfsExt2Status extStatus;
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
    if (ifs_ext2_inode_type(&fcb->Inode) != IFS_EXT2_NODE_REGULAR)
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
        fcb, (ifs_ext2_u64)requestedSize, scratch,
        fcb->Vcb->Volume.block_size * 8U);
    ExtfsReleaseMetadata(fcb->Vcb);
    if (extStatus == IFS_EXT2_OK) ExtfsSyncFcbHeaderSizes(fcb);
    ExtfsReleaseFileData(fcb);
    ExFreePool(scratch);

    if (extStatus != IFS_EXT2_OK) ExtfsDisableWritesIfUnsafe(fcb->Vcb);
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
            info->MaximumComponentNameLength = IFS_EXT2_MAX_NAME_LENGTH;
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

