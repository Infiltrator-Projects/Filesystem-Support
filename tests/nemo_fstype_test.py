#!/usr/bin/env python3
"""Verify the Nemo-local GIO bridge reports the actual OFS/FFS mount type."""
import os
from pathlib import Path
import platform
import subprocess
import sys

bridge, query, modules_root, mountinfo = map(Path, sys.argv[1:])
work = mountinfo.parent / 'nemo-fstype-fixture'
ofs_path = work / 'Amiga OFS test'
ffs_path = work / 'Amiga FFS test'
ofs_path.mkdir(parents=True, exist_ok=True)
ffs_path.mkdir(parents=True, exist_ok=True)
modules = modules_root / platform.release() / 'updates/infiltrator'
modules.mkdir(parents=True, exist_ok=True)
(modules / 'ofs.ko').touch()
(modules / 'ffs.ko').touch()

def escaped(path: Path) -> str:
    return str(path).replace('\\', r'\134').replace(' ', r'\040').replace('\t', r'\011').replace('\n', r'\012')

mountinfo.write_text(
    f'101 1 0:101 / {escaped(ofs_path)} rw,relatime - ofs /dev/fake-ofs rw\n'
    f'102 1 0:102 / {escaped(ffs_path)} rw,relatime - ffs /dev/fake-ffs rw\n'
)
env = dict(os.environ)
env['LD_PRELOAD'] = str(bridge)
for path, expected in ((ofs_path, 'ofs'), (ffs_path, 'ffs')):
    result = subprocess.run([str(query), str(path), expected], env=env,
                            text=True, capture_output=True)
    assert result.returncode == 0, (path, expected, result.stdout, result.stderr)
print('Nemo GIO filesystem::type bridge reports ofs and ffs from mountinfo: PASS')
