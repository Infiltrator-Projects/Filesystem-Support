#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

if [[ $# -ne 2 ]]; then
    echo "usage: $0 <built-binary> <output.run>" >&2
    exit 2
fi

binary=$(readlink -f "$1")
output=$2
root=$(cd "$(dirname "$0")/.." && pwd)
desktop="$root/data/org.infiltrator.FilesystemSupport.desktop"

test -x "$binary"
test -f "$desktop"

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

mkdir -p "$tmp/payload/usr/local/bin"
mkdir -p "$tmp/payload/usr/local/share/applications"
install -m 0755 "$binary" "$tmp/payload/usr/local/bin/filesystem-support"
install -m 0644 "$desktop" "$tmp/payload/usr/local/share/applications/org.infiltrator.FilesystemSupport.desktop"

tar -C "$tmp/payload" -czf "$tmp/payload.tar.gz" .

cat > "$output" <<'STUB'
#!/usr/bin/env bash
set -euo pipefail

if [[ "${1:-}" == "--help" ]]; then
    cat <<'HELP'
Filesystem Support native installer

Installs:
  /usr/local/bin/filesystem-support
  /usr/local/share/applications/org.infiltrator.FilesystemSupport.desktop

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

install -m 0755 "$tmp/usr/local/bin/filesystem-support" /usr/local/bin/filesystem-support
install -m 0644     "$tmp/usr/local/share/applications/org.infiltrator.FilesystemSupport.desktop"     /usr/local/share/applications/org.infiltrator.FilesystemSupport.desktop

echo "Filesystem Support installed successfully."
exit 0
__ARCHIVE_BELOW__
STUB

cat "$tmp/payload.tar.gz" >> "$output"
chmod 0755 "$output"
