#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Protect desktop/native policy ownership and the GTK action boundary."""

from pathlib import Path
import sys

repo = Path(sys.argv[1] if len(sys.argv) > 1 else ".").resolve()
native = repo / "data/native-filesystems.tsv"
desktop = repo / "data/desktop-filesystems.tsv"


native_rows: dict[str, str] = {}
for line in native.read_text(encoding="utf-8").splitlines():
    if not line or line.startswith("#"):
        continue
    fields = line.split("\t")
    if len(fields) != 2:
        raise SystemExit(f"invalid native row; deployment manifest owns only id/module: {line}")
    fs, module = fields
    if fs in native_rows:
        raise SystemExit(f"duplicate native filesystem: {fs}")
    if module in native_rows.values():
        raise SystemExit(f"duplicate native module: {module}")
    native_rows[fs] = module

native_manifest_text = native.read_text(encoding="utf-8")
for retired in ("reserved_kind", "reserved_variant", "desktop_kind", "desktop_variant"):
    if retired in native_manifest_text:
        raise SystemExit(f"native deployment manifest retains desktop-era field: {retired}")


desktop_rows: dict[str, int] = {}
variants: set[int] = set()
for line in desktop.read_text(encoding="utf-8").splitlines():
    if not line or line.startswith("#"):
        continue
    fields = line.split("\t")
    if len(fields) != 2:
        raise SystemExit(f"invalid desktop row: {line}")
    fs, variant_text = fields
    if fs in desktop_rows:
        raise SystemExit(f"duplicate desktop filesystem: {fs}")
    if fs not in native_rows:
        raise SystemExit(f"desktop policy references unmanaged native filesystem: {fs}")
    variant = int(variant_text)
    if variant <= 0 or variant in variants:
        raise SystemExit(f"invalid/duplicate desktop variant: {variant}")
    desktop_rows[fs] = variant
    variants.add(variant)

if not desktop_rows:
    raise SystemExit("desktop manifest is empty")


def shell_code(source: str) -> str:
    return "\n".join(
        line for line in source.splitlines()
        if not line.lstrip().startswith("#")
    )


# Native orchestration can ask desktop integration whether a filesystem is
# managed, but desktop policy cannot own kernel build/replacement mechanics.
coordinator = (repo / "packaging/linux/native-module-helper.sh").read_text(encoding="utf-8")
kernel_helper = (repo / "packaging/linux/kernel-module-helper.sh").read_text(encoding="utf-8")
coordinator_code = shell_code(coordinator)
kernel_code = shell_code(kernel_helper)

if "desktop_kind=" in coordinator_code or '$3 == "amiga"' in coordinator_code:
    raise SystemExit("native coordinator owns desktop membership")
if 'is-managed "$filesystem"' not in coordinator_code:
    raise SystemExit("native coordinator does not delegate desktop membership")
if "kernel-module-helper" not in coordinator_code:
    raise SystemExit("native coordinator does not delegate kernel transaction ownership")

for token in ("desktop-integration", "GNOME", "Nemo", "udev", "is-managed"):
    if token in kernel_code:
        raise SystemExit(f"kernel helper depends on desktop concern: {token}")
for token in ("modprobe", "depmod", "/lib/modules/", "sign_for_secure_boot"):
    if token in coordinator_code:
        raise SystemExit(f"kernel transaction logic leaked into coordinator: {token}")

cmake = (repo / "CMakeLists.txt").read_text(encoding="utf-8")
for helper in (
    "packaging/linux/native-module-helper.sh",
    "packaging/linux/kernel-module-helper.sh",
):
    if helper not in cmake:
        raise SystemExit(f"packaging does not install required helper: {helper}")
for token in (
    "desktop_kind",
    "desktop_variant",
    "FILESYSTEM_SUPPORT_AMIGA_DESKTOP_FILESYSTEMS",
):
    if token in cmake:
        raise SystemExit(f"top-level native build graph still owns desktop policy: {token}")

for relative in (
    "packaging/linux/desktop-integration.sh",
    "packaging/linux/gnome-disks-wrapper",
    "packaging/linux/nemo-wrapper",
):
    source = (repo / relative).read_text(encoding="utf-8")
    if "desktop-filesystems.tsv" not in source:
        raise SystemExit(f"{relative} does not consume desktop policy")
    if "native-filesystems.tsv" in source:
        raise SystemExit(f"{relative} still reads native deployment policy")

# Launchers and the activated UDisks/GIO plugin are presentation consumers.
# They may consume the committed desktop active set, but must never grow a
# second native-module state machine or reconstruct module paths/names.
for relative in (
    "packaging/linux/gnome-disks-wrapper",
    "packaging/linux/nemo-wrapper",
    "packaging/linux/udisks-amiga-names.c",
):
    source = (repo / relative).read_text(encoding="utf-8")
    for token in (
        "/lib/modules",
        "native-filesystems.tsv",
        "ifs_native_amiga_installed",
        "IFS_NATIVE_MODULES_ROOT",
    ):
        if token in source:
            raise SystemExit(f"presentation code owns native deployment state: {relative}: {token}")

