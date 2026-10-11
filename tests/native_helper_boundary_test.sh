#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
set -eu

repo=${1:?repository root required}
coordinator="$repo/packaging/linux/native-module-helper.sh"
kernel_helper="$repo/packaging/linux/kernel-module-helper.sh"

[ -r "$coordinator" ]
[ -r "$kernel_helper" ]

python3 - "$coordinator" "$kernel_helper" <<'PY'
from pathlib import Path
import sys

coordinator = Path(sys.argv[1]).read_text()
kernel = Path(sys.argv[2]).read_text()

# The kernel transaction must be presentation-blind. These are architectural
# boundaries, not naming conventions: if any token returns, kernel rollback can
# once again become coupled to desktop state.
for token in (
    'desktop-integration',
    'refresh_desktop',
    'GNOME',
    'Nemo',
    'udev',
):
    if token in kernel:
        raise SystemExit(f'presentation concern leaked into kernel helper: {token}')

for token in (
    'modprobe',
    'depmod',
    'kernel_build',
    'sign_for_secure_boot',
    '/lib/modules/',
):
    if token not in kernel:
        raise SystemExit(f'kernel helper lost required kernel ownership: {token}')

# The coordinator may sequence transactions, but must not own implementation or
# rollback mechanics from either subsystem.
for token in (
    'kernel-module-helper',
    'desktop-integration',
    'is-managed',
    'kernel state remains committed',
):
    if token not in coordinator:
        raise SystemExit(f'missing coordinator boundary marker: {token}')

for token in (
    'modprobe',
    'depmod',
    'kmodsign',
    'sign-file',
    '/lib/modules/',
    'previous_copy',
    'restore_kind',
    'snapshot_kind',
):
    if token in coordinator:
        raise SystemExit(f'transaction implementation leaked into coordinator: {token}')

kernel_commit = coordinator.index('"$kernel_helper" "$action" "$filesystem" "$module"')
desktop_query = coordinator.index('"$desktop_helper" is-managed "$filesystem"')
if desktop_query <= kernel_commit:
    raise SystemExit('desktop transaction can run before the kernel transaction commits')

# Historical cross-boundary rollback language must never return.
for token in (
    'desktop installation failed; native installation was rolled back',
    'desktop removal failed; native module was restored',
):
    if token in coordinator or token in kernel:
        raise SystemExit(f'retired cross-boundary rollback returned: {token}')
PY
