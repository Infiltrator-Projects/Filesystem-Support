// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef EXTFS_EXTFS_H
#define EXTFS_EXTFS_H

/*
 * ExtFS portable ext2/ext3/ext4 core.
 *
 * This public interface deliberately uses no operating-system headers and
 * performs no heap allocation. A host supplies byte reads and may additionally
 * supply writes, durability barriers and wall-clock time; callers provide
 * scratch buffers used while walking metadata.
 */

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned char      extfs_u8;
typedef unsigned short     extfs_u16;
typedef unsigned int       extfs_u32;
typedef unsigned long long extfs_u64;
typedef signed long long   extfs_s64;

#define EXTFS_VERSION_MAJOR 0
#define EXTFS_VERSION_MINOR 9
#define EXTFS_VERSION_PATCH 3

#define EXTFS_SUPERBLOCK_SIZE 1024U
#define EXTFS_MAX_BLOCK_SIZE  65536U
#define EXTFS_MAX_NAME_LENGTH 255U
#define EXTFS_ROOT_INODE      2U

typedef enum extfs_status {
    EXTFS_OK                   = 0,
    EXTFS_STOP                 = 1,
    EXTFS_ERR_INVALID_ARGUMENT = -1,
    EXTFS_ERR_IO               = -2,
    EXTFS_ERR_NOT_EXT          = -3,
    EXTFS_ERR_CORRUPT          = -4,
    EXTFS_ERR_UNSUPPORTED      = -5,
    EXTFS_ERR_CHECKSUM         = -6,
    EXTFS_ERR_BUFFER_TOO_SMALL = -7,
    EXTFS_ERR_RANGE            = -8,
    EXTFS_ERR_NOT_FOUND        = -9,
    EXTFS_ERR_NOT_DIRECTORY    = -10,
    EXTFS_ERR_IS_DIRECTORY     = -11,
    EXTFS_ERR_NO_SPACE         = -12
} extfs_status;

typedef enum extfs_kind {
    EXTFS_KIND_UNKNOWN = 0,
    EXTFS_KIND_EXT2,
    EXTFS_KIND_EXT3,
    EXTFS_KIND_EXT4
} extfs_kind;

typedef enum extfs_node_type {
    EXTFS_NODE_UNKNOWN = 0,
    EXTFS_NODE_REGULAR,
    EXTFS_NODE_DIRECTORY,
    EXTFS_NODE_SYMLINK,
    EXTFS_NODE_CHARACTER,
    EXTFS_NODE_BLOCK,
    EXTFS_NODE_FIFO,
    EXTFS_NODE_SOCKET
} extfs_node_type;

/*
 * Host byte-reader contract.  Return 0 only when the complete requested range
 * was copied to destination; any non-zero result is treated as an I/O error.
 * The portable core never retains destination or calls this function with a
 * zero byte count.
 */
typedef int (*extfs_read_at_fn)(void *user,
                                extfs_u64 byte_offset,
                                void *destination,
                                extfs_u32 byte_count);

/* Optional host byte-writer.  Return 0 only when the complete requested range
 * reached the underlying block device; non-zero is an I/O error.  The bounded
 * data-write path never asks the host to allocate filesystem space. */
typedef int (*extfs_write_at_fn)(void *user,
                                 extfs_u64 byte_offset,
                                 const void *source,
                                 extfs_u32 byte_count);

/* Optional durability barrier. Return 0 only after earlier writes have been
 * forced through the host/device cache as far as the platform can guarantee.
 * JBD2 metadata transactions require this callback. */
typedef int (*extfs_flush_fn)(void *user);

/* Optional wall-clock source used for JBD2 commit records. Return 0 with a
 * Unix-epoch timestamp and nanoseconds in the range 0..999,999,999. A real
 * journal transaction requires this callback so commit records match JBD2's
 * on-disk format rather than carrying a synthetic zero timestamp. */
typedef int (*extfs_time_fn)(void *user,
                             extfs_u64 *seconds,
                             extfs_u32 *nanoseconds);

typedef struct extfs_io {
    extfs_read_at_fn read_at;
    extfs_write_at_fn write_at;
    extfs_flush_fn flush;
    extfs_time_fn time_now;
    void *user;
} extfs_io;

