#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
from pathlib import Path
import sys

repo = Path(sys.argv[1])
native = repo / 'data/native-filesystems.tsv'
desktop = repo / 'data/desktop-filesystems.tsv'

native_rows = {}
for line in native.read_text().splitlines():
    if not line or line.startswith('#'):
        continue
    fields = line.split('\t')
    if len(fields) < 2:
        raise SystemExit(f'invalid native row: {line}')
    fs, module = fields[:2]
    if fs in native_rows:
        raise SystemExit(f'duplicate native filesystem: {fs}')
    native_rows[fs] = module

desktop_rows = {}
variants = set()
for line in desktop.read_text().splitlines():
    if not line or line.startswith('#'):
        continue
    fields = line.split('\t')
    if len(fields) != 2:
        raise SystemExit(f'invalid desktop row: {line}')
    fs, variant_text = fields
    if fs in desktop_rows:
        raise SystemExit(f'duplicate desktop filesystem: {fs}')
    if fs not in native_rows:
        raise SystemExit(f'desktop policy references unmanaged native filesystem: {fs}')
    variant = int(variant_text)
    if variant <= 0 or variant in variants:
        raise SystemExit(f'invalid/duplicate desktop variant: {variant}')
    desktop_rows[fs] = variant
    variants.add(variant)

if not desktop_rows:
    raise SystemExit('desktop manifest is empty')

helper = (repo / 'packaging/linux/native-module-helper.sh').read_text()
if 'desktop_kind=' in helper or '$3 == "amiga"' in helper:
    raise SystemExit('native module helper still owns desktop membership')
if 'is-managed "$filesystem"' not in helper:
    raise SystemExit('native module helper does not delegate desktop membership')

integration = (repo / 'packaging/linux/desktop-integration.sh').read_text()
if 'desktop-filesystems.tsv' not in integration:
    raise SystemExit('desktop integration does not own its manifest')
if 'native-filesystems.tsv' in integration:
    raise SystemExit('desktop integration still reads native deployment policy')
