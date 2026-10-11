#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
set -eu

repo=${1:?repository root required}
helper="$repo/packaging/linux/native-module-helper.sh"

[ -r "$helper" ]

# Desktop integration is allowed only after the kernel transaction has reached
# its committed success path. It must never be part of a rollback branch.
python3 - "$helper" <<'PY'
from pathlib import Path
import sys

text = Path(sys.argv[1]).read_text()

required = [
    'refresh_desktop_after_commit install',
    'refresh_desktop_after_commit remove',
    'kernel state was intentionally left committed',
]
for token in required:
    if token not in text:
        raise SystemExit(f"missing orchestration boundary marker: {token}")

# The retired coupling rolled kernel state back when desktop integration failed.
for token in [
    'desktop installation failed; native installation was rolled back',
    'desktop removal failed; native module was restored',
]:
    if token in text:
        raise SystemExit(f"retired cross-boundary rollback returned: {token}")

# Keep the desktop follow-up after the success/commit message in each action.
install_commit = text.index('native module installed and loaded for kernel')
install_desktop = text.index('refresh_desktop_after_commit install')
if install_desktop <= install_commit:
    raise SystemExit('desktop install runs before native install commit')

remove_commit = text.index('native module removed for kernel')
remove_desktop = text.index('refresh_desktop_after_commit remove', remove_commit)
if remove_desktop <= remove_commit:
    raise SystemExit('desktop removal runs before native removal commit')
PY
