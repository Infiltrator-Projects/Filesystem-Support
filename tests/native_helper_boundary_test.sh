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

# Boundary checks inspect executable shell, not comments. Documentation may
# describe the boundary; only executable ownership is forbidden to cross it.
def shell_code(text: str) -> str:
    return '\n'.join(
        line for line in text.splitlines()
        if not line.lstrip().startswith('#')
    )

coordinator_code = shell_code(coordinator)
kernel_code = shell_code(kernel)

# Privileged helpers must not inherit an arbitrary caller command search path.
trusted_path = 'PATH=/usr/sbin:/usr/bin:/sbin:/bin'
for name, source in (('coordinator', coordinator), ('kernel helper', kernel)):
    if trusted_path not in source or 'export PATH' not in source:
        raise SystemExit(f'{name} does not pin the privileged command search path')

# The kernel transaction must be presentation-blind. These are architectural
# boundaries, not naming conventions: if executable references return, kernel
# rollback can once again become coupled to presentation state.
for token in (
    'desktop-integration',
    'refresh_desktop',
    'GNOME',
    'Nemo',
    'udev',
):
    if token in kernel_code:
        raise SystemExit(f'presentation concern leaked into kernel helper: {token}')

for token in (
    'modprobe',
    'depmod',
    'kernel_build',
    'sign_for_secure_boot',
    '/lib/modules/',
):
    if token not in kernel_code:
        raise SystemExit(f'kernel helper lost required kernel ownership: {token}')

# The deployment manifest schema is exactly filesystem/module. Accepting extra
# columns here would create a hidden compatibility schema distinct from CMake.
if 'NF == 2 && $1 == fs && $2 == mod' not in kernel_code:
    raise SystemExit('kernel helper does not enforce the exact native manifest schema')

# Once old runtime state is unloaded, every persistent mutation and dependency
# refresh is guarded by the same restoration path. Install and removal both own
# recovery instead of relying on set -e to abandon a half-applied transaction.
for required in (
    'fail_with_restore()',
    'restore_previous_state',
    'if ! install -m 0644 "$built" "$destination"; then',
    'if ! depmod -a "$kernel"; then',
    'if ! modprobe "$module"; then',
    'if ! rm -f "$destination"; then',
):
    if required not in kernel:
        raise SystemExit(f'kernel transaction lost recovery boundary: {required}')

# The coordinator may sequence transactions, but must not own implementation or
# rollback mechanics from either subsystem.
for token in (
    'kernel-module-helper',
    'desktop-integration',
    'is-managed',
    'kernel state remains committed',
):
    if token not in coordinator_code:
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
    if token in coordinator_code:
        raise SystemExit(f'transaction implementation leaked into coordinator: {token}')

kernel_commit = coordinator_code.index('"$kernel_helper" "$action" "$filesystem" "$module"')
desktop_query = coordinator_code.index('"$desktop_helper" is-managed "$filesystem"')
if desktop_query <= kernel_commit:
    raise SystemExit('desktop transaction can run before the kernel transaction commits')

# Historical cross-boundary rollback language must never return to executable
# behaviour or failure reporting.
for token in (
    'desktop installation failed; native installation was rolled back',
    'desktop removal failed; native module was restored',
):
    if token in coordinator_code or token in kernel_code:
        raise SystemExit(f'retired cross-boundary rollback returned: {token}')
PY
