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
    if len(fields) != 2:
        raise SystemExit(f'invalid native row; deployment manifest owns only id/module: {line}')
    fs, module = fields
    if fs in native_rows:
        raise SystemExit(f'duplicate native filesystem: {fs}')
    if module in native_rows.values():
        raise SystemExit(f'duplicate native module: {module}')
    native_rows[fs] = module

native_manifest_text = native.read_text()
for retired in ('reserved_kind', 'reserved_variant', 'desktop_kind', 'desktop_variant'):
    if retired in native_manifest_text:
        raise SystemExit(f'native deployment manifest retains desktop-era field: {retired}')

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


def shell_code(text: str) -> str:
    return '\n'.join(
        line for line in text.splitlines()
        if not line.lstrip().startswith('#')
    )

coordinator = (repo / 'packaging/linux/native-module-helper.sh').read_text()
kernel_helper = (repo / 'packaging/linux/kernel-module-helper.sh').read_text()
coordinator_code = shell_code(coordinator)
kernel_code = shell_code(kernel_helper)

if 'desktop_kind=' in coordinator_code or '$3 == "amiga"' in coordinator_code:
    raise SystemExit('native coordinator owns desktop membership')
if 'is-managed "$filesystem"' not in coordinator_code:
    raise SystemExit('native coordinator does not delegate desktop membership')
if 'kernel-module-helper' not in coordinator_code:
    raise SystemExit('native coordinator does not delegate kernel transaction ownership')

for token in ('desktop-integration', 'GNOME', 'Nemo', 'udev', 'is-managed'):
    if token in kernel_code:
        raise SystemExit(f'kernel helper depends on desktop concern: {token}')
for token in ('modprobe', 'depmod', '/lib/modules/', 'sign_for_secure_boot'):
    if token in coordinator_code:
        raise SystemExit(f'kernel transaction logic leaked into coordinator: {token}')

cmake = (repo / 'CMakeLists.txt').read_text()
for helper in (
    'packaging/linux/native-module-helper.sh',
    'packaging/linux/kernel-module-helper.sh',
):
    if helper not in cmake:
        raise SystemExit(f'packaging does not install required helper: {helper}')

for token in (
    'desktop_kind',
    'desktop_variant',
    'FILESYSTEM_SUPPORT_AMIGA_DESKTOP_FILESYSTEMS',
):
    if token in cmake:
        raise SystemExit(f'top-level native build graph still owns desktop policy: {token}')

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

desktop_helper = (repo / 'packaging/linux/desktop-integration.sh').read_text()
for ignored in (
    'udevadm control --reload-rules || true',
    'udevadm trigger --subsystem-match=block --action=change || true',
    'udevadm settle --timeout=30 || true',
):
    if ignored in desktop_helper:
        raise SystemExit(f'desktop integration silently ignores refresh failure: {ignored}')
if 'refresh_best_effort' not in desktop_helper:
    raise SystemExit('desktop rollback has no explicit best-effort refresh path')

amiga_workflow = (repo / '.github/workflows/amiga-runtime-qualification.yml').read_text()
if 'data/desktop-filesystems.tsv' not in amiga_workflow:
    raise SystemExit('Amiga runtime qualification does not consume desktop policy')
if 'desktop_kind' in amiga_workflow:
    raise SystemExit('Amiga runtime qualification still depends on retired native desktop columns')
if "done < data/desktop-filesystems.tsv" not in amiga_workflow:
    raise SystemExit('Amiga runtime qualification is not driven by the desktop manifest')

# Ordinary CI must consume the two-field native deployment contract as two
# fields; accepting obsolete trailing variable names silently preserves the old
# ownership model and makes future drift harder to detect.
build_workflow = (repo / '.github/workflows/build.yml').read_text()
if 'read -r fs module desktop_kind desktop_variant' in build_workflow:
    raise SystemExit('build workflow still models retired native desktop columns')

# Parser hardening is a permanent qualification layer, not an occasional local
# diagnostic. The workflow must use both memory/UB sanitizers and malformed
# media that drives the real fsinspect entry point.
sanitizer_workflow = (repo / '.github/workflows/sanitizers.yml').read_text()
for required in (
    '-fsanitize=address,undefined',
    'tests/parser_adversarial_test.py',
    'build-sanitized/fsinspect',
):
    if required not in sanitizer_workflow:
        raise SystemExit(f'sanitizer qualification lost required coverage: {required}')
if not (repo / 'tests/parser_adversarial_test.py').is_file():
    raise SystemExit('malformed-media parser corpus is missing')

main = (repo / 'src/main.cpp').read_text()
if '#include "desktop_actions.hpp"' not in main:
    raise SystemExit('GTK shell does not consume the desktop action boundary')
if '#include "installer.hpp"' in main:
    raise SystemExit('GTK shell still imports installer primitives directly')
for token in (
    'install_packages_async(',
    'remove_packages_async(',
    'plan_package_removal(',
    'install_native_module_async(',
    'remove_native_module_async(',
    'load_module_async(',
    'unload_module_async(',
):
    if token in main:
        raise SystemExit(f'installer/module primitive leaked into GTK shell: {token}')
if 'prepare_package_desktop_action_async(' not in main:
    raise SystemExit('GTK package removal planning is not asynchronous')
for stale in ('ImagesFuse', 'NetworkVirtual', 'Images & FUSE', 'Network & Virtual'):
    if stale in main:
        raise SystemExit(f'GTK shell retains out-of-scope catalogue concept: {stale}')

orchestration = (repo / 'src/desktop_actions.cpp').read_text()
action_header = (repo / 'src/desktop_actions.hpp').read_text()
for required in (
    '#include "action_policy.hpp"',
    '#include "installer.hpp"',
    'prepare_package_desktop_action(',
    'prepare_package_desktop_action_async(',
    'prepare_module_desktop_action(',
    'execute_desktop_action_async(',
):
    if required not in orchestration:
        raise SystemExit(f'desktop action boundary lost required ownership: {required}')
for required in (
    'enum class DesktopActionKind',
    'DesktopActionKind kind',
    'std::vector<std::string> packages',
    'std::string filesystem_id',
    'std::string module',
    'PlanningRemoval',
):
    if required not in action_header:
        raise SystemExit(f'desktop action command is not explicit data: {required}')
for opaque in (
    'std::function<void(DesktopActionCompletion)> executor',
    'action.executor =',
):
    if opaque in action_header or opaque in orchestration:
        raise SystemExit(f'opaque executable closure returned to desktop action model: {opaque}')
if 'plan_package_removal(action.packages)' in orchestration:
    raise SystemExit('desktop action preparation synchronously invokes Debian planning')
if 'plan_package_removal_async(' not in orchestration:
    raise SystemExit('desktop removal planning does not cross the async installer boundary')
for required in ('src/desktop_actions.cpp', 'tests/desktop_actions_test.cpp'):
    if required not in cmake:
        raise SystemExit(f'build graph omits desktop action boundary asset: {required}')

catalog_test = (repo / 'tests/catalog_test.cpp').read_text()
for token in ('desktop_kind', 'desktop_variant'):
    if token in catalog_test:
        raise SystemExit(f'native catalogue validation still owns desktop policy: {token}')

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

probe_impl = (repo / 'src/probe.cpp').read_text()
if 'thread_local ProbeCache cache' not in probe_impl:
    raise SystemExit('probe cache is process-global rather than worker-local')
if 'static ProbeCache cache' in probe_impl:
    raise SystemExit('probe cache reintroduced hidden process-global mutation')

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
