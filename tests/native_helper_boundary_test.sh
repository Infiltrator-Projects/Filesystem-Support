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

# All native module mutations share depmod and module-selection state. The
# kernel authority itself must serialize that state; putting the lock in the
# UI or coordinator would leave direct helper/prerm paths outside the contract.
for required in (
    'kernel_lock_path=/run/lock/infiltrator-filesystem-support-kernel.lock',
    'acquire_kernel_mutation_lock()',
    'flock -x 9',
    'export FILESYSTEM_SUPPORT_KERNEL_LOCK_FD=9',
    '[[ $(readlink /proc/$$/fd/9 2>/dev/null || true) == "$kernel_lock_path" ]]',
):
    if required not in kernel:
        raise SystemExit(f'kernel mutation serialization lost boundary: {required}')
if kernel.count('acquire_kernel_mutation_lock') < 3:
    raise SystemExit('kernel mutation lock is not applied to remove-all and direct mutations')

# Read-only observation belongs to the kernel authority and must remain usable
# by the unprivileged manager. The coordinator forwards the query rather than
# reconstructing deployment state or requiring an authentication prompt.
for required in (
    'if [[ $# -eq 2 && $1 == status ]]; then',
    'native_source_digest_for()',
    'source_stamp="$destination.source-sha256"',
    "printf 'installed\\t%s\\t%s\\n'",
):
    if required not in kernel:
        raise SystemExit(f'kernel state observation lost contract: {required}')
if coordinator_code.index('if [[ $# -eq 2 && ( $1 == is-installed || $1 == status ) ]]') > coordinator_code.index('[[ ${EUID} -eq 0 ]]'):
    raise SystemExit('read-only native state query was moved behind the privilege gate')
for forbidden in ('/lib/modules/', '.source-sha256', 'native_source_digest_for'):
    if forbidden in coordinator_code:
        raise SystemExit(f'kernel state policy leaked into coordinator: {forbidden}')

# Secure-Boot key discovery follows the declared Debian baseline and accepts
# Ubuntu/Mint's distinct shim-signed location only as a derivative fallback.
# The helper consumes an existing distribution signing identity; it must not
# manufacture another private-key authority inside Filesystem Support.
debian_key = '/var/lib/dkms/mok.key'
debian_cert = '/var/lib/dkms/mok.pub'
derivative_key = '/var/lib/shim-signed/mok/MOK.priv'
derivative_cert = '/var/lib/shim-signed/mok/MOK.der'
for token in (debian_key, debian_cert, derivative_key, derivative_cert):
    if token not in kernel_code:
        raise SystemExit(f'kernel helper lost supported Secure-Boot identity: {token}')
if kernel_code.index(debian_key) > kernel_code.index(derivative_key):
    raise SystemExit('Debian Secure-Boot identity is not the primary signing convention')
for forbidden in (
    '/var/lib/infiltrator-filesystem-support/mok',
    'openssl req',
    'openssl genpkey',
):
    if forbidden in kernel_code:
        raise SystemExit(f'kernel helper created a competing signing authority: {forbidden}')

# The deployment manifest schema is exactly filesystem/module. Accepting extra
# columns here would create a hidden compatibility schema distinct from CMake.
if 'NF == 2 && $1 == fs && $2 == mod' not in kernel_code:
    raise SystemExit('kernel helper does not enforce the exact native manifest schema')

# Once old runtime state is unloaded, every persistent mutation and dependency
# refresh is guarded by the same restoration path. The source provenance stamp
# is part of that persistent transaction; it may never describe different bytes
# from the module image after either success or rollback.
for required in (
    'fail_with_restore()',
    'restore_previous_state',
    'local restore_runtime=$5',
    'previous_stamp_copy',
    'if ! install -m 0644 "$built" "$destination"; then',
    'if ! install -m 0644 "$tmp/current.source-sha256" "$source_stamp"; then',
    'if ! depmod -a "$kernel"; then',
    'if ! modprobe "$module"; then',
    'if ! rm -f "$destination" "$source_stamp"; then',
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
    'infiltrator-filesystem-support-kernel.lock',
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
