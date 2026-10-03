#!/usr/bin/env python3
"""Generate minimal writable SFS0/SFS2 qualification media.

This generator is deliberately self-contained.  It emits the filesystem
structures consumed by the project's SFS and SFS2 implementations so runtime
qualification can exercise those drivers without depending on another
project's synthetic analyser fixtures.
"""

from __future__ import annotations

import argparse
import struct
from pathlib import Path

BLOCK_SIZE = 512
ADMIN_BLOCKS = 32
ROOT_NODE = 1
RECYCLED_NODE = 2

ID_ADMC = b"ADMC"
ID_OBJC = b"OBJC"
ID_HTAB = b"HTAB"
ID_TROK = b"TROK"
ID_BNDC = b"BNDC"
ID_NDC = b"NDC "
ID_BTMP = b"BTMP"

OTYPE_HIDDEN = 0x01
OTYPE_UNDELETABLE = 0x02
OTYPE_QUICKDIR = 0x04
OTYPE_DIR = 0x80
ROOTBITS_RECYCLED = 0x40


def put16(buf: bytearray, offset: int, value: int) -> None:
    struct.pack_into(">H", buf, offset, value & 0xFFFF)


def put32(buf: bytearray, offset: int, value: int) -> None:
    struct.pack_into(">I", buf, offset, value & 0xFFFFFFFF)


def get32(buf: bytearray, offset: int) -> int:
    return struct.unpack_from(">I", buf, offset)[0]


def finish_block(buf: bytearray, own_block: int, sfs2: bool) -> None:
    put32(buf, 4, 0)
    put32(buf, 8, own_block)
    total = 0
    for offset in range(0, len(buf), 4):
        total = (total + get32(buf, offset)) & 0xFFFFFFFF
    checksum = ((~total) - (1 if sfs2 else 0)) & 0xFFFFFFFF
    put32(buf, 4, checksum)


def name_hash(name: bytes) -> int:
    folded = bytearray()
    for value in name:
        if 0x61 <= value <= 0x7A or (0xE0 <= value <= 0xFE and value != 0xF7):
            value -= 0x20
        folded.append(value)
    value = len(folded)
    for ch in folded:
        value = ((value * 13) + ch) & 0xFFFF
    return value


def write_object(
    buf: bytearray,
    offset: int,
    *,
    node: int,
    protection: int,
    first_union: int,
    second_union: int,
    timestamp: int,
    bits: int,
    name: bytes,
    sfs2: bool,
) -> int:
    extra = 2 if sfs2 else 0
    put32(buf, offset + 4, node)
    put32(buf, offset + 8, protection)
    put32(buf, offset + 12, first_union)
    put32(buf, offset + 16, second_union)
    put32(buf, offset + 20 + extra, timestamp)
    buf[offset + 24 + extra] = bits & 0xFF
    name_offset = offset + 25 + extra
    buf[name_offset : name_offset + len(name)] = name
    buf[name_offset + len(name)] = 0
    buf[name_offset + len(name) + 1] = 0
    raw_end = name_offset + len(name) + 2
    return (raw_end + 1) & ~1