/* Reasons a parsed volume is not safe for traversal/mount by the reader. */
#define EXTFS_READONLY_RISK_DIRTY                 0x00000001U
#define EXTFS_READONLY_RISK_NEEDS_RECOVERY        0x00000002U
#define EXTFS_READONLY_RISK_ERROR_STATE           0x00000004U
#define EXTFS_READONLY_RISK_UNSUPPORTED_INCOMPAT  0x00000008U
#define EXTFS_READONLY_RISK_UNSUPPORTED_LAYOUT    0x00000010U
#define EXTFS_READONLY_RISK_UNVERIFIED_CHECKSUMS  0x00000020U

/* Reasons the conservative bounded in-place write path must remain disabled. */
#define EXTFS_WRITE_RISK_NO_WRITER                 0x00000001U
#define EXTFS_WRITE_RISK_READONLY_POLICY           0x00000002U
#define EXTFS_WRITE_RISK_UNSUPPORTED_RO_COMPAT     0x00000004U
#define EXTFS_WRITE_RISK_MMP                       0x00000008U

typedef struct extfs_volume {
    extfs_io io;
    extfs_kind kind;
    extfs_u32 block_size;
    extfs_u32 blocks_per_group;
    extfs_u32 inodes_per_group;
    extfs_u32 first_data_block;
    extfs_u32 total_inodes;
    extfs_u64 total_blocks;
    extfs_u64 free_blocks;
    extfs_u64 byte_size;
    extfs_u32 group_count;
    extfs_u16 inode_size;
    extfs_u16 descriptor_size;
    extfs_u16 state;
    extfs_u32 revision;
    extfs_u32 feature_compat;
    extfs_u32 feature_incompat;
    extfs_u32 feature_ro_compat;
    extfs_u32 unsupported_incompat;
    extfs_u32 checksum_seed;
    extfs_u32 journal_inode;
    extfs_u8 uuid[16];
    extfs_u8 journal_uuid[16];
    char label[17];
    extfs_u8 metadata_checksums;
    extfs_u8 superblock_checksum_valid;
} extfs_volume;

typedef struct extfs_inode {
    extfs_u32 number;
    extfs_u16 mode;
    extfs_u16 links_count;
    extfs_u32 uid;
    extfs_u32 gid;
    extfs_u32 flags;
    extfs_u32 generation;
    extfs_u64 size;
    extfs_s64 access_time;
    extfs_s64 change_time;
    extfs_s64 modification_time;
    extfs_s64 creation_time;
    extfs_u32 access_time_nanoseconds;
    extfs_u32 change_time_nanoseconds;
    extfs_u32 modification_time_nanoseconds;
    extfs_u32 creation_time_nanoseconds;
    extfs_u8 block_map[60];
} extfs_inode;

/* Reasons an internal JBD2 journal cannot be used by the transaction writer. */
#define EXTFS_JOURNAL_RISK_NO_JOURNAL           0x00000001U
#define EXTFS_JOURNAL_RISK_EXTERNAL_JOURNAL     0x00000002U
#define EXTFS_JOURNAL_RISK_NO_WRITER            0x00000004U
#define EXTFS_JOURNAL_RISK_NO_FLUSH              0x00000008U
#define EXTFS_JOURNAL_RISK_DIRTY                 0x00000010U
#define EXTFS_JOURNAL_RISK_UNSUPPORTED_FEATURE   0x00000020U
#define EXTFS_JOURNAL_RISK_CHECKSUM              0x00000040U
#define EXTFS_JOURNAL_RISK_NO_CLOCK              0x00000080U

/* Parsed state of an internal JBD2 journal. All on-disk JBD2 integer fields
 * are big-endian; these members are converted to host-order values. */
typedef struct extfs_journal {
    extfs_inode inode;
    extfs_u32 block_size;
    extfs_u32 maxlen;
    extfs_u32 first;
    extfs_u32 sequence;
    extfs_u32 start;
    extfs_u32 errno_value;
    extfs_u32 head;
    extfs_u32 max_transaction;
    extfs_u32 max_trans_data;
    extfs_u32 feature_compat;
    extfs_u32 feature_incompat;
    extfs_u32 feature_ro_compat;
    extfs_u32 checksum_seed;
    extfs_u8 checksum_type;
    extfs_u8 uuid[16];
    extfs_u8 checksum_v2;
    extfs_u8 checksum_v3;
    extfs_u8 has_64bit;
    extfs_u8 clean;
} extfs_journal;

