#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Protect ownership and lifecycle boundaries found by the forensic review.

These checks intentionally target authority relationships rather than private
implementation shape: trusted destructive command identity, platform-neutral
catalogue ownership, native-module lifetime/rollback ownership, asynchronous
publication, and format-centric provider documentation.
"""

from pathlib import Path
import sys

repo = Path(sys.argv[1] if len(sys.argv) > 1 else ".").resolve()


def text(path: str) -> str:
    return (repo / path).read_text(encoding="utf-8")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit(message)


installer = text("src/installer.cpp")
for required in (
    'constexpr const char* kAptGet = "/usr/bin/apt-get";',
    'constexpr const char* kDpkgQuery = "/usr/bin/dpkg-query";',
    'run_command({\n        kDpkgQuery,',
    'std::vector<std::string> arguments = {\n        kAptGet,\n        "-s",',
):
    require(required in installer,
            f"Debian destructive planning lost trusted command identity: {required}")
for forbidden in (
    'run_command({\n        "dpkg-query",',
    'std::vector<std::string> arguments = {\n        "apt-get",',
):
    require(forbidden not in installer,
            f"caller PATH can again select Debian planning command: {forbidden}")

cmake = text("CMakeLists.txt")
for required in (
    "add_library(filesystem-support-identity STATIC\n    src/platform_support.cpp)",
    "add_library(filesystem-support-catalog STATIC\n    src/catalog.cpp)",
    "PRIVATE filesystem-support-identity Comctl32",
    "PRIVATE filesystem-support-identity)\n    filesystem_support_configure(filesystem-support-windows-catalog-test)",
):
    require(required in cmake,
            f"platform-neutral identity boundary lost in CMake: {required}")
require("src/platform_support.cpp\n    src/catalog.cpp" not in cmake and
        "src/catalog.cpp\n    src/platform_support.cpp" not in cmake,
        "platform-neutral identity and Linux catalogue were recombined")

kernel = text("packaging/linux/kernel-module-helper.sh")
for required in (
    "remove_stale_kernel_directories()",
    "if [[ $# -eq 1 && $1 == remove-all ]]; then",
    'local restore_runtime=$4',
    'local runtime_changed=false',
    'runtime_changed=true',
    '"$previous_loaded" "$runtime_changed"',
):
    require(required in kernel,
            f"kernel lifetime/rollback ownership lost boundary: {required}")

prerm = text("packaging/linux/prerm")
require('"$helper" remove-all' in prerm,
        "package removal no longer delegates native state cleanup to kernel authority")
package_cmake = text("cmake/amiga-desktop-detection.cmake")
require('"${CMAKE_CURRENT_SOURCE_DIR}/packaging/linux/prerm"' in package_cmake,
        "Debian package does not install the native-state prerm owner")

apt = text(".github/workflows/apt-publication.yml")
for forbidden in ("seq 1 120", "sleep 10", "within 20 minutes"):
    require(forbidden not in apt,
            f"source APT workflow again depends on scheduler timing: {forbidden}")
require("central scheduled publisher remains authoritative" in apt,
        "APT workflow does not state the asynchronous publication authority")

for path, stale in (
    ("native/filesystems/ntfs/DESIGN.md", "catalogue currently exposes two external provider paths"),
    ("native/filesystems/zfs/DESIGN.md", "separate `zfs-fuse` catalogue identity"),
    ("native/filesystems/exfat/DESIGN.md", "separate `exfat-fuse` catalogue identity"),
):
    require(stale not in text(path), f"stale provider-as-identity wording remains in {path}")

print("forensic ownership boundary contract: PASS")
