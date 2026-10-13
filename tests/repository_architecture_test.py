#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Guard repository-wide ownership, dependency and release invariants.

This test protects architectural contracts that span subsystems. Desktop/native
presentation ownership has its own focused test; this file covers boundaries
whose failure would otherwise couple unrelated implementation or delivery paths.
"""

from pathlib import Path
import sys

repo = Path(sys.argv[1] if len(sys.argv) > 1 else ".").resolve()


def require(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit(message)


def text(path: str) -> str:
    return (repo / path).read_text(encoding="utf-8")


# Dead migration/build surfaces must stay dead. Native deployment is owned by
# data/native-filesystems.tsv plus each filesystem's Linux build unit.
for retired in (
    "kernel/Makefile",
    "kernel/README.md",
    "docs/EXT_COMMENTING_STANDARD.md",
    ".github/workflows/release.yml",
    ".github/workflows/release-contract.yml",
):
    require(not (repo / retired).exists(), f"retired repository surface returned: {retired}")

readme = text("README.md")
require("Current source release:" not in readme,
        "README duplicates VERSION as a hand-maintained release number")
require("[`VERSION`](VERSION)" in readme,
        "README does not identify VERSION as the source/release authority")

# Process execution is one mechanism. Probe and installer may apply different
# policy, but neither may grow a private spawning implementation.
runner = text("src/process_runner.hpp")
for required in ("struct CommandResult", "run_command(", "LC_ALL", "g_spawn_sync"):
    require(required in runner, f"process runner lost required mechanism: {required}")
for relative in ("src/probe.cpp", "src/installer.cpp"):
    source = text(relative)
    require('#include "process_runner.hpp"' in source,
            f"{relative} does not use the shared process runner")
    require("struct CommandResult" not in source and "CommandResult run_command" not in source,
            f"{relative} reintroduced private process execution")

# Probe cache ownership is worker-local. Inventory is cached for one refresh,
# never as hidden mutable process-global state shared between concurrent scans.
probe = text("src/probe.cpp")
require("thread_local ProbeCache cache" in probe,
        "probe cache is not worker-local")
require("static ProbeCache cache" not in probe,
        "probe cache reintroduced hidden process-global mutation")

# Platform-neutral identity cannot depend on Linux provider policy. Linux joins
# the two boundaries explicitly in catalog.cpp.
identity = text("src/platform_support.cpp")
require('#include "catalog.hpp"' not in identity,
        "cross-platform identity depends on Linux catalogue")
for linux_token in ("SupportProvider::", "project_native_linux", "linux_support_policies"):
    require(linux_token not in identity,
            f"Linux policy leaked into identity implementation: {linux_token}")

catalog = text("src/catalog.cpp")
for required in ("filesystem_identity.hpp", "linux_support_policy.hpp"):
    require(required in catalog,
            f"Linux catalogue is not joining explicit boundaries: {required}")

linux_policy = text("src/linux_support_policy.hpp")
for identity_field in ("description;", "family;", "std::string_view name;"):
    require(identity_field not in linux_policy,
            f"filesystem identity leaked into Linux policy: {identity_field}")

# Reference refresh must discover the current repository boundary. A workflow
# must never own a second historical list capable of recreating retired trees.
refresh = text(".github/workflows/import-linux-filesystems.yml")
for required in (
    "data/source-filesystems.txt",
    "data/native-filesystems.tsv",
    "data/retired-source-identities.txt",
    "*/reference/linux",
    "bash tools/validate-source-contract.sh",
):
    require(required in refresh, f"reference refresh lost boundary: {required}")
for retired in (
    line.strip()
    for line in text("data/retired-source-identities.txt").splitlines()
    if line.strip() and not line.lstrip().startswith("#")
):
    require(f"native/filesystems/{retired}" not in refresh,
            f"reference refresh hard-codes retired source identity: {retired}")

# VERSION/tag identity is the release-state authority. Commit prose is not
# executable state, and publication consumes rather than rebuilds the exact
# qualified bundle created by the producer job.
build = text(".github/workflows/build.yml")
for forbidden in (
    "startsWith(github.event.head_commit.message",
    "Release commit subject",
    "cmake -S . -B build-release",
    "gh workflow run",
    "actions/runs?",
    "wait_for_workflow",
    "git rev-parse origin/main",
):
    require(forbidden not in build,
            f"release workflow retained hidden/coupled state: {forbidden}")
for required in (
    "release-state:",
    "git ls-remote --tags origin",
    "uses: ./.github/workflows/ext-runtime-qualification.yml",
    "uses: ./.github/workflows/amiga-runtime-qualification.yml",
    "uses: ./.github/workflows/sanitizers.yml",
    "filesystem-support-linux-release",
    "actions/download-artifact@v4",
    "publish-release:",
    "github.ref }}",
):
    require(required in build, f"release DAG lost required contract: {required}")

build_run_pos = build.find("./packaging/build-run.sh")
upload_pos = build.find("uses: actions/upload-artifact@v4")
require(0 <= build_run_pos < upload_pos,
        ".run installer is not constructed and qualified before artefact upload")
publisher = build.split("  publish-release:", 1)[1]
require("build-run.sh" not in publisher,
        "publisher rebuilds the .run installer instead of promoting qualified bytes")
require("sha256sum -c RELEASE_SHA256SUMS.txt" in build,
        "qualified release bundle is not checksum-verified")
require("filesystem-support-build-v6-${{ github.event_name == 'pull_request' && github.event.pull_request.number || github.ref }}" in build,
        "main release candidates are not serialized by branch")

for relative in (
    ".github/workflows/ext-runtime-qualification.yml",
    ".github/workflows/amiga-runtime-qualification.yml",
    ".github/workflows/sanitizers.yml",
):
    workflow = text(relative)
    for required in ("workflow_call:", "Exact commit SHA to qualify", "inputs.sha"):
        require(required in workflow,
                f"{relative} lost reusable exact-SHA boundary: {required}")

sanitizers = text(".github/workflows/sanitizers.yml")
for required in (
    "-fsanitize=address,undefined",
    "tests/parser_adversarial_test.py",
    "build-sanitized/fsinspect",
):
    require(required in sanitizers,
            f"sanitizer qualification lost required coverage: {required}")
require("\n  push:" not in sanitizers,
        "sanitizers duplicate release work as an independent main-push workflow")
require((repo / "tests/parser_adversarial_test.py").is_file(),
        "malformed-media parser corpus is missing")

apt = text(".github/workflows/apt-publication.yml")
for forbidden in (
    "head_commit.message",
    "startsWith(",
    "Waiting for GitHub release",
    'workflows: ["Publish release"]',
):
    require(forbidden not in apt,
            f"APT publication retained hidden release state: {forbidden}")
for required in (
    'workflows: ["build"]',
    "head_sha",
    "target_commitish",
    "steps.release.outputs.publish == 'true'",
):
    require(required in apt, f"APT publication lost exact-release gate: {required}")

# Current-state documentation must not claim implementation paths that no
# longer exist or deny adapters that are actually shipped by the native manifest.
for design in (
    "native/filesystems/sfs2/DESIGN.md",
    "native/filesystems/pfs3/DESIGN.md",
):
    require("Linux adapter not yet implemented" not in text(design),
            f"stale Linux implementation status in {design}")

migration = text("docs/EXTFS_FOR_WINDOWS_MIGRATION.md")
require("archive/extfs-for-windows-v0.9.9/" not in migration,
        "migration ledger claims a live archive path that is not present")
windows_arch = text("docs/WINDOWS_FILESYSTEM_ARCHITECTURE.md")
require("104-filesystem" not in windows_arch,
        "Windows architecture retains obsolete catalogue size")
require("temporary-mounter" not in windows_arch and "temporary file/volume-mounter" not in windows_arch,
        "Windows architecture retains an undocumented product mounter fallback")
windows_lifecycle = text("docs/windows/WINDOWS_DRIVER_LIFECYCLE.md")
require("sources remain in the migration archive" not in windows_lifecycle,
        "Windows lifecycle claims a non-existent live migration archive")

print("repository architecture contract: PASS")
