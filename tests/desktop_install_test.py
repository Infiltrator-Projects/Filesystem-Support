#!/usr/bin/env python3
"""Run the real desktop installer in a private filesystem and dpkg database."""
import hashlib
import os
from pathlib import Path
import platform
import shlex
import shutil
import subprocess
import sys

source, build = map(Path, sys.argv[1:])
root = build / 'desktop-test-root'
if root.exists(): shutil.rmtree(root)
private = root / 'usr/lib/infiltrator-filesystem-support'
templates = private / 'desktop/templates'
templates.mkdir(parents=True)
manifest = source / 'data/desktop-filesystems.tsv'
rows = []
for line in manifest.read_text().splitlines():
    if not line or line.startswith('#'):
        continue
    fields = line.split('\t')
    assert len(fields) == 2, fields
    rows.append(fields)
kinds = tuple(fields[0] for fields in rows)
assert kinds
shutil.copy(manifest, private / 'desktop-filesystems.tsv')
for kind in kinds:
    shutil.copy(build / f'desktop-test-templates/{kind}.so', templates / f'{kind}.so')
    shutil.copy(source / f'packaging/linux/desktop-rules/{kind}.rules', templates / f'{kind}.rules')
shutil.copy(source / 'packaging/linux/desktop-rules/cleanup.rules', templates / 'cleanup.rules')

def fixture_script(source_file, destination):
    code = source_file.read_text()
    assert code.count('system_root=/\n') == 1
    destination.write_text(code.replace('system_root=/\n', 'system_root=' + shlex.quote(str(root)) + '\n'))
    destination.chmod(0o755)

fixture_script(source / 'packaging/linux/gnome-disks-wrapper', templates / 'gnome-disks')
fixture_script(source / 'packaging/linux/nemo-wrapper', templates / 'nemo')
helper = private / 'desktop-integration'
fixture_script(source / 'packaging/linux/desktop-integration.sh', helper)

# Desktop recovery must not infer a kernel-module filename from filesystem
# identity. Give every test filesystem a deliberately different module name and
# expose only the native coordinator's installed-state contract to the desktop
# helper. Any return of "$kind.ko" coupling therefore fails this integration
# test even though current production module names happen to equal their IDs.
module_by_kind = {kind: f'fixture_{kind}_native' for kind in kinds}
native_state = private / 'native-module-helper'
case_rows = '\n'.join(
    f'        {kind}) module={shlex.quote(module)} ;;'
    for kind, module in module_by_kind.items()
)
native_state.write_text(
    '#!/usr/bin/env bash\n'
    'set -euo pipefail\n'
    '[[ $# -eq 2 && $1 == is-installed ]] || exit 2\n'
    'case "$2" in\n'
    f'{case_rows}\n'
    '        *) exit 2 ;;\n'
    'esac\n'
    f'path={shlex.quote(str(root))}/lib/modules/$(uname -r)/updates/infiltrator/$module.ko\n'
    '[[ -f "$path" ]] && exit 0\n'
    'exit 3\n'
)
native_state.chmod(0o755)

(root / 'var/lib/dpkg').mkdir(parents=True)
(root / 'var/lib/dpkg/status').touch()
(root / 'usr/bin').mkdir(parents=True)
disks = root / 'usr/bin/gnome-disks'
nemo = root / 'usr/bin/nemo'
disks.symlink_to(sys.executable)
nemo.symlink_to(sys.executable)
display_args = (source / 'tests/udisks_display_test.py', '--preloaded')
original_disks_hash = hashlib.sha256(disks.read_bytes()).hexdigest()
original_nemo_hash = hashlib.sha256(nemo.read_bytes()).hexdigest()

def run(*args, success=True, env=None):
    result = subprocess.run(list(map(str, args)), text=True, capture_output=True, env=env)
    assert (result.returncode == 0) == success, (args, result.stdout, result.stderr)
    return result

def owner(path):
    return run('dpkg-divert', f'--root={root}', '--listpackage', path).stdout.strip()

def check_files(selected, prepared=True):
    active = private / 'desktop/active'
    assert {p.stem for p in active.glob('*.so')} == selected
    rules = root / 'etc/udev/rules.d'
    assert {p.name.removeprefix('99-infiltrator-').removesuffix('.rules') for p in rules.glob('99-infiltrator-*.rules')} == selected
    assert not list(rules.glob('59-infiltrator-*.rules'))

    if prepared:
        assert owner('/usr/bin/gnome-disks') == 'infiltrator-filesystem-support'
        assert owner('/usr/bin/nemo') == 'infiltrator-filesystem-support'
        assert (root / 'usr/bin/gnome-disks.filesystem-support-original').exists()
        assert (root / 'usr/bin/nemo.filesystem-support-original').exists()
    else:
        assert owner('/usr/bin/gnome-disks') == ''
        assert owner('/usr/bin/nemo') == ''
        assert hashlib.sha256(disks.read_bytes()).hexdigest() == original_disks_hash
        assert hashlib.sha256(nemo.read_bytes()).hexdigest() == original_nemo_hash