/* One complete filesystem metadata block image to journal and checkpoint. */
typedef struct extfs_journal_metadata {
    extfs_u64 home_block;
    const void *block_data;
} extfs_journal_metadata;

/* Assess whether this particular inode can use the bounded data-only writer. */
extfs_status extfs_inode_write_assess(const extfs_volume *volume,
                                      const extfs_inode *inode);


/* Open and validate the filesystem's internal JBD2 journal. Dirty journals are
 * parsed but the conservative writer deliberately refuses them; replay is a
 * later checkpoint. scratch must be at least one filesystem block. */
extfs_status extfs_journal_open(const extfs_volume *volume,
                                extfs_journal *journal,
                                void *scratch,
                                extfs_u32 scratch_size);

/* Assess whether a parsed internal journal can accept a metadata transaction. */
extfs_status extfs_journal_write_assess(const extfs_volume *volume,
                                        const extfs_journal *journal,
                                        extfs_u32 *risk_flags);

/* Commit one conservative single-descriptor JBD2 metadata transaction. The
 * filesystem RECOVER bit is made durable before journal mutation. Descriptor
 * and data blocks are flushed before the commit record; committed metadata is
 * then checkpointed, the journal is marked empty, and RECOVER is cleared.
 * On any I/O failure after recovery is armed, the volume remains recovery-
 * required and subsequent ordinary writes are refused. scratch must be at
 * least one filesystem block. */
extfs_status extfs_journal_commit_metadata(extfs_volume *volume,
                                            extfs_journal *journal,
                                            const extfs_journal_metadata *items,
                                            extfs_u32 item_count,
                                            void *scratch,
                                            extfs_u32 scratch_size);

/*
 * Directory callbacks receive a borrowed, non-NUL-terminated UTF-8 name.
 * Return 0 to continue enumeration or non-zero to stop successfully.
 */
typedef int (*extfs_directory_callback)(void *user,
                                        extfs_u32 inode_number,
                                        extfs_node_type type,
                                        const char *name,
                                        extfs_u8 name_length);

/* Parse and validate the primary superblock.  No filesystem blocks are kept. */
extfs_status extfs_open(extfs_volume *volume, const extfs_io *io);

/*
 * Apply the conservative policy used by the Windows driver.  risk_flags is
 * always populated; EXTFS_OK means no currently known read/traversal risk remains.
 */
extfs_status extfs_readonly_assess(const extfs_volume *volume,
                                   extfs_u32 *risk_flags);

/*
 * Assess whether same-size, already-allocated regular-file data overwrites are
 * safe for this volume.  This is the common data-only gate; it does not imply
 * that the filesystem-specific allocation, resize or namespace path supports
 * the particular volume layout.
 */
extfs_status extfs_write_assess(const extfs_volume *volume,
                                extfs_u32 *risk_flags);

/*
 * Read one inode by number and validate metadata checksums when present.
 * scratch must be at least volume->inode_size bytes.
 */
extfs_status extfs_read_inode(const extfs_volume *volume,
                              extfs_u32 inode_number,
                              extfs_inode *inode,
                              void *scratch,
                              extfs_u32 scratch_size);

/*
 * Translate a file-relative logical block to a physical filesystem block.
 * Sparse and unwritten extents return EXTFS_OK with *is_hole non-zero.
 * scratch must be at least volume->block_size bytes.
 */
extfs_status extfs_map_file_block(const extfs_volume *volume,
                                  const extfs_inode *inode,
                                  extfs_u32 logical_block,
                                  extfs_u64 *physical_block,
                                  int *is_hole,
                                  void *scratch,
                                  extfs_u32 scratch_size);

/*
 * Read up to byte_count bytes from a regular file or symlink.  Reads at or
 * beyond EOF succeed with zero bytes; reads crossing EOF are shortened.  Hole
 * ranges are returned as zero bytes.  scratch must be one filesystem block.
 */
extfs_status extfs_read_file(const extfs_volume *volume,
                             const extfs_inode *inode,
                             extfs_u64 byte_offset,
                             void *destination,
                             extfs_u32 byte_count,
                             void *scratch,
                             extfs_u32 scratch_size,
                             extfs_u32 *bytes_read);

/*
 * Overwrite bytes in an existing allocated regular file without changing its
 * size or block map.  The complete range must lie below EOF and every touched
 * logical block must already be allocated and initialized.  Sparse holes and
 * unwritten extents are refused because satisfying them requires metadata
 * allocation/conversion.  No inode timestamps or journal metadata are changed.
 */
