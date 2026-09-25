// SPDX-License-Identifier: GPL-3.0-or-later
/* Windows Unicode/path conversion, information helpers and IRP buffers.
 *
 * Included by ext2_driver.c so the Windows adapter remains one WDK translation
 * unit while its source is cut by responsibility. Filesystem semantics stay
 * in ../core; this fragment contains Windows integration only.
 */

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
    WCHAR Name[IFS_EXT2_MAX_NAME_LENGTH];
    ULONG Length;
    ULONG InodeNumber;
    ULONG Matches;
} EXTFS_CASE_LOOKUP, *PEXTFS_CASE_LOOKUP;

static int ExtfsCaseLookupCallback(void *User, ifs_ext2_u32 InodeNumber,
                                   IfsExt2NodeType Type, const char *Name,
                                   ifs_ext2_u8 NameLength)
{
    PEXTFS_CASE_LOOKUP context = (PEXTFS_CASE_LOOKUP)User;
    WCHAR unicode[IFS_EXT2_MAX_NAME_LENGTH];
    ULONG unicodeLength = 0U;
    ULONG index;
    NTSTATUS status;
    (void)Type;

    status = ExtfsUtf8ToUnicode(Name, NameLength, unicode,
                                IFS_EXT2_MAX_NAME_LENGTH, &unicodeLength);
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
static IfsExt2Status ExtfsLookupComponent(PEXTFS_VCB Vcb,
                                         const IfsExt2Inode *Directory,
                                         const CHAR *Name, UCHAR NameLength,
                                         BOOLEAN CaseSensitive,
                                         PULONG InodeNumber,
                                         PVOID Scratch)
{
    ifs_ext2_u32 inodeNumber = 0U;
    IfsExt2Status status = ifs_ext2_lookup(&Vcb->Volume, Directory, Name,
                                       NameLength, &inodeNumber,
                                       Scratch, Vcb->Volume.block_size);
    EXTFS_CASE_LOOKUP context;
    if (status != IFS_EXT2_ERROR_NOT_FOUND || CaseSensitive) {
        if (status == IFS_EXT2_OK) *InodeNumber = (ULONG)inodeNumber;
        return status;
    }
    context.InodeNumber = 0U;
    context.Matches = 0U;
    {
        ULONG unicodeLength = 0U;
        NTSTATUS ntStatus = ExtfsUtf8ToUnicode(Name, NameLength, context.Name,
                                                IFS_EXT2_MAX_NAME_LENGTH,
                                                &unicodeLength);
        if (!NT_SUCCESS(ntStatus)) return IFS_EXT2_ERROR_NOT_FOUND;
        context.Length = unicodeLength;
    }
    status = ifs_ext2_iterate_directory(&Vcb->Volume, Directory,
                                     ExtfsCaseLookupCallback, &context,
                                     Scratch, Vcb->Volume.block_size);
    if (status != IFS_EXT2_OK && status != IFS_EXT2_STOP) return status;
    if (context.Matches != 1U) return IFS_EXT2_ERROR_NOT_FOUND;
    *InodeNumber = (ULONG)context.InodeNumber;
    return IFS_EXT2_OK;
}

/*
 * Resolve a FILE_OBJECT name either from the volume root or relative to the
 * related FILE_OBJECT supplied by the I/O manager.  A zero-length name on the
 * filesystem device represents a volume open rather than inode 2.
 */
static NTSTATUS ExtfsResolveFileName(PEXTFS_VCB Vcb,
                                     PFILE_OBJECT FileObject,
                                     BOOLEAN CaseSensitive,
                                     IfsExt2Inode *Inode,
                                     PVOID Scratch,
                                     PBOOLEAN VolumeOpen)
{
    const WCHAR *cursor;
    ULONG characters;
    IfsExt2Status extStatus;
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
        extStatus = ifs_ext2_read_inode(&Vcb->Volume, IFS_EXT2_ROOT_INODE, Inode,
                                     Scratch, Vcb->Volume.block_size);
        if (extStatus != IFS_EXT2_OK) return ExtfsStatusToNt(extStatus);
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
        CHAR utf8[IFS_EXT2_MAX_NAME_LENGTH];
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
        if (ifs_ext2_inode_type(Inode) != IFS_EXT2_NODE_DIRECTORY) {
            return STATUS_NOT_A_DIRECTORY;
        }
        status = ExtfsUnicodeToUtf8(start, componentChars, utf8,
                                    sizeof(utf8), &utf8Length);
        if (!NT_SUCCESS(status)) return status;
        extStatus = ExtfsLookupComponent(Vcb, Inode, utf8, utf8Length,
                                         CaseSensitive, &nextInode, Scratch);
        if (extStatus != IFS_EXT2_OK) return ExtfsStatusToNt(extStatus);
        extStatus = ifs_ext2_read_inode(&Vcb->Volume, nextInode, Inode,
                                     Scratch, Vcb->Volume.block_size);
        if (extStatus != IFS_EXT2_OK) return ExtfsStatusToNt(extStatus);
        cursor += componentChars;
        characters -= componentChars;
        while (characters != 0U && (*cursor == L'\\' || *cursor == L'/')) {
            ++cursor;
            --characters;
        }
    }
    return STATUS_SUCCESS;
}

static ULONG ExtfsFileAttributes(PEXTFS_VCB Vcb, const IfsExt2Inode *Inode)
{
    ULONG attributes = 0U;
    if (ifs_ext2_inode_type(Inode) == IFS_EXT2_NODE_DIRECTORY)
        attributes |= FILE_ATTRIBUTE_DIRECTORY;
    if (Vcb == NULL || !Vcb->WriteEnabled ||
        ifs_ext2_inode_write_assess(&Vcb->Volume, Inode) != IFS_EXT2_OK)
        attributes |= FILE_ATTRIBUTE_READONLY;
    if (attributes == 0U) attributes = FILE_ATTRIBUTE_NORMAL;
    return attributes;
}

static LARGE_INTEGER ExtfsNtTime(long long UnixSeconds, ULONG Nanoseconds)
{
    LARGE_INTEGER value;
    const long long epoch = 11644473600LL;
    long long shifted;
    ULONGLONG ticks;
    if (Nanoseconds >= 1000000000U || UnixSeconds < -epoch) {
        value.QuadPart = 0;
        return value;
    }
    shifted = UnixSeconds + epoch;
    if ((ifs_ext2_u64)shifted >
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
                                      const IfsExt2Inode *Inode,
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
                                         const IfsExt2Inode *Inode,
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
    Buffer->Directory = ifs_ext2_inode_type(Inode) == IFS_EXT2_NODE_DIRECTORY;
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

