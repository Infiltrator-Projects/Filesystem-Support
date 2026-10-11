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

coordinator = (repo / 'packaging/linux/native-module-helper.sh').read_text()
kernel_helper = (repo / 'packaging/linux/kernel-module-helper.sh').read_text()

if 'desktop_kind=' in coordinator or '$3 == "amiga"' in coordinator:
    raise SystemExit('native coordinator owns desktop membership')
if 'is-managed "$filesystem"' not in coordinator:
    raise SystemExit('native coordinator does not delegate desktop membership')
if 'kernel-module-helper' not in coordinator:
    raise SystemExit('native coordinator does not delegate kernel transaction ownership')

for token in ('desktop-integration', 'GNOME', 'Nemo', 'udev', 'is-managed'):
    if token in kernel_helper:
        raise SystemExit(f'kernel helper depends on desktop concern: {token}')
for token in ('modprobe', 'depmod', '/lib/modules/', 'sign_for_secure_boot'):
    if token in coordinator:
        raise SystemExit(f'kernel transaction logic leaked into coordinator: {token}')

# Both halves of the split transaction must ship in the same package. This
# prevents a source-only refactor where the coordinator is installed but its
# kernel transaction implementation is accidentally omitted.
cmake = (repo / 'CMakeLists.txt').read_text()
for helper in (
    'packaging/linux/native-module-helper.sh',
    'packaging/linux/kernel-module-helper.sh',
):
    if helper not in cmake:
        raise SystemExit(f'packaging does not install required helper: {helper}')

for relative in (
    'packaging/linux/desktop-integration.sh',
    'packaging/linux/gnome-disks-wrapper',
    'packaging/linux/nemo-wrapper',
):
    text = (repo / relative).read_text()
    if 'desktop-filesystems.tsv' not in text:
        raise SystemExit(f'{relative} does not consume desktop policy')
    if 'native-filesystems.tsv' in text:
        raise SystemExit(f'{relative} still reads native deployment policy')

# Synchronous process execution has one source authority. Probe and installer
# own policy only; neither may grow a private spawn implementation again.
runner = (repo / 'src/process_runner.hpp').read_text()
for required in ('struct CommandResult', 'run_command(', 'LC_ALL', 'g_spawn_sync'):
    if required not in runner:
        raise SystemExit(f'process runner lost required mechanism: {required}')
for relative in ('src/probe.cpp', 'src/installer.cpp'):
    text = (repo / relative).read_text()
    if '#include "process_runner.hpp"' not in text:
        raise SystemExit(f'{relative} does not use the shared process runner')
    if 'struct CommandResult' in text or 'CommandResult run_command' in text:
        raise SystemExit(f'{relative} reintroduced private process execution')

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