def set_bitmap_free(bitmap: bytearray, local_block: int) -> None:
    word_offset = 12 + (local_block // 32) * 4
    word = get32(bitmap, word_offset)
    word |= 1 << (31 - (local_block & 31))
    put32(bitmap, word_offset, word)


def make_image(path: Path, *, sfs2: bool, blocks: int, volume: str) -> None:
    if blocks < 128:
        raise ValueError("qualification image requires at least 128 blocks")
    if blocks > 0xFFFFFFFF:
        raise ValueError("block count exceeds SFS root field")

    volume_bytes = volume.encode("latin-1", "strict")
    if not 1 <= len(volume_bytes) <= 30 or b":" in volume_bytes or b"/" in volume_bytes:
        raise ValueError("volume name must be 1..30 Latin-1 bytes without ':' or '/'")

    bits_per_bitmap = (BLOCK_SIZE - 12) * 8
    bitmap_count = (blocks + bits_per_bitmap - 1) // bits_per_bitmap
    reserved_start = 1
    reserved_end = 1
    admin = reserved_start
    root_object = admin + 1
    root_hash = root_object + 1
    transaction = root_object + 2
    extent_root = root_object + 3
    object_node_root = root_object + 4
    recycled_object = root_object + 5
    bitmap_base = admin + ADMIN_BLOCKS
    first_free = ADMIN_BLOCKS + bitmap_count + reserved_start
    last_free_exclusive = blocks - reserved_end

    if bitmap_base + bitmap_count >= last_free_exclusive:
        raise ValueError("image too small for SFS metadata")

    image = bytearray(blocks * BLOCK_SIZE)

    def install(block_number: int, block: bytearray) -> None:
        start = block_number * BLOCK_SIZE
        image[start : start + BLOCK_SIZE] = block

    # Admin-space container.  Blocks admin..admin+31 are removed from the
    # general bitmap and allocated internally.  The first seven are the
    # initial filesystem metadata objects; the remaining 25 are available to
    # the driver for hash tables, object containers and node-tree growth.
    block = bytearray(BLOCK_SIZE)
    block[0:4] = ID_ADMC
    block[20] = ADMIN_BLOCKS
    put32(block, 24, admin)
    put32(block, 28, 0xFE000000)
    finish_block(block, admin, sfs2)
    install(admin, block)

    # Root object container.  Its first object must be the root directory.
    block = bytearray(BLOCK_SIZE)
    block[0:4] = ID_OBJC
    write_object(
        block,
        24,
        node=ROOT_NODE,
        protection=0x0F,
        first_union=root_hash,
        second_union=recycled_object,
        timestamp=0,
        bits=OTYPE_DIR,
        name=volume_bytes,
        sfs2=sfs2,
    )
    free_blocks = blocks - ADMIN_BLOCKS - reserved_start - reserved_end - bitmap_count
    root_info = BLOCK_SIZE - 36
    put32(block, root_info + 8, free_blocks)
    put32(block, root_info + 12, 0)
    finish_block(block, root_object, sfs2)
    install(root_object, block)

    # Root directory hash table contains only the hidden recycled directory.
    block = bytearray(BLOCK_SIZE)
    block[0:4] = ID_HTAB
    put32(block, 12, ROOT_NODE)
    chain_count = (BLOCK_SIZE - 16) // 4
    chain = name_hash(b".recycled") % chain_count
    put32(block, 16 + chain * 4, RECYCLED_NODE)
    finish_block(block, root_hash, sfs2)
    install(root_hash, block)

    # Clean transaction marker.
    block = bytearray(BLOCK_SIZE)
    block[0:4] = ID_TROK
    finish_block(block, transaction, sfs2)
    install(transaction, block)

    # Empty extent B-tree leaf.
    block = bytearray(BLOCK_SIZE)
    block[0:4] = ID_BNDC
    block[14] = 1
    block[15] = 16 if sfs2 else 14
    finish_block(block, extent_root, sfs2)
    install(extent_root, block)

    # Object-node leaf.  Nodes 1 and 2 identify root and recycled.  Nodes
    # 3..6 are reserved by the format; node 7 is the first creatable node.
    block = bytearray(BLOCK_SIZE)
    block[0:4] = ID_NDC
    put32(block, 12, 1)
    put32(block, 16, 1)
    put32(block, 20, root_object)
    put32(block, 30, recycled_object)
    put16(block, 38, name_hash(b".recycled"))
    for slot in range(2, 6):
        put32(block, 20 + slot * 10, 0xFFFFFFFF)
    finish_block(block, object_node_root, sfs2)
    install(object_node_root, block)

    # Hidden recycled directory.
    block = bytearray(BLOCK_SIZE)
    block[0:4] = ID_OBJC
    put32(block, 12, ROOT_NODE)
    write_object(
        block,
        24,
        node=RECYCLED_NODE,
        protection=0x0C,
        first_union=0,
        second_union=0,
        timestamp=0,
        bits=OTYPE_DIR | OTYPE_UNDELETABLE | OTYPE_QUICKDIR | OTYPE_HIDDEN,
        name=b".recycled",
        sfs2=sfs2,
    )
    finish_block(block, recycled_object, sfs2)
    install(recycled_object, block)

    # General free-space bitmap.  Only blocks after admin space + bitmap
    # storage and before the backup root are initially free.
    for bitmap_index in range(bitmap_count):
        block = bytearray(BLOCK_SIZE)
        block[0:4] = ID_BTMP
        coverage_start = bitmap_index * bits_per_bitmap
        coverage_end = min(blocks, coverage_start + bits_per_bitmap)
        for absolute in range(max(first_free, coverage_start), min(last_free_exclusive, coverage_end)):
            set_bitmap_free(block, absolute - coverage_start)
        own = bitmap_base + bitmap_index
        finish_block(block, own, sfs2)
        install(own, block)

    # Root copies.  Both carry the same metadata; block identity is included
    # in each checksum.  A zero sequence tie intentionally selects a clean
    # equivalent root without inventing an update history.
    root_template = bytearray(BLOCK_SIZE)
    root_template[0:4] = b"SFS\x02" if sfs2 else b"SFS\x00"
    put16(root_template, 12, 4 if sfs2 else 3)
    put16(root_template, 14, 0)
    put32(root_template, 16, 0)
    root_template[20] = ROOTBITS_RECYCLED
    put32(root_template, 32, 0)
    put32(root_template, 36, 0)
    total_bytes = blocks * BLOCK_SIZE
    put32(root_template, 40, total_bytes >> 32)
    put32(root_template, 44, total_bytes)
    put32(root_template, 48, blocks)
    put32(root_template, 52, BLOCK_SIZE)
    put32(root_template, 96, bitmap_base)
    put32(root_template, 100, admin)
    put32(root_template, 104, root_object)
    put32(root_template, 108, extent_root)
    put32(root_template, 112, object_node_root)

    primary = bytearray(root_template)
    finish_block(primary, 0, sfs2)
    install(0, primary)

    backup = bytearray(root_template)
    finish_block(backup, blocks - 1, sfs2)
    install(blocks - 1, backup)

    # Internal consistency checks for every metadata block generated here.
    checksum_target = 0xFFFFFFFE if sfs2 else 0xFFFFFFFF
    for number in [0, admin, root_object, root_hash, transaction,
                   extent_root, object_node_root, recycled_object,
                   *range(bitmap_base, bitmap_base + bitmap_count), blocks - 1]:
        data = image[number * BLOCK_SIZE : (number + 1) * BLOCK_SIZE]
        total = sum(struct.unpack_from(">I", data, offset)[0]
                    for offset in range(0, BLOCK_SIZE, 4)) & 0xFFFFFFFF
        if total != checksum_target:
            raise AssertionError(f"checksum mismatch in block {number}: {total:#x}")
        if get32(data, 8) != number:
            raise AssertionError(f"own-block mismatch in block {number}")

    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(image)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--variant", choices=("sfs", "sfs2"), required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--blocks", type=int, default=4096)
    parser.add_argument("--volume", default="RuntimeTest")
    args = parser.parse_args()

    make_image(
        args.output,
        sfs2=args.variant == "sfs2",
        blocks=args.blocks,
        volume=args.volume,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
