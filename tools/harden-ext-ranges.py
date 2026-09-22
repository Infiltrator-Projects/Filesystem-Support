#!/usr/bin/env python3
"""
Apply generic safety contracts learned from Infiltratr Common to EXT2/3/4.

The kernel drivers cannot link the userspace Common library.  This shaping step
therefore ports only contracts that are mathematically generic and strictly
stronger: subtraction-first range validation and overflow-free interval tests.
Filesystem-format semantics remain local to the owning EXT implementation.
"""

from pathlib import Path

ROOT = Path("native/filesystems")


def replace_once(path: Path, old: str, new: str) -> None:
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{path}: expected one match, found {count}")
    path.write_text(text.replace(old, new), encoding="utf-8")


def replace_exact(path: Path, old: str, new: str, expected: int) -> None:
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count != expected:
        raise RuntimeError(
            f"{path}: expected {expected} matches, found {count}"
        )
    path.write_text(text.replace(old, new), encoding="utf-8")


def replace_once_or_accept(
    path: Path, old: str, new: str, accepted: str
) -> None:
    """Replace one legacy form or accept an equivalent stronger form."""
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count == 1:
        path.write_text(text.replace(old, new), encoding="utf-8")
        return
    if count == 0 and accepted in text:
        return
    raise RuntimeError(
        f"{path}: expected one legacy match or the accepted safe form, "
        f"found {count} legacy matches"
    )


# EXT2: validate the half-open block range before constructing its inclusive
# endpoint.  This is the same subtraction-first contract Common uses for
# bounded positioned I/O.
replace_once(
    ROOT / "ext2/kernel/balloc.c",
    """int ext2_data_block_valid(struct ext2_sb_info *sbi, ext2_fsblk_t start_blk,
\t\t\t  unsigned int count)
{
\tif ((start_blk <= le32_to_cpu(sbi->s_es->s_first_data_block)) ||
\t    (start_blk + count - 1 < start_blk) ||
\t    (start_blk + count - 1 >= le32_to_cpu(sbi->s_es->s_blocks_count)))
\t\treturn 0;

\t/* Ensure we do not step over superblock */
\tif ((start_blk <= sbi->s_sb_block) &&
\t    (start_blk + count - 1 >= sbi->s_sb_block))
\t\treturn 0;

\treturn 1;
}
""",
    """int ext2_data_block_valid(struct ext2_sb_info *sbi, ext2_fsblk_t start_blk,
\t\t\t  unsigned int count)
{
\text2_fsblk_t blocks_count = le32_to_cpu(sbi->s_es->s_blocks_count);
\text2_fsblk_t last_blk;

\tif (count == 0 ||
\t    start_blk <= le32_to_cpu(sbi->s_es->s_first_data_block) ||
\t    start_blk >= blocks_count ||
\t    count > blocks_count - start_blk)
\t\treturn 0;

\tlast_blk = start_blk + count - 1;
\tif (start_blk <= sbi->s_sb_block && last_blk >= sbi->s_sb_block)
\t\treturn 0;

\treturn 1;
}
""",
)

replace_once(
    ROOT / "ext2/kernel/xattr.c",
    """\tif (size > end_offs ||
\t    le16_to_cpu(entry->e_value_offs) + size > end_offs)
\t\treturn false;
""",
    """\tif (size > end_offs ||
\t    le16_to_cpu(entry->e_value_offs) > end_offs - size)
\t\treturn false;
""",
)

# EXT3: the historical in_range macro and several validators formed the end
# point before proving it representable.  Keep the same interval semantics
# while making the proof precede the addition.
replace_once(
    ROOT / "ext3/kernel/balloc.c",
    """#define in_range(b, first, len)\t((b) >= (first) && (b) <= (first) + (len) - 1)
""",
    """#define in_range(b, first, len) \\
\t((len) != 0 && (b) >= (first) && (b) - (first) < (len))
""",
)

replace_once(
    ROOT / "ext3/kernel/balloc.c",
    """\tif (block < le32_to_cpu(es->s_first_data_block) ||
\t    block + count < block ||
\t    block + count > le32_to_cpu(es->s_blocks_count)) {
""",
    """\tif (count == 0 ||
\t    block < le32_to_cpu(es->s_first_data_block) ||
\t    block >= le32_to_cpu(es->s_blocks_count) ||
\t    count > le32_to_cpu(es->s_blocks_count) - block) {
""",
)

