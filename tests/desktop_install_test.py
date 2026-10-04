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
kinds = ('ofs', 'ffs', 'sfs', 'sfs2', 'pfs3')
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
(root / 'var/lib/dpkg').mkdir(parents=True)
(root / 'var/lib/dpkg/status').touch()
(root / 'usr/bin').mkdir(parents=True)
disks = root / 'usr/bin/gnome-disks'
nemo = root / 'usr/bin/nemo'
disks.symlink_to(sys.executable)
nemo.symlink_to(sys.executable)
display_args = (source / 'tests/udisks_display_test.py', '--preloaded',
                '--modules-root', root / 'lib/modules')
original_disks_hash = hashlib.sha256(disks.read_bytes()).hexdigest()
original_nemo_hash = hashlib.sha256(nemo.read_bytes()).hexdigest()

def run(*args, success=True, env=None):
    result = subprocess.run(list(map(str, args)), text=True, capture_output=True, env=env)
    assert (result.returncode == 0) == success, (args, result.stdout, result.stderr)
    return result

def owner(path):
    return run('dpkg-divert', f'--root={root}', '--listpackage', path).stdout.strip()

def check_files(selected):
    active = private / 'desktop/active'
    assert {p.stem for p in active.glob('*.so')} == selected
    rules = root / 'etc/udev/rules.d'
    assert {p.name.removeprefix('99-infiltrator-').removesuffix('.rules') for p in rules.glob('99-infiltrator-*.rules')} == selected
    assert not list(rules.glob('59-infiltrator-*.rules'))
    expected_owner = 'infiltrator-filesystem-support' if selected else ''
    assert owner('/usr/bin/gnome-disks') == expected_owner
    assert owner('/usr/bin/nemo') == expected_owner
    if not selected:
        assert hashlib.sha256(disks.read_bytes()).hexdigest() == original_disks_hash
        assert hashlib.sha256(nemo.read_bytes()).hexdigest() == original_nemo_hash

run(helper, 'sync')
check_files(set())
modules = root / 'lib/modules' / platform.release() / 'updates/infiltrator'
modules.mkdir(parents=True)
for kind in kinds: (modules / f'{kind}.ko').touch()
for mask in range(32):
    selected = {kind for bit, kind in enumerate(kinds) if mask & (1 << bit)}
    for kind in kinds: run(helper, 'install' if kind in selected else 'remove', kind)
    check_files(selected)
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
check_files(set())
for kind in ('sfs2', 'pfs3'): (modules / f'{kind}.ko').unlink()
legacy_rules = root / 'etc/udev/rules.d'
legacy_rules.mkdir(parents=True, exist_ok=True)
(legacy_rules / '59-infiltrator-ofs.rules').write_text('legacy\n')
run(helper, 'sync')
check_files({'ofs', 'ffs', 'sfs'})
for kind in ('ofs', 'ffs', 'sfs'):
    env = dict(os.environ, FSUPPORT_TEST_PRELOAD='unset')
    env.pop('LD_PRELOAD', None)
    run(nemo, source / 'tests/udisks_display_test.py', '--preloaded',
        '--gio-type', kind, env=env)
run(helper, 'purge')
check_files(set())
run('dpkg-divert', f'--root={root}', '--package', 'foreign-test-owner', '--add', '--rename',
    '--divert', '/usr/bin/gnome-disks.foreign', '/usr/bin/gnome-disks')
run(helper, 'install', 'ofs', success=False)
assert not list((private / 'desktop/active').glob('*.so'))
assert not list((root / 'etc/udev/rules.d').glob('99-infiltrator-*.rules'))
assert not list((root / 'etc/udev/rules.d').glob('59-infiltrator-*.rules'))
assert owner('/usr/bin/gnome-disks') == 'foreign-test-owner'
assert owner('/usr/bin/nemo') == ''
run('dpkg-divert', f'--root={root}', '--package', 'foreign-test-owner', '--remove', '--rename',
    '--divert', '/usr/bin/gnome-disks.foreign', '/usr/bin/gnome-disks')
check_files(set())
print('Per-filesystem desktop install/remove, all 32 file selections, GIO/Nemo identity, late-rule migration and rollback: PASS')
