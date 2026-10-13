#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Guard repository-level ownership and release invariants.

These checks intentionally target architectural contracts rather than incidental
implementation syntax. They keep retired control planes from returning and make
release qualification/promotion rules executable alongside the source.
"""

from pathlib import Path
import sys

repo = Path(sys.argv[1] if len(sys.argv) > 1 else ".").resolve()


def require(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit(message)


def text(path: str) -> str:
    return (repo / path).read_text(encoding="utf-8")


# Dead migration/build surfaces must stay dead. The native deployment manifest
# and per-filesystem linux/Makefiles are the only native Linux build authority.
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

# VERSION/tag identity is the one release-state authority. Commit prose is not
# executable release state, and publication consumes rather than rebuilds the
# artefacts qualified by the producer job.
build = text(".github/workflows/build.yml")
for forbidden in (
    "startsWith(github.event.head_commit.message",
    "Release commit subject",
    "cmake -S . -B build-release",
    "gh workflow run",
    "actions/runs?",
):
    require(forbidden not in build, f"release workflow retained hidden/coupled state: {forbidden}")
for required in (
    "release-state:",
    "git ls-remote --tags origin",
    "uses: ./.github/workflows/ext-runtime-qualification.yml",
    "uses: ./.github/workflows/amiga-runtime-qualification.yml",
    "uses: ./.github/workflows/sanitizers.yml",
    "filesystem-support-linux-release",
    "actions/download-artifact@v4",
    "publish-release:",
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

sanitizers = text(".github/workflows/sanitizers.yml")
require("workflow_call:" in sanitizers,
        "sanitizer qualification is not callable from the exact release DAG")
require("Exact commit SHA to qualify" in sanitizers,
        "sanitizer reusable boundary does not document exact-SHA ownership")
require("\n  push:" not in sanitizers,
        "sanitizers duplicate release work as an independent main-push workflow")

apt = text(".github/workflows/apt-publication.yml")
for forbidden in (
    "head_commit.message",
    "startsWith(",
    "Waiting for GitHub release",
):
    require(forbidden not in apt, f"APT publication retained hidden release state: {forbidden}")
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

print("repository architecture contract: PASS")