replace_once(
    ROOT / "ext3/kernel/xattr.c",
    """\tif (entry->e_value_block != 0 || value_size > size ||
\t    le16_to_cpu(entry->e_value_offs) + value_size > size)
\t\treturn -EIO;
""",
    """\tif (entry->e_value_block != 0 || value_size > size ||
\t    le16_to_cpu(entry->e_value_offs) > size - value_size)
\t\treturn -EIO;
""",
)

replace_once(
    ROOT / "ext3/kernel/resize.c",
    """\tif (le32_to_cpu(es->s_blocks_count) + input->blocks_count <
\t    le32_to_cpu(es->s_blocks_count)) {
""",
    """\tif (input->blocks_count >
\t    U32_MAX - le32_to_cpu(es->s_blocks_count)) {
""",
)

replace_once(
    ROOT / "ext3/kernel/resize.c",
    """\tif (le32_to_cpu(es->s_inodes_count) + EXT3_INODES_PER_GROUP(sb) <
\t    le32_to_cpu(es->s_inodes_count)) {
""",
    """\tif (EXT3_INODES_PER_GROUP(sb) >
\t    U32_MAX - le32_to_cpu(es->s_inodes_count)) {
""",
)

# EXT4 already contains many subtraction-first validators.  Bring the older
# remaining block/resize checks up to the same contract.
replace_once(
    ROOT / "ext4/kernel/block_validity.c",
    """int ext4_sb_block_valid(struct super_block *sb, struct inode *inode,
\t\t\t\text4_fsblk_t start_blk, unsigned int count)
{
\tstruct ext4_sb_info *sbi = EXT4_SB(sb);
\tstruct ext4_system_blocks *system_blks;
\tstruct ext4_system_zone *entry;
\tstruct rb_node *n;
\tint ret = 1;

\tif ((start_blk <= le32_to_cpu(sbi->s_es->s_first_data_block)) ||
\t    (start_blk + count < start_blk) ||
\t    (start_blk + count > ext4_blocks_count(sbi->s_es)))
\t\treturn 0;

\t/*
\t * Lock the system zone to prevent it being released concurrently
\t * when doing a remount which inverse current "[no]block_validity"
\t * mount option.
\t */
\trcu_read_lock();
""",
    """int ext4_sb_block_valid(struct super_block *sb, struct inode *inode,
\t\t\t\text4_fsblk_t start_blk, unsigned int count)
{
\tstruct ext4_sb_info *sbi = EXT4_SB(sb);
\tstruct ext4_system_blocks *system_blks;
\tstruct ext4_system_zone *entry;
\tstruct rb_node *n;
\text4_fsblk_t blocks_count = ext4_blocks_count(sbi->s_es);
\text4_fsblk_t last_blk;
\tint ret = 1;

\tif (count == 0 ||
\t    start_blk <= le32_to_cpu(sbi->s_es->s_first_data_block) ||
\t    start_blk >= blocks_count ||
\t    count > blocks_count - start_blk)
\t\treturn 0;

\tlast_blk = start_blk + count - 1;
\trcu_read_lock();
""",
)

replace_once(
    ROOT / "ext4/kernel/block_validity.c",
    """\t\tif (start_blk + count - 1 < entry->start_blk)
\t\t\tn = n->rb_left;
\t\telse if (start_blk >= (entry->start_blk + entry->count))
\t\t\tn = n->rb_right;
""",
    """\t\tif (last_blk < entry->start_blk)
\t\t\tn = n->rb_left;
\t\telse if (start_blk >= entry->start_blk &&
\t\t\t start_blk - entry->start_blk >= entry->count)
\t\t\tn = n->rb_right;
""",
)


# The system-zone tree uses the same interval contract. Compare distance
# instead of constructing an exclusive endpoint, and reject impossible merged
# counts before mutating a node.
replace_once(
    ROOT / "ext4/kernel/block_validity.c",
    """\tif ((entry1->start_blk + entry1->count) == entry2->start_blk &&
\t    entry1->ino == entry2->ino)
""",
    """\tif (entry2->start_blk >= entry1->start_blk &&
\t    entry2->start_blk - entry1->start_blk == entry1->count &&
\t    entry1->ino == entry2->ino)
""",
)

replace_once(
    ROOT / "ext4/kernel/block_validity.c",
    """\t\telse if (start_blk >= (entry->start_blk + entry->count))
\t\t\tn = &(*n)->rb_right;
""",
    """\t\telse if (start_blk >= entry->start_blk &&
\t\t\t start_blk - entry->start_blk >= entry->count)
\t\t\tn = &(*n)->rb_right;
""",
)

