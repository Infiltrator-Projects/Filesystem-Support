#!/bin/sh
set -eu

if [ "$#" -ne 4 ]; then
    echo "usage: $0 <fsinspect> <python> <source-root> <test-modules-root>" >&2
    exit 2
fi

inspect=$1
python=$2
source_root=$3
modules_root=$4
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT HUP INT TERM

"$python" - "$work" <<'PY'
from pathlib import Path
import struct
import sys

out = Path(sys.argv[1])


def write_affs(path: Path, signature: bytes) -> None:
    block_size = 512
    blocks = 4096
    image = bytearray(blocks * block_size)
    image[0:4] = signature

    # Match the kernel adapter's default root search. A valid AFFS root has
    # T_SHORT at the front, ST_ROOT at the final word and a zero additive
    # big-endian checksum across the complete block.
    root_number = (2 + blocks - 1) // 2
    root = memoryview(image)[root_number * block_size:(root_number + 1) * block_size]
    struct.pack_into(">I", root, 0, 2)          # T_SHORT
    struct.pack_into(">I", root, block_size - 4, 1)  # ST_ROOT
    total = 0
    for offset in range(0, block_size, 4):
        total = (total + struct.unpack_from(">I", root, offset)[0]) & 0xFFFFFFFF
    struct.pack_into(">I", root, 20, (-total) & 0xFFFFFFFF)
    path.write_bytes(image)


write_affs(out / "ofs.img", b"DOS\x00")
write_affs(out / "ffs.img", b"DOS\x01")

# Signature-only media must not be accepted as OFS/FFS. This is the false
# positive that root/checksum validation is intended to prevent.
loose = bytearray(2 * 1024 * 1024)
loose[0:4] = b"DOS\x00"
(out / "signature-only.img").write_bytes(loose)

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

check_unknown() {
    image=$1
    if "$inspect" --udev "$image" >"$work/unknown.out" 2>"$work/unknown.err"; then
        echo "unknown media was incorrectly identified: $image" >&2
        exit 1
    fi
    if [ -s "$work/unknown.out" ]; then
        echo "udev mode emitted properties for unknown media: $image" >&2
        cat "$work/unknown.out" >&2
        exit 1
    fi
}

"$python" - "$inspect" "$work" "$modules_root" <<'CHECK'
from pathlib import Path
import platform
import subprocess
import sys
inspect, media, modules_root = sys.argv[1:]
modules = Path(modules_root) / platform.release() / "updates/infiltrator"
modules.mkdir(parents=True, exist_ok=True)
cases = (("ofs", "DOS/0"), ("ffs", "DOS/1"), ("sfs", "3"),
         ("sfs2", "4"), ("pfs3", "PFS/1"))
# Format validation in the explicit diagnostic command is independent of
# install state; it supplies no desktop properties or mounting capability.
for kind, version in cases:
    result = subprocess.run([inspect, str(Path(media) / f"{kind}.img")],
                            text=True, capture_output=True, check=True)
    assert f"filesystem={kind}\n" in result.stdout
# Every subset proves that one installed filesystem cannot activate another.
for mask in range(32):
    for bit, (kind, _) in enumerate(cases):
        module = modules / f"{kind}.ko"
        module.unlink(missing_ok=True)
        if mask & (1 << bit): module.touch()
    for bit, (kind, version) in enumerate(cases):
        result = subprocess.run([inspect, "--udev", str(Path(media) / f"{kind}.img")],
                                text=True, capture_output=True)
        if mask & (1 << bit):
            assert result.returncode == 0, (mask, kind, result.stderr)
            lines = result.stdout.splitlines()
            assert "ID_FS_USAGE=filesystem" in lines
            assert f"ID_FS_TYPE={kind}" in lines
            assert f"ID_FS_VERSION={version}" in lines
            if kind == "pfs3": assert "ID_FS_LABEL=PFS Test" in lines
        else:
            assert result.returncode != 0 and not result.stdout, (mask, kind, result.stdout)
print("All 32 Amiga driver installation subsets: PASS")
CHECK

check_unknown "$work/signature-only.img"
check_unknown "$work/unknown.img"

echo "Amiga filesystem desktop detection: PASS"
