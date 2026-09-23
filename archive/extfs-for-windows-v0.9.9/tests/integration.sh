#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
set -eu

project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
test_dir="$project_dir/build/integration"
fixture_dir="$project_dir/tests/fixtures"
tool="$project_dir/build/extfs-tool"
mutator="$project_dir/build/extfs-mutate-test"
stage_dir="$test_dir/stage"
mutation_stage="$test_dir/mutation-stage"

mkdir -p "$test_dir"
rm -rf "$stage_dir" "$mutation_stage"
mkdir -p "$stage_dir" "$mutation_stage"
cp -R "$fixture_dir/." "$stage_dir/"
cp "$fixture_dir/hello.txt" "$mutation_stage/hello.txt"
dd if=/dev/urandom of="$stage_dir/big.bin" bs=1048576 count=2 status=none
dd if=/dev/urandom of="$test_dir/sparse-seed.bin" bs=1024 count=1 status=none
truncate -s 8M "$stage_dir/sparse.bin"
sparse_extent=0
while [ "$sparse_extent" -lt 12 ]; do
    dd if="$test_dir/sparse-seed.bin" of="$stage_dir/sparse.bin" bs=1 \
        seek=$((sparse_extent * 524288)) conv=notrunc status=none
    sparse_extent=$((sparse_extent + 1))
done
ln -s hello.txt "$stage_dir/link-to-hello"

entry=0
while [ "$entry" -lt 300 ]; do
    cp "$fixture_dir/hello.txt" "$stage_dir/subdir/item-$entry.txt"
    entry=$((entry + 1))
done

for filesystem in ext2 ext3 ext4; do
    image="$test_dir/$filesystem.img"
    extracted="$test_dir/$filesystem-hello.txt"
    truncate -s 96M "$image"
    mke2fs -q -F -t "$filesystem" -b 1024 -L "EXTFS-$filesystem" \
        -d "$stage_dir" "$image"
    e2fsck -f -y -D "$image" > "$test_dir/$filesystem-e2fsck.txt" 2>&1
    "$tool" info "$image" > "$test_dir/$filesystem-info.txt"
    "$tool" ls "$image" / > "$test_dir/$filesystem-root.txt"
    "$tool" extract "$image" /hello.txt "$extracted"
    cmp "$stage_dir/hello.txt" "$extracted"
    "$tool" cat "$image" /subdir/nested.txt > "$test_dir/$filesystem-nested.txt"
    cmp "$stage_dir/subdir/nested.txt" "$test_dir/$filesystem-nested.txt"
    "$tool" extract "$image" /big.bin "$test_dir/$filesystem-big.bin"
    cmp "$stage_dir/big.bin" "$test_dir/$filesystem-big.bin"
    "$tool" extract "$image" /sparse.bin "$test_dir/$filesystem-sparse.bin"
    cmp "$stage_dir/sparse.bin" "$test_dir/$filesystem-sparse.bin"
    test "$("$tool" cat "$image" /link-to-hello)" = "hello.txt"
    "$tool" ls "$image" /subdir > "$test_dir/$filesystem-subdir.txt"
    grep 'item-299.txt' "$test_dir/$filesystem-subdir.txt" > /dev/null
done

# Run actual ExtFS metadata mutation against real mke2fs images. ext2/ext3 grow
# beyond the twelve 1 KiB direct blocks so CI exercises creation and later
# removal of a real classic single-indirect block. The ext4 image keeps the
# separately-qualified bounded extent-tree path.
for filesystem in ext2 ext3 ext4; do
    image="$test_dir/$filesystem-mutation.img"
    expected_growth="$test_dir/$filesystem-expected-growth.bin"
    expected_shrink="$test_dir/$filesystem-expected-shrink.bin"
    extracted="$test_dir/$filesystem-mutated.bin"
    growth_size=4096
    truncate -s 64M "$image"

    case "$filesystem" in
        ext2)
            growth_size=13312
            mke2fs -q -F -t ext2 -b 1024 -E lazy_itable_init=0 \
                -L EXTFS-MUT-EXT2 -d "$mutation_stage" "$image"
            ;;
        ext3)
            growth_size=13312
            mke2fs -q -F -t ext3 -b 1024 \
                -E lazy_itable_init=0,lazy_journal_init=0 \
                -L EXTFS-MUT-EXT3 -d "$mutation_stage" "$image"
            ;;
        ext4)
            mke2fs -q -F -t ext4 -b 1024 \
                -O metadata_csum,^64bit,^flex_bg,^fast_commit,^orphan_file,^huge_file,^dir_nlink \
                -E lazy_itable_init=0,lazy_journal_init=0 \
                -L EXTFS-MUT-EXT4 -d "$mutation_stage" "$image"
            ;;
    esac

    "$tool" info "$image" > "$test_dir/$filesystem-mutation-info.txt"
    if ! e2fsck -f -n "$image" > "$test_dir/$filesystem-before-mutation-fsck.txt" 2>&1; then
        cat "$test_dir/$filesystem-before-mutation-fsck.txt" >&2
        echo "Initial $filesystem mutation image failed e2fsck." >&2
        exit 1
    fi

    cp "$fixture_dir/hello.txt" "$expected_growth"
    truncate -s "$growth_size" "$expected_growth"
    "$mutator" resize "$image" /hello.txt "$growth_size"
    if ! e2fsck -f -n "$image" > "$test_dir/$filesystem-after-growth-fsck.txt" 2>&1; then
        cat "$test_dir/$filesystem-after-growth-fsck.txt" >&2
        echo "$filesystem growth produced an inconsistent filesystem." >&2
        exit 1
    fi
    "$tool" extract "$image" /hello.txt "$extracted"
    cmp "$expected_growth" "$extracted"

    head -c 7 "$fixture_dir/hello.txt" > "$expected_shrink"
    "$mutator" resize "$image" /hello.txt 7
    if ! e2fsck -f -n "$image" > "$test_dir/$filesystem-after-shrink-fsck.txt" 2>&1; then
        cat "$test_dir/$filesystem-after-shrink-fsck.txt" >&2
        echo "$filesystem shrink produced an inconsistent filesystem." >&2
        exit 1
    fi
    "$tool" extract "$image" /hello.txt "$extracted"
    cmp "$expected_shrink" "$extracted"
done

corrupt_image="$test_dir/ext4-corrupt-superblock.img"
cp "$test_dir/ext4.img" "$corrupt_image"
printf 'Z' | dd of="$corrupt_image" bs=1 seek=1144 conv=notrunc status=none
if "$tool" info "$corrupt_image" > "$test_dir/corrupt-output.txt" 2>&1; then
    echo "Corrupted ext4 superblock was incorrectly accepted." >&2
    exit 1
fi
grep 'metadata checksum mismatch' "$test_dir/corrupt-output.txt" > /dev/null

echo "Ext2, ext3 and ext4 read/traversal and real-image resize tests passed."