replace_once(
    ROOT / "ext4/kernel/block_validity.c",
    """\t\tif (can_merge(entry, new_entry)) {
\t\t\tnew_entry->start_blk = entry->start_blk;
\t\t\tnew_entry->count += entry->count;
""",
    """\t\tif (can_merge(entry, new_entry)) {
\t\t\tif (entry->count > UINT_MAX - new_entry->count)
\t\t\t\treturn -EFSCORRUPTED;
\t\t\tnew_entry->start_blk = entry->start_blk;
\t\t\tnew_entry->count += entry->count;
""",
)

replace_once(
    ROOT / "ext4/kernel/block_validity.c",
    """\t\tif (can_merge(new_entry, entry)) {
\t\t\tnew_entry->count += entry->count;
""",
    """\t\tif (can_merge(new_entry, entry)) {
\t\t\tif (entry->count > UINT_MAX - new_entry->count)
\t\t\t\treturn -EFSCORRUPTED;
\t\t\tnew_entry->count += entry->count;
""",
)

replace_once(
    ROOT / "ext4/kernel/resize.c",
    """\tif (ext4_blocks_count(es) + input->blocks_count <
\t    ext4_blocks_count(es)) {
""",
    """\tif (input->blocks_count >
\t    U64_MAX - ext4_blocks_count(es)) {
""",
)

replace_once(
    ROOT / "ext4/kernel/resize.c",
    """\tif (le32_to_cpu(es->s_inodes_count) + EXT4_INODES_PER_GROUP(sb) <
\t    le32_to_cpu(es->s_inodes_count)) {
""",
    """\tif (EXT4_INODES_PER_GROUP(sb) >
\t    U32_MAX - le32_to_cpu(es->s_inodes_count)) {
""",
)


# Checked allocation sizing. Common's generic contract is to reject arithmetic
# overflow before allocation. Kernel code expresses the same contract through
# the Linux array-allocation and overflow helpers instead of linking the
# userspace Common library into a .ko.
replace_exact(
    ROOT / "ext3/kernel/jbd_journal.c",
    """\tjournal->j_wbuf = kmalloc(n * sizeof(struct buffer_head*), GFP_KERNEL);
""",
    """\tjournal->j_wbuf = kmalloc_array(n, sizeof(struct buffer_head *),
\t\t\t\t\t GFP_KERNEL);
""",
    2,
)

replace_once(
    ROOT / "ext3/kernel/jbd_revoke.c",
    """\ttable->hash_table =
\t\tkmalloc(hash_size * sizeof(struct list_head), GFP_KERNEL);
""",
    """\ttable->hash_table =
\t\tkmalloc_array(hash_size, sizeof(struct list_head), GFP_KERNEL);
""",
)

replace_once(
    ROOT / "ext3/kernel/super.c",
    """\tsbi->s_group_desc = kmalloc(db_count * sizeof (struct buffer_head *),
\t\t\t\t    GFP_KERNEL);
""",
    """\tsbi->s_group_desc = kmalloc_array(db_count,
\t\t\t\t\t sizeof(struct buffer_head *), GFP_KERNEL);
""",
)

replace_once(
    ROOT / "ext3/kernel/resize.c",
    """\tn_group_desc = kmalloc((gdb_num + 1) * sizeof(struct buffer_head *),
\t\t\tGFP_NOFS);
""",
    """\tn_group_desc = kmalloc_array(gdb_num + 1,
\t\t\t\t     sizeof(struct buffer_head *), GFP_NOFS);
""",
)

replace_once(
    ROOT / "ext3/kernel/resize.c",
    """\tprimary = kmalloc(reserved_gdb * sizeof(*primary), GFP_NOFS);
""",
    """\tprimary = kmalloc_array(reserved_gdb, sizeof(*primary), GFP_NOFS);
""",
)

