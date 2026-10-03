#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

if [[ $# -ne 3 ]]; then
    echo "usage: $0 <built-binary> <output.run> <desktop-client.deb>" >&2
    exit 2
fi

binary=$(readlink -f "$1")
build_dir=$(dirname "$binary")
fsinspect="$build_dir/fsinspect"
output=$2
desktop_client=$(readlink -f "$3")
root=$(cd "$(dirname "$0")/.." && pwd)
desktop="$root/data/org.infiltrator.FilesystemSupport.desktop"
udev_rule="$root/packaging/linux/59-infiltrator-filesystems.rules"

test -x "$binary"
test -x "$fsinspect"
test -f "$desktop"
test -f "$udev_rule"
test "$(dpkg-deb -f "$desktop_client" Package)" = infiltrator-filesystem-support-udisks
test "$(dpkg-deb -f "$desktop_client" Version)" = "$(tr -d '[:space:]' < "$root/VERSION")"

 tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

mkdir -p "$tmp/payload/usr/local/bin"
mkdir -p "$tmp/payload/usr/bin"
mkdir -p "$tmp/payload/usr/local/share/applications"
mkdir -p "$tmp/payload/usr/local/lib/infiltrator-filesystem-support/native/filesystems"
mkdir -p "$tmp/payload/usr/lib/udev/rules.d"
mkdir -p "$tmp/payload/desktop-packages"
cp "$desktop_client" "$tmp/payload/desktop-packages/desktop-client.deb"
install -m 0755 "$binary" "$tmp/payload/usr/local/bin/filesystem-support"
install -m 0755 "$fsinspect" "$tmp/payload/usr/bin/fsinspect"
install -m 0644 "$desktop" "$tmp/payload/usr/local/share/applications/org.infiltrator.FilesystemSupport.desktop"
install -m 0644 "$udev_rule" "$tmp/payload/usr/lib/udev/rules.d/59-infiltrator-filesystems.rules"
install -m 0755 "$root/packaging/linux/native-module-helper.sh" \
    "$tmp/payload/usr/local/lib/infiltrator-filesystem-support/native-module-helper"

for native_fs in ext2 ext3 ext4 ofs ffs sfs sfs2 pfs3; do
    mkdir -p "$tmp/payload/usr/local/lib/infiltrator-filesystem-support/native/filesystems/$native_fs"
    cp -a "$root/native/filesystems/$native_fs/core" \
        "$tmp/payload/usr/local/lib/infiltrator-filesystem-support/native/filesystems/$native_fs/core"
    cp -a "$root/native/filesystems/$native_fs/linux" \
        "$tmp/payload/usr/local/lib/infiltrator-filesystem-support/native/filesystems/$native_fs/linux"
done

find "$tmp/payload/usr/local/lib/infiltrator-filesystem-support/native/filesystems" \
    -type f \( -name '*.o' -o -name '*.ko' -o -name '*.mod' -o -name '*.mod.c' \
    -o -name 'Module.symvers' -o -name 'modules.order' \) -delete

tar -C "$tmp/payload" -czf "$tmp/payload.tar.gz" .

cat > "$output" <<'STUB'
#!/usr/bin/env bash
set -euo pipefail

if [[ "${1:-}" == "--help" ]]; then
    cat <<'HELP'
Filesystem Support native installer

Installs:
  /usr/local/bin/filesystem-support
  /usr/bin/fsinspect
  /usr/lib/udev/rules.d/59-infiltrator-filesystems.rules
  /usr/local/share/applications/org.infiltrator.FilesystemSupport.desktop
  /usr/local/lib/infiltrator-filesystem-support/native-module-helper
  native module source for EXT2/EXT3/EXT4/OFS/FFS/SFS/SFS2/PFS3
  infiltrator-filesystem-support-udisks (managed UDisks client package)

The filesystem detector publishes OFS/FFS/SFS/SFS2/PFS3 ID_FS_* properties
for udev/UDisks so supported Amiga media is visible to desktop disk tools.
The bundled distribution-built UDisks client supplies the Amiga filesystem
names displayed by GNOME Disks in Contents and the volume map.

Run the file normally. PolicyKit will request administrator authentication if needed.
HELP
    exit 0
fi

self=$(readlink -f "$0")

if [[ ${EUID} -ne 0 ]]; then
    if ! command -v pkexec >/dev/null 2>&1; then
        echo "pkexec is required to install Filesystem Support." >&2
        exit 1
    fi
    exec pkexec "$self" --as-root
fi

if [[ "${1:-}" != "--as-root" ]]; then
    echo "Refusing unexpected privileged invocation." >&2
    exit 1
fi

archive_line=$(awk '/^__ARCHIVE_BELOW__$/{print NR + 1; exit}' "$self")
if [[ -z "$archive_line" ]]; then
    echo "Installer payload marker is missing." >&2
    exit 1
fi

 tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

tail -n +"$archive_line" "$self" | tar -xz -C "$tmp"

# Let APT own the ABI-matched library replacement and dependency transaction.
# Install it before copying the application, so failure cannot be reported as
# a successful installation with a missing desktop display-name component.
apt-get install -y --no-install-recommends "$tmp/desktop-packages/desktop-client.deb"

install -m 0755 "$tmp/usr/local/bin/filesystem-support" /usr/local/bin/filesystem-support
install -d -m 0755 /usr/bin
install -m 0755 "$tmp/usr/bin/fsinspect" /usr/bin/fsinspect
install -d -m 0755 /usr/lib/udev/rules.d
install -m 0644 \
    "$tmp/usr/lib/udev/rules.d/59-infiltrator-filesystems.rules" \
    /usr/lib/udev/rules.d/59-infiltrator-filesystems.rules
install -d -m 0755 /usr/local/share/applications
install -m 0644 \
    "$tmp/usr/local/share/applications/org.infiltrator.FilesystemSupport.desktop" \
    /usr/local/share/applications/org.infiltrator.FilesystemSupport.desktop

install -d -m 0755 /usr/local/lib/infiltrator-filesystem-support
install -m 0755 \
    "$tmp/usr/local/lib/infiltrator-filesystem-support/native-module-helper" \
    /usr/local/lib/infiltrator-filesystem-support/native-module-helper
rm -rf /usr/local/lib/infiltrator-filesystem-support/native
cp -a "$tmp/usr/local/lib/infiltrator-filesystem-support/native" \
    /usr/local/lib/infiltrator-filesystem-support/native

if command -v udevadm >/dev/null 2>&1; then
    udevadm control --reload-rules || true
    udevadm trigger --subsystem-match=block --action=change || true
    udevadm settle --timeout=30 || true
fi

echo "Filesystem Support installed successfully."
exit 0
__ARCHIVE_BELOW__
STUB

cat "$tmp/payload.tar.gz" >> "$output"
chmod 0755 "$output"