extfs_status extfs_write_file_existing(const extfs_volume *volume,
                                         const extfs_inode *inode,
                                         extfs_u64 byte_offset,
                                         const void *source,
                                         extfs_u32 byte_count,
                                         void *scratch,
                                         extfs_u32 scratch_size,
                                         extfs_u32 *bytes_written);

/*
 * Resize an existing ext2 regular file whose complete block map fits in the
 * twelve direct i_block entries.  Growth allocates ordinary filesystem blocks
 * and zero-fills newly exposed bytes; shrink releases direct blocks and zeros
 * the retained block tail.  This API remains ext2-only; ext3 and ext4 use
 * their separate journaled metadata-resize APIs.
 * scratch must be at least two filesystem blocks. A metadata I/O failure can
 * leave the ext2 volume marked dirty, requiring fsck.
 */
extfs_status extfs_resize_file_ext2_direct(extfs_volume *volume,
                                              extfs_inode *inode,
                                              extfs_u64 new_size,
                                              void *scratch,
                                              extfs_u32 scratch_size);

/*
 * Resize an existing ext3 regular file whose mapping fits entirely in the
 * twelve direct i_block entries. Metadata updates are committed through one
 * internal JBD2 transaction and therefore require a clean supported journal,
 * a host durability barrier and a wall clock. Growth may allocate direct
 * blocks from one block group per operation; shrink may release blocks from
 * one block group per operation. Newly exposed data is zeroed and flushed
 * before the metadata transaction. scratch must be at least five filesystem
 * blocks. ext4 metadata mutation remains a separate checksum-aware extent path.
 */
extfs_status extfs_resize_file_ext3_journaled_direct(extfs_volume *volume,
                                                      extfs_inode *inode,
                                                      extfs_u64 new_size,
                                                      void *scratch,
                                                      extfs_u32 scratch_size);

/*
 * Resize a bounded ext4 regular file whose extent tree is either the inline
 * depth-0 inode root or one checksummed external depth-0 leaf referenced by a
 * depth-1 inode root. Growth can convert a full four-extent inline root into
 * the external form and append/merge additional initialized extents; shrink
 * can collapse the tree back inline. Bitmap, group descriptor, inode, extent
 * block and superblock metadata are committed through JBD2. 0.9.2 still
 * requires 32-byte group descriptors and one allocation group per resize and
 * refuses depth > 1, multiple index children, 64bit/flex_bg/bigalloc, holes,
 * unwritten extents and quota-accounted layouts. scratch >= eight blocks.
 */
extfs_status extfs_resize_file_ext4_journaled_extent_tree(
    extfs_volume *volume,
    extfs_inode *inode,
    extfs_u64 new_size,
    void *scratch,
    extfs_u32 scratch_size);

/*
 * Enumerate live entries in an ext directory.  Deleted/unused records and ext4
 * checksum/index-tail records are not exposed to the callback.  A non-zero
 * callback result stops enumeration and returns EXTFS_STOP.
 */
extfs_status extfs_iterate_directory(const extfs_volume *volume,
                                     const extfs_inode *directory,
                                     extfs_directory_callback callback,
                                     void *callback_user,
                                     void *scratch,
                                     extfs_u32 scratch_size);

/* Exact byte-for-byte UTF-8 lookup within one directory. */
extfs_status extfs_lookup(const extfs_volume *volume,
                          const extfs_inode *directory,
                          const char *name,
                          extfs_u8 name_length,
                          extfs_u32 *inode_number,
                          void *scratch,
                          extfs_u32 scratch_size);

/*
 * Resolve a root-relative '/'-separated UTF-8 path from EXTFS_ROOT_INODE.
 * Leading '/' characters are optional; symlink traversal is not performed.
 */
extfs_status extfs_resolve_path(const extfs_volume *volume,
                                const char *path,
                                extfs_inode *inode,
                                void *scratch,
                                extfs_u32 scratch_size);

/* Lightweight helpers; returned strings have static lifetime. */
extfs_node_type extfs_inode_type(const extfs_inode *inode);
const char *extfs_status_string(extfs_status status);
const char *extfs_kind_string(extfs_kind kind);

#ifdef __cplusplus
}
#endif

#endif