# Once shrink has been rejected, n_blocks_count - o_blocks_count is the exact
# remaining capacity. Older EXT3 formed the endpoint first; newer source uses
# check_add_overflow() and carries the checked endpoint in new_blocks_count.
# Keep whichever safe form the imported source already provides.
replace_once_or_accept(
    ROOT / "ext3/kernel/resize.c",
    """\tif (o_blocks_count + add < o_blocks_count) {
\t\text3_warning(sb, __func__, "blocks_count overflow");
\t\treturn -EINVAL;
\t}

\tif (o_blocks_count + add > n_blocks_count)
\t\tadd = n_blocks_count - o_blocks_count;

\tif (o_blocks_count + add < n_blocks_count)
""",
    """\tif (add > n_blocks_count - o_blocks_count)
\t\tadd = n_blocks_count - o_blocks_count;

\tif (add < n_blocks_count - o_blocks_count)
""",
    """\tif (check_add_overflow(o_blocks_count, (ext3_fsblk_t)add,
\t\t\t       &new_blocks_count)) {
""",
)

replace_exact(
    ROOT / "ext4/kernel/resize.c",
    """\tn_group_desc = kvmalloc((gdb_num + 1) * sizeof(struct buffer_head *),
\t\t\t\tGFP_KERNEL);
""",
    """\tn_group_desc = kvmalloc_array(gdb_num + 1,
\t\t\t\t      sizeof(struct buffer_head *), GFP_KERNEL);
""",
    2,
)

replace_once(
    ROOT / "ext4/kernel/fast_commit.c",
    """#include "mballoc.h"


#include <trace/events/ext4.h>
""",
    """#include "mballoc.h"

#include <linux/overflow.h>
#include <trace/events/ext4.h>
""",
)

replace_once(
    ROOT / "ext4/kernel/fast_commit.c",
    """\tif (state->fc_modified_inodes_used == state->fc_modified_inodes_size) {
\t\tint *fc_modified_inodes;

\t\tfc_modified_inodes = krealloc(state->fc_modified_inodes,
\t\t\t\tsizeof(int) * (state->fc_modified_inodes_size +
\t\t\t\tEXT4_FC_REPLAY_REALLOC_INCREMENT),
\t\t\t\tGFP_KERNEL);
\t\tif (!fc_modified_inodes)
\t\t\treturn -ENOMEM;
\t\tstate->fc_modified_inodes = fc_modified_inodes;
\t\tstate->fc_modified_inodes_size +=
\t\t\tEXT4_FC_REPLAY_REALLOC_INCREMENT;
\t}
""",
    """\tif (state->fc_modified_inodes_used == state->fc_modified_inodes_size) {
\t\ttypeof(state->fc_modified_inodes_size) new_size;
\t\tint *fc_modified_inodes;

\t\tif (check_add_overflow(state->fc_modified_inodes_size,
\t\t\t\t       EXT4_FC_REPLAY_REALLOC_INCREMENT,
\t\t\t\t       &new_size))
\t\t\treturn -EOVERFLOW;
\t\tfc_modified_inodes = krealloc_array(state->fc_modified_inodes,
\t\t\t\t\t\t    new_size,
\t\t\t\t\t\t    sizeof(*fc_modified_inodes),
\t\t\t\t\t\t    GFP_KERNEL);
\t\tif (!fc_modified_inodes)
\t\t\treturn -ENOMEM;
\t\tstate->fc_modified_inodes = fc_modified_inodes;
\t\tstate->fc_modified_inodes_size = new_size;
\t}
""",
)

replace_once(
    ROOT / "ext4/kernel/fast_commit.c",
    """\tif (state->fc_regions_used == state->fc_regions_size) {
\t\tstruct ext4_fc_alloc_region *fc_regions;

\t\tfc_regions = krealloc(state->fc_regions,
\t\t\t\t      sizeof(struct ext4_fc_alloc_region) *
\t\t\t\t      (state->fc_regions_size +
\t\t\t\t       EXT4_FC_REPLAY_REALLOC_INCREMENT),
\t\t\t\t      GFP_KERNEL);
\t\tif (!fc_regions)
\t\t\treturn -ENOMEM;
\t\tstate->fc_regions_size +=
\t\t\tEXT4_FC_REPLAY_REALLOC_INCREMENT;
\t\tstate->fc_regions = fc_regions;
\t}
""",
    """\tif (state->fc_regions_used == state->fc_regions_size) {
\t\ttypeof(state->fc_regions_size) new_size;
\t\tstruct ext4_fc_alloc_region *fc_regions;

\t\tif (check_add_overflow(state->fc_regions_size,
\t\t\t\t       EXT4_FC_REPLAY_REALLOC_INCREMENT,
\t\t\t\t       &new_size))
\t\t\treturn -EOVERFLOW;
\t\tfc_regions = krealloc_array(state->fc_regions, new_size,
\t\t\t\t\t    sizeof(*fc_regions), GFP_KERNEL);
\t\tif (!fc_regions)
\t\t\treturn -ENOMEM;
\t\tstate->fc_regions_size = new_size;
\t\tstate->fc_regions = fc_regions;
\t}
""",
)

