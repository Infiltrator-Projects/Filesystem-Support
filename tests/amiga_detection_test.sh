#!/bin/sh
set -eu

if [ "$#" -ne 3 ]; then
    echo "usage: $0 <fsinspect> <python> <source-root>" >&2
    exit 2
fi

inspect=$1
python=$2
source_root=$3
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT HUP INT TERM

"$python" - "$work" <<'PY'
from pathlib import Path
import struct
import sys

out = Path(sys.argv[1])

for name, signature in (("ofs.img", b"DOS\x00"), ("ffs.img", b"DOS\x01")):
    image = bytearray(2 * 1024 * 1024)
    image[0:4] = signature
    (out / name).write_bytes(image)

# Minimal PFS\1 root record accepted by the canonical PFS3 geometry validator.
sectors = 4096
image = bytearray(sectors * 512)
root = memoryview(image)[2 * 512:3 * 512]
struct.pack_into(">I", root, 0, 0x50465301)   # PFS\1
struct.pack_into(">I", root, 4, 0x00000017)   # harddisk, split anodes, dir extension, size field
name = b"PFS Test"
root[20] = len(name)
root[21:21 + len(name)] = name
struct.pack_into(">I", root, 52, 33)           # last reserved sector
struct.pack_into(">I", root, 56, 2)            # first reserved sector
struct.pack_into(">I", root, 60, 8)            # free reserved blocks
struct.pack_into(">H", root, 64, 1024)         # reserved block size
struct.pack_into(">H", root, 66, 2)            # root block cluster
struct.pack_into(">I", root, 68, 4000)         # free data blocks
struct.pack_into(">I", root, 72, 10)           # always-free blocks
struct.pack_into(">I", root, 76, 34)           # roving pointer
struct.pack_into(">I", root, 80, 0)            # delete directory
struct.pack_into(">I", root, 84, sectors)      # media size in sectors
struct.pack_into(">I", root, 88, 0)            # no extension block
(out / "pfs3.img").write_bytes(image)

(out / "unknown.img").write_bytes(bytes(2 * 1024 * 1024))
PY

"$python" "$source_root/tools/generate-sfs-runtime-images.py" \
    --variant sfs --output "$work/sfs.img" --blocks 4096 --volume DetectSFS
"$python" "$source_root/tools/generate-sfs-runtime-images.py" \
    --variant sfs2 --output "$work/sfs2.img" --blocks 4096 --volume DetectSFS2

check_type() {
    image=$1
    expected_type=$2
    expected_version=$3
    output=$($inspect --udev "$image")
    printf '%s\n' "$output" | grep -Fxq 'ID_FS_USAGE=filesystem'
    printf '%s\n' "$output" | grep -Fxq "ID_FS_TYPE=$expected_type"
    printf '%s\n' "$output" | grep -Fxq "ID_FS_VERSION=$expected_version"
}

check_type "$work/ofs.img" ofs DOS/0
check_type "$work/ffs.img" ffs DOS/1
check_type "$work/sfs.img" sfs 3
check_type "$work/sfs2.img" sfs2 4
check_type "$work/pfs3.img" pfs3 PFS/1

pfs_output=$($inspect --udev "$work/pfs3.img")
printf '%s\n' "$pfs_output" | grep -Fxq 'ID_FS_LABEL=PFS Test'

if $inspect --udev "$work/unknown.img" >"$work/unknown.out" 2>"$work/unknown.err"; then
    echo "unknown media was incorrectly identified" >&2
    exit 1
fi
if [ -s "$work/unknown.out" ]; then
    echo "udev mode emitted properties for unknown media" >&2
    cat "$work/unknown.out" >&2
    exit 1
fi

echo "Amiga filesystem desktop detection: PASS"