# Package-level sync owns the shared launchers even when no native Amiga
# filesystem is active. Per-filesystem operations below must never toggle them.
# Keep this exact-SHA integration test in the release qualification path so
# Amiga desktop changes cannot be published without their runtime gate running.
run(helper, 'sync')
check_files(set(), prepared=True)

modules = root / 'lib/modules' / platform.release() / 'updates/infiltrator'
modules.mkdir(parents=True)
for kind in kinds:
    (modules / f'{module_by_kind[kind]}.ko').touch()

for mask in range(1 << len(kinds)):
    selected = {kind for bit, kind in enumerate(kinds) if mask & (1 << bit)}
    for kind in kinds:
        run(helper, 'install' if kind in selected else 'remove', kind)
    check_files(selected, prepared=True)
    enabled = ','.join(sorted(selected))
    env = dict(os.environ, FSUPPORT_TEST_PRELOAD='unset')
    env.pop('LD_PRELOAD', None)
    run(disks, *display_args, '--enabled', enabled, env=env)
    env = dict(os.environ, LD_PRELOAD='libm.so.6', FSUPPORT_TEST_PRELOAD='libm.so.6')
    run(disks, *display_args, '--enabled', enabled, env=env)
    for kind in sorted(selected):
        env = dict(os.environ, FSUPPORT_TEST_PRELOAD='unset')
        env.pop('LD_PRELOAD', None)
        run(nemo, source / 'tests/udisks_display_test.py', '--preloaded',
            '--gio-type', kind, env=env)

run(helper, 'purge')
check_files(set(), prepared=True)

for kind in kinds[-2:]:
    (modules / f'{module_by_kind[kind]}.ko').unlink()
legacy_rules = root / 'etc/udev/rules.d'
legacy_rules.mkdir(parents=True, exist_ok=True)
(legacy_rules / '59-infiltrator-ofs.rules').write_text('legacy\n')
run(helper, 'sync')
check_files(set(kinds[:-2]), prepared=True)
for kind in kinds[:-2]:
    env = dict(os.environ, FSUPPORT_TEST_PRELOAD='unset')
    env.pop('LD_PRELOAD', None)
    run(nemo, source / 'tests/udisks_display_test.py', '--preloaded',
        '--gio-type', kind, env=env)

run(helper, 'purge')
check_files(set(), prepared=True)

# Package removal is the only operation that tears down shared diversions.
run(helper, 'unprepare')
check_files(set(), prepared=False)

# An individual filesystem install must not recreate shared launcher state.
run(helper, 'install', kinds[0], success=False)
check_files(set(), prepared=False)

# A foreign Disks diversion blocks package-level preparation before any
# filesystem state is touched.
run('dpkg-divert', f'--root={root}', '--package', 'foreign-test-owner', '--add', '--rename',
    '--divert', '/usr/bin/gnome-disks.foreign', '/usr/bin/gnome-disks')
run(helper, 'sync', success=False)
assert not list((private / 'desktop/active').glob('*.so'))
assert not list((root / 'etc/udev/rules.d').glob('99-infiltrator-*.rules'))
assert not list((root / 'etc/udev/rules.d').glob('59-infiltrator-*.rules'))
assert owner('/usr/bin/gnome-disks') == 'foreign-test-owner'
assert owner('/usr/bin/nemo') == ''
run('dpkg-divert', f'--root={root}', '--package', 'foreign-test-owner', '--remove', '--rename',
    '--divert', '/usr/bin/gnome-disks.foreign', '/usr/bin/gnome-disks')
check_files(set(), prepared=False)

# A conflict discovered at the second shared launcher must not leave the first
# launcher diverted. This is the package-level transaction boundary that was
# historically vulnerable to half-prepared configuration.
run('dpkg-divert', f'--root={root}', '--package', 'foreign-test-owner', '--add', '--rename',
    '--divert', '/usr/bin/nemo.foreign', '/usr/bin/nemo')
run(helper, 'sync', success=False)
assert owner('/usr/bin/gnome-disks') == ''
assert owner('/usr/bin/nemo') == 'foreign-test-owner'
assert hashlib.sha256(disks.read_bytes()).hexdigest() == original_disks_hash
assert not list((private / 'desktop/active').glob('*.so'))
assert not list((root / 'etc/udev/rules.d').glob('99-infiltrator-*.rules'))
run('dpkg-divert', f'--root={root}', '--package', 'foreign-test-owner', '--remove', '--rename',
    '--divert', '/usr/bin/nemo.foreign', '/usr/bin/nemo')
check_files(set(), prepared=False)

# Re-prepare, prove one filesystem transaction leaves launcher ownership alone,
# then return the fixture to stock state.
run(helper, 'sync')
check_files(set(kinds[:-2]), prepared=True)
run(helper, 'remove', kinds[0])
assert owner('/usr/bin/gnome-disks') == 'infiltrator-filesystem-support'
assert owner('/usr/bin/nemo') == 'infiltrator-filesystem-support'
run(helper, 'purge')
run(helper, 'unprepare')
check_files(set(), prepared=False)

print(
    f'Package-scoped launchers plus isolated per-filesystem desktop transactions, '
    f'all {1 << len(kinds)} selections, GIO/Nemo identity, migration and rollback: PASS'
)
