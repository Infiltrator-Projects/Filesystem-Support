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

# Stable identity must not be materialized from the Linux support catalogue.
identity_impl = (repo / 'src/platform_support.cpp').read_text()
if '#include "catalog.hpp"' in identity_impl:
    raise SystemExit('cross-platform identity still depends on Linux catalogue')
for linux_token in ('SupportProvider::', 'project_native_linux', 'linux_support_policies'):
    if linux_token in identity_impl:
        raise SystemExit(f'Linux policy leaked into identity implementation: {linux_token}')

catalog_impl = (repo / 'src/catalog.cpp').read_text()
for required in ('filesystem_identity.hpp', 'linux_support_policy.hpp'):
    if required not in catalog_impl:
        raise SystemExit(f'Linux catalogue is not joining explicit boundaries: {required}')

linux_policy = (repo / 'src/linux_support_policy.hpp').read_text()
for identity_field in ('description;', 'family;', 'std::string_view name;'):
    if identity_field in linux_policy:
        raise SystemExit(f'filesystem identity leaked into Linux policy: {identity_field}')
