// SPDX-License-Identifier: GPL-3.0-or-later
/* Windows directory enumeration and directory-control dispatch.
 *
 * Included by ext2_driver.c so the Windows adapter remains one WDK translation
 * unit while its source is cut by responsibility. Filesystem semantics stay
 * in ../core; this fragment contains Windows integration only.
 */

static int ExtfsPickDirectoryEntry(void *User, ifs_ext2_u32 InodeNumber,
                                   IfsExt2NodeType Type, const char *Name,
                                   ifs_ext2_u8 NameLength)
{
    PEXTFS_DIRENT_PICK pick = (PEXTFS_DIRENT_PICK)User;
    WCHAR unicode[IFS_EXT2_MAX_NAME_LENGTH];
    ULONG unicodeLength = 0U;
    NTSTATUS status;
    if ((NameLength == 1U && Name[0] == '.') ||
        (NameLength == 2U && Name[0] == '.' && Name[1] == '.')) {
        return 0;
    }
    if (pick->CurrentIndex++ < pick->StartIndex) return 0;
    status = ExtfsUtf8ToUnicode(Name, NameLength, unicode,
                                IFS_EXT2_MAX_NAME_LENGTH, &unicodeLength);
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
    IfsExt2Status status;
    ExtfsZeroMemory(Pick, sizeof(*Pick));
    Pick->StartIndex = Ccb->DirectoryIndex;
    Pick->Pattern = Ccb->Pattern;
    Pick->PatternLength = Ccb->PatternLength;
    Pick->CaseSensitive = CaseSensitive;
    status = ifs_ext2_iterate_directory(&Fcb->Vcb->Volume, &Fcb->Inode,
                                     ExtfsPickDirectoryEntry, Pick,
                                     Scratch, Fcb->Vcb->Volume.block_size);
    if (status != IFS_EXT2_OK && status != IFS_EXT2_STOP)
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
                                         const IfsExt2Inode *Inode,
                                         PULONG RecordLength)
{
    WCHAR name[IFS_EXT2_MAX_NAME_LENGTH];
    ULONG nameChars = 0U;
    ULONG nameBytes;
    ULONG base = ExtfsDirectoryBaseLength(InformationClass);
    NTSTATUS status;
    FILE_STANDARD_INFORMATION standard;
    ULONG attributes = ExtfsFileAttributes(Directory->Vcb, Inode);
    if (base == 0U) return STATUS_INVALID_INFO_CLASS;
    status = ExtfsUtf8ToUnicode(Pick->Utf8Name, Pick->Utf8Length,
                                name, IFS_EXT2_MAX_NAME_LENGTH, &nameChars);
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
        ifs_ext2_inode_type(&fcb->Inode) != IFS_EXT2_NODE_DIRECTORY ||
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
        IfsExt2Inode inode;
        IfsExt2Status extStatus;
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
        extStatus = ifs_ext2_read_inode(&fcb->Vcb->Volume, pick.InodeNumber,
                                     &inode, scratch,
                                     fcb->Vcb->Volume.block_size);
        if (extStatus != IFS_EXT2_OK) {
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

