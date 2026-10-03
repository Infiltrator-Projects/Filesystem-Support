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
helper = private / 'desktop-integration'
fixture_script(source / 'packaging/linux/desktop-integration.sh', helper)
(root / 'var/lib/dpkg').mkdir(parents=True)
(root / 'var/lib/dpkg/status').touch()
(root / 'usr/bin').mkdir(parents=True)
disks = root / 'usr/bin/gnome-disks'
disks.symlink_to(sys.executable)
display_args = (source / 'tests/udisks_display_test.py', '--preloaded',
                '--modules-root', root / 'lib/modules')
original_hash = hashlib.sha256(disks.read_bytes()).hexdigest()
def run(*args, success=True, env=None):
    result = subprocess.run(list(map(str, args)), text=True, capture_output=True, env=env)
    assert (result.returncode == 0) == success, (args, result.stdout, result.stderr)
    return result
def owner():
    return run('dpkg-divert', f'--root={root}', '--listpackage', '/usr/bin/gnome-disks').stdout.strip()
def check_files(selected):
    active = private / 'desktop/active'
    assert {p.stem for p in active.glob('*.so')} == selected
    rules = root / 'etc/udev/rules.d'
    assert {p.name.removeprefix('59-infiltrator-').removesuffix('.rules') for p in rules.glob('59-infiltrator-*.rules')} == selected
    assert owner() == ('infiltrator-filesystem-support' if selected else '')
    if not selected:
        assert hashlib.sha256(disks.read_bytes()).hexdigest() == original_hash
run(helper, 'sync')
check_files(set())
modules = root / 'lib/modules' / platform.release() / 'updates/infiltrator'
modules.mkdir(parents=True)
# All five drivers are present throughout this matrix. Only the selected
# desktop files may enable names; one installed file cannot name another type.
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
run(helper, 'purge')
check_files(set())
# Migrate only drivers actually installed for this running kernel.
for kind in ('sfs2', 'pfs3'): (modules / f'{kind}.ko').unlink()
run(helper, 'sync')
check_files({'ofs', 'ffs', 'sfs'})
run(helper, 'purge')
check_files(set())
# A foreign diversion must survive a failed paired installation unchanged.
run('dpkg-divert', f'--root={root}', '--package', 'foreign-test-owner', '--add', '--rename',
    '--divert', '/usr/bin/gnome-disks.foreign', '/usr/bin/gnome-disks')
run(helper, 'install', 'ofs', success=False)
assert not list((private / 'desktop/active').glob('*.so'))
assert not list((root / 'etc/udev/rules.d').glob('59-infiltrator-*.rules'))
assert owner() == 'foreign-test-owner'
run('dpkg-divert', f'--root={root}', '--package', 'foreign-test-owner', '--remove', '--rename',
    '--divert', '/usr/bin/gnome-disks.foreign', '/usr/bin/gnome-disks')
check_files(set())
print('Per-filesystem desktop install/remove, all 32 file selections, migration and rollback: PASS')
