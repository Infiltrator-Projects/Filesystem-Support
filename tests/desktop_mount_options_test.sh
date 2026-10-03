#!/usr/bin/env bash
# Verify the installed udev rules preserve UDisks caller ownership placeholders.
# This runs only on a disposable CI host; it does not mount or modify user media.
set -euo pipefail
[[ ${EUID} -eq 0 && $# -eq 1 ]]
source_root=$(realpath "$1")
work=$(mktemp -d)
module_dir="/lib/modules/$(uname -r)/updates/infiltrator"
helper=/usr/lib/infiltrator-filesystem-support/desktop-integration
kind=
loop=
cleanup() {
    [[ -z "$loop" ]] || losetup -d "$loop"
    if [[ -n "$kind" ]]; then
        rm -f "$module_dir/$kind.ko"
        "$helper" remove "$kind"
    fi
    rm -rf "$work"
}
trap cleanup EXIT
mkdir -p "$module_dir"
for filesystem in ofs ffs sfs sfs2; do
    test ! -e "$module_dir/$filesystem.ko"
done
python3 - "$work" <<'PY'
from pathlib import Path
import struct
import sys
out=Path(sys.argv[1])
for kind, signature in (('ofs', b'DOS\0'), ('ffs', b'DOS\1')):
    image=bytearray(4096*512)
    image[:4]=signature
    root=memoryview(image)[2048*512:2049*512]
    struct.pack_into('>I',root,0,2)
    struct.pack_into('>I',root,508,1)
    struct.pack_into('>I',root,20,(-3)&0xffffffff)
    (out/f'{kind}.img').write_bytes(image)
PY
for filesystem in sfs sfs2; do
    python3 "$source_root/tools/generate-sfs-runtime-images.py" \
        --variant "$filesystem" --output "$work/$filesystem.img" \
        --blocks 4096 --volume DesktopOwnership
 done
for filesystem in ofs ffs sfs sfs2; do
    kind=$filesystem
    touch "$module_dir/$kind.ko"
    "$helper" install "$kind"
    loop=$(losetup --find --show "$work/$kind.img")
    sys_path=$(udevadm info --query=path --name="$loop")
    udevadm test --action=change "/sys$sys_path" > "$work/udev.out" 2>&1
    python3 - "$kind" "$work/udev.out" <<'PY'
from pathlib import Path
import sys
kind, output=sys.argv[1:]
properties={}
for line in Path(output).read_text().splitlines():
    key, sep, value=line.partition('=')
    if sep and key in ('ID_FS_TYPE','UDISKS_MOUNT_OPTIONS_DEFAULTS','UDISKS_MOUNT_OPTIONS_ALLOW'):
        properties[key]=value
assert properties.get('ID_FS_TYPE')==kind, properties
for key in ('UDISKS_MOUNT_OPTIONS_DEFAULTS','UDISKS_MOUNT_OPTIONS_ALLOW'):
    options=set(properties.get(key,'').split(','))
    assert {'setuid=$UID','setgid=$GID'}.issubset(options), properties
    assert not any('$$' in option for option in options), properties
    assert not any(option in options for option in ('setuid','setgid','setuid=0','setgid=0')), properties
print(f'{kind}: real udev processing preserves caller UID/GID options: PASS')
PY
    losetup -d "$loop"
    loop=
    rm "$module_dir/$kind.ko"
    "$helper" remove "$kind"
    kind=
done
