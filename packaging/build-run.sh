#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
[[ $# -eq 2 ]] || { echo "usage: $0 <main-package.deb> <output.run>" >&2; exit 2; }
deb=$(readlink -f "$1")
output=$2
root=$(cd "$(dirname "$0")/.." && pwd)
test "$(dpkg-deb -f "$deb" Package)" = infiltrator-filesystem-support
test "$(dpkg-deb -f "$deb" Version)" = "$(tr -d '[:space:]' < "$root/VERSION")"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cp "$deb" "$work/filesystem-support.deb"
tar -C "$work" -czf "$work/payload.tar.gz" filesystem-support.deb
cat > "$output" <<'STUB'
#!/usr/bin/env bash
set -euo pipefail
if [[ "${1:-}" == --help ]]; then
    cat <<'HELP'
Filesystem Support native installer
Installs the single infiltrator-filesystem-support Debian package.
Includes /usr/bin/fsinspect, the native installer, driver sources and inactive
per-filesystem desktop templates for project-native Amiga filesystems.
Each Install button deploys only its own driver and desktop files together.
The upgrade retires the obsolete separate desktop package.
Run normally; PolicyKit requests administrator authentication when needed.
HELP
    exit 0
fi
self=$(readlink -f "$0")
if [[ ${EUID} -ne 0 ]]; then
    command -v pkexec >/dev/null 2>&1 || { echo 'pkexec is required' >&2; exit 1; }
    exec pkexec "$self" --as-root
fi
[[ $# -eq 0 || ( $# -eq 1 && "$1" == --as-root ) ]] || { echo 'Refusing unexpected privileged invocation' >&2; exit 1; }
archive_line=$(awk '/^__ARCHIVE_BELOW__$/{print NR + 1; exit}' "$self")
[[ -n "$archive_line" ]] || exit 1
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
tail -n +"$archive_line" "$self" | tar -xz -C "$work"
apt-get -s install --no-install-recommends "$work/filesystem-support.deb" > "$work/plan"
if awk '$1 == "Remv" && $2 != "infiltrator-filesystem-support-udisks" { bad=1 } END { exit !bad }' "$work/plan"; then
    cat "$work/plan" >&2
    echo 'Refusing removal of an unrelated package' >&2
    exit 1
fi
apt-get install -y --no-install-recommends "$work/filesystem-support.deb"
echo 'Filesystem Support installed successfully.'
exit 0
__ARCHIVE_BELOW__
STUB
cat "$work/payload.tar.gz" >> "$output"
chmod 0755 "$output"
