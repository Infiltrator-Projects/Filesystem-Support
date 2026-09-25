// SPDX-License-Identifier: GPL-3.0-or-later
/* Windows DriverEntry and major-function registration.
 *
 * Included by ext2_driver.c so the Windows adapter remains one WDK translation
 * unit while its source is cut by responsibility. Filesystem semantics stay
 * in ../core; this fragment contains Windows integration only.
 */

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
               "Filesystem Support EXT2: experimental ext4-depth1-extent-resize IFS 0.9.3 loaded\n");
    return STATUS_SUCCESS;
}

