// SPDX-License-Identifier: GPL-3.0-or-later
/* Windows mount/verify/lock/dismount and filesystem/device control.
 *
 * Included by ext2_driver.c so the Windows adapter remains one WDK translation
 * unit while its source is cut by responsibility. Filesystem semantics stay
 * in ../core; this fragment contains Windows integration only.
 */

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
    IfsExt2Io io;
    IfsExt2Status extStatus;
    ifs_ext2_u32 risks;
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
        io.user = &vcb->Reader;
    extStatus = ifs_ext2_open(&vcb->Volume, &io);
    if (extStatus != IFS_EXT2_OK) {
        status = ExtfsStatusToNt(extStatus);
        if (status != STATUS_UNRECOGNIZED_VOLUME) status = STATUS_UNRECOGNIZED_VOLUME;
        ExDeleteResourceLite(&vcb->MetadataResource);
        ExDeleteResourceLite(&vcb->WriteResource);
        ExDeleteResourceLite(&vcb->FcbListResource);
        IoDeleteDevice(volumeDevice);
        return status;
    }
    extStatus = ifs_ext2_readonly_assess(&vcb->Volume, &risks);
    if (extStatus != IFS_EXT2_OK) {
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_WARNING_LEVEL,
                   "Filesystem Support EXT2: volume refused by read policy (risks=0x%08x)\n",
                   risks);
        ExDeleteResourceLite(&vcb->MetadataResource);
        ExDeleteResourceLite(&vcb->WriteResource);
        ExDeleteResourceLite(&vcb->FcbListResource);
        IoDeleteDevice(volumeDevice);
        return STATUS_UNRECOGNIZED_VOLUME;
    }
    vcb->WriteEnabled =
        ExtfsDeviceIsWritable(target) &&
        ifs_ext2_write_assess(&vcb->Volume, &risks) == IFS_EXT2_OK ? TRUE : FALSE;
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
               "Filesystem Support EXT2: mounted clean volume %s\n",
               vcb->WriteEnabled ? "limited-write" : "read-only");
    return STATUS_SUCCESS;
}

static NTSTATUS ExtfsVerifyMountedVolume(PEXTFS_VCB Vcb)
{
    IfsExt2Volume observed;
    IfsExt2Io io;
    ifs_ext2_u32 risks;
    IfsExt2Status extStatus;

    io.read_at = ExtfsReadAt;
    io.write_at = ExtfsWriteAt;
    io.flush = ExtfsFlushCore;
        io.user = &Vcb->Reader;
    extStatus = ifs_ext2_open(&observed, &io);
    if (extStatus != IFS_EXT2_OK ||
        ifs_ext2_readonly_assess(&observed, &risks) != IFS_EXT2_OK) {
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
        observed.feature_ro_compat != Vcb->Volume.feature_ro_compat) {
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
            ifs_ext2_write_assess(&Vcb->Volume, &risks) == IFS_EXT2_OK ? TRUE : FALSE;
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

