#!/usr/bin/env python3
"""Guard the release pipeline against accidental orchestration coupling."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
WORKFLOWS = ROOT / ".github" / "workflows"

release = (WORKFLOWS / "release.yml").read_text()
apt = (WORKFLOWS / "apt-publication.yml").read_text()
build = (WORKFLOWS / "build.yml").read_text()
amiga = (WORKFLOWS / "amiga-runtime-qualification.yml").read_text()

checks = [
    ('release is triggered by build', 'workflows: ["build"]' in release),
    ('release consumes the triggering build run', 'BUILD_RUN_ID: ${{ github.event.workflow_run.id }}' in release),
    ('release waits for EXT qualification', 'wait_for_workflow "EXT runtime qualification"' in release),
    ('release waits for Amiga qualification', 'wait_for_workflow "Amiga runtime qualification"' in release),
    ('release does not dispatch Amiga qualification', 'gh workflow run amiga-runtime-qualification.yml' not in release),
    ('release does not search for a build run', 'map(select(.name == "build"' not in release),
    ('release does not require main to remain at the release SHA', 'git rev-parse origin/main' not in release),
    ('release does not rebuild qualified source', 'cmake -S . -B build-release' not in release),
    ('APT follows release publication', 'workflows: ["Publish release"]' in apt),
    ('APT does not wait for its sibling release workflow', 'Waiting for GitHub release' not in apt),
    ('pushed builds are keyed by immutable SHA', "github.event.pull_request.number || github.sha" in build),
    ('Amiga qualification is keyed by immutable SHA', 'filesystem-support-amiga-runtime-${{ github.sha }}' in amiga),
    ('release commits always require Amiga qualification', '[[ "$subject" == Release\\ * ]]' in amiga),
]

failed = [name for name, ok in checks if not ok]
if failed:
    for name in failed:
        print(f"FAIL: {name}")
    raise SystemExit(1)

for name, _ in checks:
    print(f"PASS: {name}")