amiga_desktop_cmake = (repo / "cmake/amiga-desktop-detection.cmake").read_text(encoding="utf-8")
desktop_test_definition = "IFS_NATIVE_MODULES_ROOT=\"${CMAKE_CURRENT_BINARY_DIR}/desktop-test-root/lib/modules\""
if desktop_test_definition in amiga_desktop_cmake:
    raise SystemExit("desktop plugin test target still injects native module state")

desktop_helper = (repo / "packaging/linux/desktop-integration.sh").read_text(encoding="utf-8")
for ignored in (
    "udevadm control --reload-rules || true",
    "udevadm trigger --subsystem-match=block --action=change || true",
    "udevadm settle --timeout=30 || true",
):
    if ignored in desktop_helper:
        raise SystemExit(f"desktop integration silently ignores refresh failure: {ignored}")
if "refresh_best_effort" not in desktop_helper:
    raise SystemExit("desktop rollback has no explicit best-effort refresh path")

# Runtime qualification consumes the desktop manifest instead of duplicating an
# Amiga filesystem list, and the crash/recovery phase remains explicit.
amiga_workflow = (repo / ".github/workflows/amiga-runtime-qualification.yml").read_text(encoding="utf-8")
if "data/desktop-filesystems.tsv" not in amiga_workflow:
    raise SystemExit("Amiga runtime qualification does not consume desktop policy")
if "desktop_kind" in amiga_workflow:
    raise SystemExit("Amiga runtime qualification still depends on retired native desktop columns")
if "done < data/desktop-filesystems.tsv" not in amiga_workflow:
    raise SystemExit("Amiga runtime qualification is not driven by the desktop manifest")
for required in (
    "pull_request:",
    "tests/amiga_runtime_init.sh",
    "AMIGA-CRASH-ARMED",
    "AMIGA-CRASH-RECOVERY-PASS",
):
    if required not in amiga_workflow:
        raise SystemExit(f"Amiga recovery qualification lost required contract: {required}")

amiga_init = (repo / "tests/amiga_runtime_init.sh").read_text(encoding="utf-8")
for required in (
    'sync || fail "crash arm sync"',
    "AMIGA-CRASH-ARMED",
    "AMIGA-CRASH-RECOVERY-PASS",
    "reboot -f",
):
    if required not in amiga_init:
        raise SystemExit(f"Amiga crash-recovery init lost required phase: {required}")

# GTK owns presentation. The desktop action layer owns command preparation and
# calls the installer boundary; it returns explicit data rather than closures.
main = (repo / "src/main.cpp").read_text(encoding="utf-8")
if '#include "desktop_actions.hpp"' not in main:
    raise SystemExit("GTK shell does not consume the desktop action boundary")
if '#include "installer.hpp"' in main:
    raise SystemExit("GTK shell imports installer primitives directly")
for token in (
    "install_packages_async(",
    "remove_packages_async(",
    "plan_package_removal(",
    "install_native_module_async(",
    "remove_native_module_async(",
    "load_module_async(",
    "unload_module_async(",
):
    if token in main:
        raise SystemExit(f"installer/module primitive leaked into GTK shell: {token}")
if "prepare_package_desktop_action_async(" not in main:
    raise SystemExit("GTK package removal planning is not asynchronous")
for stale in ("ImagesFuse", "NetworkVirtual", "Images & FUSE", "Network & Virtual"):
    if stale in main:
        raise SystemExit(f"GTK shell retains out-of-scope catalogue concept: {stale}")

orchestration = (repo / "src/desktop_actions.cpp").read_text(encoding="utf-8")
action_header = (repo / "src/desktop_actions.hpp").read_text(encoding="utf-8")
for required in (
    '#include "action_policy.hpp"',
    '#include "installer.hpp"',
    "prepare_package_desktop_action(",
    "prepare_package_desktop_action_async(",
    "prepare_module_desktop_action(",
    "execute_desktop_action_async(",
):
    if required not in orchestration:
        raise SystemExit(f"desktop action boundary lost required ownership: {required}")
for required in (
    "enum class DesktopActionKind",
    "DesktopActionKind kind",
    "std::vector<std::string> packages",
    "std::string filesystem_id",
    "std::string module",
    "PlanningRemoval",
):
    if required not in action_header:
        raise SystemExit(f"desktop action command is not explicit data: {required}")
for opaque in (
    "std::function<void(DesktopActionCompletion)> executor",
    "action.executor =",
):
    if opaque in action_header or opaque in orchestration:
        raise SystemExit(f"opaque executable closure returned to desktop action model: {opaque}")
if "plan_package_removal(action.packages)" in orchestration:
    raise SystemExit("desktop action preparation synchronously invokes Debian planning")
if "plan_package_removal_async(" not in orchestration:
    raise SystemExit("desktop removal planning does not cross the async installer boundary")

for required in ("src/desktop_actions.cpp", "tests/desktop_actions_test.cpp"):
    if required not in cmake:
        raise SystemExit(f"build graph omits desktop action boundary asset: {required}")

catalog_test = (repo / "tests/catalog_test.cpp").read_text(encoding="utf-8")
for token in ("desktop_kind", "desktop_variant"):
    if token in catalog_test:
        raise SystemExit(f"native catalogue validation still owns desktop policy: {token}")

print("desktop/native boundary contract: PASS")
