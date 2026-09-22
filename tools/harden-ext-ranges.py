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
        "entry->start_blk + entry->count",
    ),
    ROOT / "ext4/kernel/resize.c": (
        "ext4_blocks_count(es) + input->blocks_count <",
        "le32_to_cpu(es->s_inodes_count) + EXT4_INODES_PER_GROUP(sb) <",
    ),
}

for path, forbidden in checks.items():
    text = path.read_text(encoding="utf-8")
    for pattern in forbidden:
        if pattern in text:
            raise RuntimeError(f"{path}: unsafe range form survived: {pattern}")

print("EXT Common-derived range hardening: OK")