# All three ACL serializers have already computed their exact encoded size.
# Allocate that proven value rather than rebuilding a second multiplication.
for fs, gfp in (("ext2", "GFP_KERNEL"), ("ext3", "GFP_NOFS"),
                ("ext4", "GFP_NOFS")):
    replace_once(
        ROOT / f"{fs}/kernel/xattr.c",
        f"""\text_acl = kmalloc(sizeof({fs}_acl_header) + acl->a_count *
\t\t\tsizeof({fs}_acl_entry), {gfp});
""",
        f"""\text_acl = kmalloc(*size, {gfp});
""",
    )

# Structural invariants: the exact unsafe forms this pass exists to remove must
# not silently return after a future upstream refresh.
checks = {
    ROOT / "ext2/kernel/balloc.c": (
        "start_blk + count - 1 < start_blk",
    ),
    ROOT / "ext2/kernel/xattr.c": (
        "le16_to_cpu(entry->e_value_offs) + size > end_offs",
    ),
    ROOT / "ext3/kernel/balloc.c": (
        "(b) <= (first) + (len) - 1",
        "block + count < block",
    ),
    ROOT / "ext3/kernel/xattr.c": (
        "le16_to_cpu(entry->e_value_offs) + value_size > size",
    ),
    ROOT / "ext3/kernel/resize.c": (
        "le32_to_cpu(es->s_blocks_count) + input->blocks_count <",
        "le32_to_cpu(es->s_inodes_count) + EXT3_INODES_PER_GROUP(sb) <",
    ),
    ROOT / "ext4/kernel/block_validity.c": (
        "start_blk + count < start_blk",
        "start_blk + count - 1 < entry->start_blk",
        "start_blk >= (entry->start_blk + entry->count)",
        "(entry1->start_blk + entry1->count) == entry2->start_blk",
    ),
    ROOT / "ext4/kernel/resize.c": (
        "ext4_blocks_count(es) + input->blocks_count <",
        "le32_to_cpu(es->s_inodes_count) + EXT4_INODES_PER_GROUP(sb) <",
        "kvmalloc((gdb_num + 1) * sizeof(struct buffer_head *)",
    ),
    ROOT / "ext3/kernel/jbd_journal.c": (
        "kmalloc(n * sizeof(struct buffer_head*)",
    ),
    ROOT / "ext3/kernel/jbd_revoke.c": (
        "kmalloc(hash_size * sizeof(struct list_head)",
    ),
    ROOT / "ext3/kernel/super.c": (
        "kmalloc(db_count * sizeof (struct buffer_head *)",
    ),
    ROOT / "ext3/kernel/resize.c": (
        "le32_to_cpu(es->s_blocks_count) + input->blocks_count <",
        "le32_to_cpu(es->s_inodes_count) + EXT3_INODES_PER_GROUP(sb) <",
        "o_blocks_count + add < o_blocks_count",
        "kmalloc((gdb_num + 1) * sizeof(struct buffer_head *)",
        "kmalloc(reserved_gdb * sizeof(*primary)",
    ),
    ROOT / "ext4/kernel/fast_commit.c": (
        "sizeof(int) * (state->fc_modified_inodes_size +",
        "sizeof(struct ext4_fc_alloc_region) *",
    ),
    ROOT / "ext2/kernel/xattr.c": (
        "le16_to_cpu(entry->e_value_offs) + size > end_offs",
        "kmalloc(sizeof(ext2_acl_header) + acl->a_count *",
    ),
    ROOT / "ext3/kernel/xattr.c": (
        "le16_to_cpu(entry->e_value_offs) + value_size > size",
        "kmalloc(sizeof(ext3_acl_header) + acl->a_count *",
    ),
    ROOT / "ext4/kernel/xattr.c": (
        "kmalloc(sizeof(ext4_acl_header) + acl->a_count *",
    ),
}

for path, forbidden in checks.items():
    text = path.read_text(encoding="utf-8")
    for pattern in forbidden:
        if pattern in text:
            raise RuntimeError(f"{path}: unsafe range form survived: {pattern}")

print("EXT Common-derived range hardening: OK")
