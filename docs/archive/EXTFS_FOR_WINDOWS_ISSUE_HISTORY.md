# ExtFS-for-Windows issue and pull-request history

Preserved before repository retirement. Engineering provenance only; these are not active Filesystem Support issues.

Baseline main: `dda219be47033372217149bce8c8ee20f19ded0a`.

## #1 — Harden and qualify ExtFS 0.9.2

- Type: pull request
- State: closed
- Created: 2026-08-18T10:06:58Z
- Closed: 2026-08-18T10:38:14Z
- Original: https://github.com/Infiltrator-Projects/ExtFS-for-Windows/pull/1

Forensic-audit hardening checkpoint before further filesystem feature work.

Scope completed:
- preserved the bounded 0.9 filesystem feature boundary while versioning the post-0.9.1 durability work as 0.9.2;
- added ext2 durability capability preflight and explicit crash-consistency barriers;
- added destructive real mke2fs ext2/ext3/ext4 grow/shrink/reopen/e2fsck qualification;
- expanded sanitizer/static-analysis CI;
- added Windows WDK Release x64 / Driver Code Analysis / InfVerif / Inf2Cat CI;
- added reversible Windows append/EOF-resize qualification probes;
- refreshed stale version and feature descriptions.

Final branch head passed both permanent CI gates. The 47-step audit-development history was intentionally not merged. Its tested tree was squashed directly onto main as clean commit `40eaa6db663f91a8ad343e07dab06362d71e32d5`. This PR is therefore closed as integrated-by-squash.

## #2 — Publish verified ExtFS 0.9.2

- Type: pull request
- State: closed
- Created: 2026-08-18T10:48:13Z
- Closed: 2026-08-19T11:39:25Z
- Original: https://github.com/Infiltrator-Projects/ExtFS-for-Windows/pull/2

Release-only lane completed successfully.

Run #7 (`32248121863`) revalidated the exact `main` release source, passed the portable and real-image qualification, rebuilt/analyzed/validated the Windows driver package, test-signed the SYS/CAT deterministically without the headless trust-chain hang, built and hashed the NSIS installer, published `v0.9.2`, verified the exact two explicit release assets, and removed all release/audit working branches.

This PR was never intended to be merged; it is closed after successful publication.

## #3 — Apply ExtFS forensic fixes and Common 1.9 pin

- Type: pull request
- State: closed
- Created: 2026-08-20T01:10:25Z
- Closed: 2026-08-20T01:33:45Z
- Original: https://github.com/Infiltrator-Projects/ExtFS-for-Windows/pull/3
- Merged: 2026-08-20T01:33:45Z

Temporary validation PR for the forensic maintenance pass. The repair runner applies three concrete Windows-driver fixes, pins Infiltratr Common 1.9.0 exactly, runs portable/unit/real-image qualification and a freestanding Windows cross-build, then publishes one clean commit to main only if all checks succeed. The temporary workflow/runner are removed from the resulting main commit.

## #4 — Release ExtFS 0.9.3 maintenance package

- Type: pull request
- State: closed
- Created: 2026-08-20T01:41:39Z
- Closed: 2026-08-20T01:48:17Z
- Original: https://github.com/Infiltrator-Projects/ExtFS-for-Windows/pull/4
- Merged: 2026-08-20T01:48:17Z

Publishes the already-audited Windows lifecycle/Common 1.9 maintenance changes as ExtFS 0.9.3 instead of mutating the historical 0.9.2 release. This PR updates authoritative binary/INF/installer/docs metadata to 0.9.3, keeps the filesystem mutation boundary unchanged, removes the stale hardening branch trigger from WDK CI, and adds an automated release publisher. On the final main release commit it builds and test-signs the x64 installer, verifies version/SHA/release asset metadata, publishes GitHub Release v0.9.3, and deletes the merged forensic-fix-common19 branch so main is the only branch. No duplicate custom source ZIP or patch asset is created.

## #5 — Trigger verified ExtFS 0.9.3 publication

- Type: pull request
- State: closed
- Created: 2026-08-20T02:09:21Z
- Closed: 2026-08-20T22:48:00Z
- Original: https://github.com/Infiltrator-Projects/ExtFS-for-Windows/pull/5

Superseded by the controlled ExtFS 0.9.3 recovery. This PR's force-publish workflow is obsolete and must not be merged.

## #6 — Fix hung ExtFS 0.9.3 publisher

- Type: pull request
- State: closed
- Created: 2026-08-20T03:52:37Z
- Closed: 2026-08-20T03:52:53Z
- Original: https://github.com/Infiltrator-Projects/ExtFS-for-Windows/pull/6
- Merged: 2026-08-20T03:52:53Z

Replaces the stalled 0.9.3 rescue packaging path with a bounded fast publisher. It builds the exact already-qualified release commit d7b43df5c672927b767e2ef63a58b724f23c61ad, verifies Common 1.9.0, skips only the redundant second Code Analysis pass during packaging, publishes/repairs v0.9.3 with exactly one installer asset, and removes the temporary release branches after successful verification.

## #7 — Make ExtFS 0.9.3 publisher observable

- Type: pull request
- State: closed
- Created: 2026-08-20T04:00:02Z
- Closed: 2026-08-20T04:00:16Z
- Original: https://github.com/Infiltrator-Projects/ExtFS-for-Windows/pull/7
- Merged: 2026-08-20T04:00:16Z

Adds a PR trigger to the fast 0.9.3 publisher so the packaging job can be inspected directly through GitHub Actions while retaining the main-push trigger. The release source remains pinned to the already-qualified 0.9.3 commit and temporary release branches are removed after successful publication.

## #8 — Recover ExtFS 0.9.3 Windows lifecycle and publication

- Type: pull request
- State: closed
- Created: 2026-08-20T23:03:23Z
- Closed: 2026-08-20T23:44:48Z
- Original: https://github.com/Infiltrator-Projects/ExtFS-for-Windows/pull/8
- Merged: 2026-08-20T23:44:48Z

## Recovery scope

- replace the invalid FILE_OBJECT FsContext layout with an FSRTL advanced FCB header;
- make Memory Manager authoritative for mapped-section closure before FCB reclamation and removable-media refresh;
- keep FCB header sizes synchronized after supported resize operations;
- pin and assert the staged INF at DriverVer 0.9.3.0;
- sign the final SYS before catalog generation, then sign the catalog and inspect both signatures/catalog membership in bounded workers without mutating Windows trust stores;
- make Windows CI build the complete signed installer under explicit time limits;
- remove the broad rescue publisher and retain one exact-message, post-merge release trigger;
- update the Windows qualification probe and documentation.

This intentionally does not expand the filesystem mutation boundary. Hosted CI validates build, analysis, package metadata, signing and installer assembly; installed-driver/Driver Verifier testing remains a manual disposable-VM qualification step.

## #9 — Fix v0.9.3 publisher absence preflight

- Type: pull request
- State: closed
- Created: 2026-08-21T00:51:02Z
- Closed: 2026-08-21T00:59:21Z
- Original: https://github.com/Infiltrator-Projects/ExtFS-for-Windows/pull/9
- Merged: 2026-08-21T00:59:21Z

The guarded publisher correctly refused to continue after PowerShell promoted GitHub CLI's expected `release not found` stderr to a terminating error. Replace that native-process probe with an authenticated GitHub REST lookup that explicitly accepts only HTTP 404 as the absent-release state. All other HTTP failures remain fatal, and an existing v0.9.3 still blocks publication.

## #10 — Make v0.9.3 recovery cleanup idempotent

- Type: pull request
- State: closed
- Created: 2026-08-21T01:07:32Z
- Closed: 2026-08-21T01:12:37Z
- Original: https://github.com/Infiltrator-Projects/ExtFS-for-Windows/pull/10
- Merged: 2026-08-21T01:12:37Z

The v0.9.3 release and installer were published and verified, but the publisher's final housekeeping step stopped because obsolete PR #5 was already closed. Separate branch cleanup into a guarded, idempotent job that first verifies the final release asset, accepts only missing branches as harmless, and deletes the stale recovery branches after a one-time finalize merge.

## #11 — Add ARM64 test package and guarded transition workflow

- Type: pull request
- State: closed
- Created: 2026-08-21T03:52:21Z
- Closed: 2026-08-21T04:16:36Z
- Original: https://github.com/Infiltrator-Projects/ExtFS-for-Windows/pull/11
- Merged: 2026-08-21T04:16:36Z

## Summary

- add x64 and ARM64 WDK project/package configurations
- validate the driver PE machine type and architecture-specific INF catalog targets
- build separately named x64 and ARM64 experimental installers
- refuse host/driver architecture mismatches, pending restarts and unsafe Secure Boot state
- add read-only host readiness inventory and a reversible ARM64/Paragon transition guide
- add a separately labelled ARM64 test prerelease workflow; the existing v0.9.3 x64 release is untouched

## Safety boundary

This is test readiness, not production readiness. The ARM64 artifact remains development/test signed and is intended only for a disposable Windows test system and a disposable, backed-up ext volume.

## Validation

- [x] x64 WDK build, code analysis, INF/catalog/signature verification and installer assembly
- [x] ARM64 WDK build, code analysis, PE `0xAA64`, INF/catalog/signature verification and installer assembly
- [x] portable core CI remains green

Validated on commit `de6ad8a80c69e2f1fa355795b234e50ca6c8b4a6`:

- Windows WDK package CI run 100: success (x64 and ARM64)
- Portable ExtFS CI run 102: success

Related host audit: the target Windows PC is native ARM64, has a pending restart, and currently exposes no unambiguous Paragon/UFSD uninstall entry or filesystem service. Existing software must remain installed until the replacement artifact is qualified and the exact product is identified.

## #12 — Fix ARM64 prerelease absence check

- Type: pull request
- State: closed
- Created: 2026-08-21T04:19:03Z
- Closed: 2026-08-21T04:25:19Z
- Original: https://github.com/Infiltrator-Projects/ExtFS-for-Windows/pull/12
- Merged: 2026-08-21T04:25:19Z

## Summary

Replace the GitHub CLI absence probe with an explicit authenticated REST check that accepts only HTTP 404 as the expected not-yet-published state.

The failed publication run stopped at this preflight before building or creating a release. The validated ARM64 and x64 package code is unchanged.

## Validation

- [x] Portable ExtFS CI run 104
- [x] x64 WDK package CI run 102
- [x] ARM64 WDK package CI run 102

## #13 — Fix ExtFS 0.9.3 installer StartService failure

- Type: pull request
- State: closed
- Created: 2026-08-22T11:03:02Z
- Closed: 2026-08-22T11:08:59Z
- Original: https://github.com/Infiltrator-Projects/ExtFS-for-Windows/pull/13
- Merged: 2026-08-22T11:08:59Z

Fixes the reproducible installer failure where CreateService succeeds but StartService returns exit 6 because Install-ExtFS.ps1 wrote a doubled-backslash NT service ImagePath. Adds registry-level service contract validation and diagnostics, packages the installer-only hotfix as 0.9.4 while retaining the already-qualified 0.9.3.0 driver payload, adds a regression test, updates WDK package CI, and prepares verified v0.9.4 publication plus cleanup of temporary branches.

### Preserved discussion

**chatgpt-codex-connector[bot] — 2026-08-22T11:08:54Z**

You have reached your Codex usage limits for code reviews. You can see your limits in the [Codex usage dashboard](https://chatgpt.com/codex/cloud/settings/usage).

## #14 — Trigger verified ExtFS 0.9.4 artifact publication

- Type: pull request
- State: closed
- Created: 2026-08-22T11:13:15Z
- Closed: 2026-08-22T11:15:17Z
- Original: https://github.com/Infiltrator-Projects/ExtFS-for-Windows/pull/14

One-shot publication trigger only. Uses the already-green x64 WDK/package artifact from run 32569243508, verifies installer 0.9.4.0 and the unchanged 0.9.3.0 driver payload, publishes/verifies v0.9.4, then closes this PR and removes all temporary branches. This PR is not intended to merge.

## #15 — Publish verified ExtFS 0.9.5

- Type: pull request
- State: closed
- Created: 2026-08-22T11:42:14Z
- Closed: 2026-08-22T11:54:02Z
- Original: https://github.com/Infiltrator-Projects/ExtFS-for-Windows/pull/15

One-shot publication trigger for the already-committed 0.9.5 architecture-detection hotfix. Builds the exact main source plus this workflow-only trigger, verifies Common 1.9.0 and installer contracts, publishes v0.9.5, then closes and deletes this temporary branch. This PR is not intended to merge.

### Preserved discussion

**github-actions[bot] — 2026-08-22T11:54:01Z**

ExtFS 0.9.5 published and verified; removing one-shot publication branch.

## #16 — [HAL] Run ExtFS pre-MS hardening

- Type: issue
- State: closed
- Created: 2026-08-22T13:32:25Z
- Closed: 2026-08-22T13:34:53Z
- Original: https://github.com/Infiltrator-Projects/ExtFS-for-Windows/issues/16

Superseded before execution because the initial qualification workflow YAML was syntactically invalid. No source hardening ran from this trigger. Replaced by a fresh trigger after correcting and validating the workflow syntax.

## #17 — [HAL] Run ExtFS pre-MS hardening

- Type: issue
- State: closed
- Created: 2026-08-22T13:35:02Z
- Closed: 2026-08-22T13:36:58Z
- Original: https://github.com/Infiltrator-Projects/ExtFS-for-Windows/issues/17

The gate triggered correctly but stopped before publishing any filesystem source change. The initial failure report was too terse to identify the failing stage. Superseded by an instrumented rerun that records stage outcomes and build/test log tails.

### Preserved discussion

**github-actions[bot] — 2026-08-22T13:35:10Z**

Pre-MS hardening started on main at a5fc64db4a75befeb1c9e09e53cfdd55bd8d72c1. Applying the audited lifecycle/name fixes, then running portable and real-image qualification.

**github-actions[bot] — 2026-08-22T13:35:13Z**

Pre-MS hardening stopped before publication: the source transformation or portable qualification failed. No hardening source commit was pushed by this run.

## #18 — [HAL] Run ExtFS pre-MS hardening

- Type: issue
- State: closed
- Created: 2026-08-22T13:37:07Z
- Closed: 2026-08-22T13:39:43Z
- Original: https://github.com/Infiltrator-Projects/ExtFS-for-Windows/issues/18

Instrumented run identified the exact source-transformer failure: the write and FileEndOfFile set-information handlers have different guard layouts, while the transformer incorrectly required two identical anchors. No filesystem source change was published. Transformer corrected with separate exact anchors and additional dismounted guards for query-volume/device-control paths.

### Preserved discussion

**github-actions[bot] — 2026-08-22T13:37:15Z**

Pre-MS hardening started on main at f30c33f6a551c59b7efd0e68beeff150cc226ed2. Applying the audited lifecycle/name fixes, then running portable and real-image qualification.

**github-actions[bot] — 2026-08-22T13:37:17Z**

Pre-MS hardening stopped before publication. No hardening source commit was pushed by this run.

Stage outcomes: transform=failure, docs=skipped, qualify=skipped, publish=skipped

--- extfs-transform.log tail ---
write/set-information guard anchors missing


## #19 — [HAL] Run ExtFS pre-MS hardening

- Type: issue
- State: closed
- Created: 2026-08-22T13:39:52Z
- Closed: 2026-08-22T13:55:13Z
- Original: https://github.com/Infiltrator-Projects/ExtFS-for-Windows/issues/19

Portable/real-image/contracts/cross-build all passed, but this run used the old direct-main publication step and correctly failed at publication. Superseded by the detached-candidate gate, which keeps main untouched until x64 and ARM64 WDK both pass.

### Preserved discussion

**github-actions[bot] — 2026-08-22T13:40:00Z**

Pre-MS hardening started on main at 0c3566a62e69df4eb485c6c729518350e05ad4a4. Applying the audited lifecycle/name fixes, then running portable and real-image qualification.

**github-actions[bot] — 2026-08-22T13:40:32Z**

Pre-MS hardening stopped before publication. No hardening source commit was pushed by this run.

Stage outcomes: transform=success, docs=success, qualify=success, publish=failure

--- extfs-transform.log tail ---

--- extfs-qualification.log tail ---
e2fsprogs set to manually installed.
make is already the newest version (4.3-4.1build2).
gcc is already the newest version (4:13.2.0-7ubuntu1).
The following additional packages will be installed:
  gcc-mingw-w64-base gcc-mingw-w64-x86-64-posix
  gcc-mingw-w64-x86-64-posix-runtime gcc-mingw-w64-x86-64-win32
  gcc-mingw-w64-x86-64-win32-runtime mingw-w64-common mingw-w64-x86-64-dev
Suggested packages:
  gcc-13-locales wine64
The following NEW packages will be installed:
  binutils-mingw-w64-x86-64 gcc-mingw-w64-base gcc-mingw-w64-x86-64
  gcc-mingw-w64-x86-64-posix gcc-mingw-w64-x86-64-posix-runtime
  gcc-mingw-w64-x86-64-win32 gcc-mingw-w64-x86-64-win32-runtime
  mingw-w64-common mingw-w64-x86-64-dev
0 upgraded, 9 newly installed, 0 to remove and 28 not upgraded.
Need to get 115 MB of archives.
After this operation, 587 MB of additional disk space will be used.
Get:1 file:/etc/apt/apt-mirrors.txt Mirrorlist [144 B]
Get:2 http://azure.archive.ubuntu.com/ubuntu noble/universe amd64 binutils-mingw-w64-x86-64 amd64 2.41.90.20240122-1ubuntu1+11.4 [6053 kB]
Get:3 http://azure.archive.ubuntu.com/ubuntu noble/universe amd64 mingw-w64-common all 11.0.1-3build1 [5580 kB]
Get:4 http://azure.archive.ubuntu.com/ubuntu noble/universe amd64 mingw-w64-x86-64-dev all 11.0.1-3build1 [3786 kB]
Get:5 http://azure.archive.ubuntu.com/ubuntu noble/universe amd64 gcc-mingw-w64-base amd64 13.2.0-6ubuntu1+26.1 [188 kB]
Get:6 http://azure.archive.ubuntu.com/ubuntu noble/universe amd64 gcc-mingw-w64-x86-64-posix-runtime amd64 13.2.0-6ubuntu1+26.1 [14.0 MB]
Get:7 http://azure.archive.ubuntu.com/ubuntu noble/universe amd64 gcc-mingw-w64-x86-64-posix amd64 13.2.0-6ubuntu1+26.1 [35.4 MB]
Get:8 http://azure.archive.ubuntu.com/ubuntu noble/universe amd64 gcc-mingw-w64-x86-64-win32-runtime amd64 13.2.0-6ubuntu1+26.1 [14.2 MB]
Get:9 http://azure.archive.ubuntu.com/ubuntu noble/universe amd64 gcc-mingw-w64-x86-64-win32 amd64 13.2.0-6ubuntu1+26.1 [35.4 MB]
Get:10 http://azure.archive.ubuntu.com/ubuntu noble/universe amd64 gcc-mingw-w64-x86-64 all 13.2.0-6ubuntu1+26.1 [187 kB]
Fetched 115 MB in 1s (85.2 MB/s)
Selecting previously unselected package binutils-mingw-w64-x86-64.
(Reading database ... (Reading database ... 5%(Reading database ... 10%(Reading database ... 15%(Reading database ... 20%(Reading database ... 25%(Reading database ... 30%(Reading database ... 35%(Reading database ... 40%(Reading database ... 45%(Reading database ... 50%(Reading database ... 55%(Reading database ... 60%(Reading database ... 65%(Reading database ... 70%(Reading database ... 75%(Reading database ... 80%(Reading database ... 85%(Reading database ... 90%(Reading database ... 95%(Reading database ... 100%(Reading database ... 203124 files and directories currently installed.)
Preparing to unpack .../0-binutils-mingw-w64-x86-64_2.41.90.20240122-1ubuntu1+11.4_amd64.deb ...
Unpacking binutils-mingw-w64-x86-64 (2.41.90.20240122-1ubuntu1+11.4) ...
Selecting previously unselected package mingw-w64-common.
Preparing to unpack .../1-mingw-w64-common_11.0.1-3build1_all.deb ...
Unpacking mingw-w64-common (11.0.1-3build1) ...
Selecting previously unselected package mingw-w64-x86-64-dev.
Preparing to unpack .../2-mingw-w64-x86-64-dev_11.0.1-3build1_all.deb ...
Unpacking mingw-w64-x86-64-dev (11.0.1-3build1) ...
Selecting previously unselected package gcc-mingw-w64-base:amd64.
Preparing to unpack .../3-gcc-mingw-w64-base_13.2.0-6ubuntu1+26.1_amd64.deb ...
Unpacking gcc-mingw-w64-base:amd64 (13.2.0-6ubuntu1+26.1) ...
Selecting previously unselected package gcc-mingw-w64-x86-64-posix-runtime.
Preparing to unpack .../4-gcc-mingw-w64-x86-64-posix-runtime_13.2.0-6ubuntu1+26.1_amd64.deb ...
Unpacking gcc-mingw-w64-x86-64-posix-runtime (13.2.0-6ubuntu1+26.1) ...
Selecting previously unselected package gcc-mingw-w64-x86-64-posix.
Preparing to unpack .../5-gcc-mingw-w64-x86-64-posix_13.2.0-6ubuntu1+26.1_amd64.deb ...
Unpacking gcc-mingw-w64-x86-64-posix (13.2.0-6ubuntu1+26.1) ...
Selecting previously unselected package gcc-mingw-w64-x86-64-win32-runtime.
Preparing to unpack .../6-gcc-mingw-w64-x86-64-win32-runtime_13.2.0-6ubuntu1+26.1_amd64.deb ...
Unpacking gcc-mingw-w64-x86-64-win32-runtime (13.2.0-6ubuntu1+26.1) ...
Selecting previously unselected package gcc-mingw-w64-x86-64-win32.
Preparing to unpack .../7-gcc-mingw-w64-x86-64-win32_13.2.0-6ubuntu1+26.1_amd64.deb ...
Unpacking gcc-mingw-w64-x86-64-win32 (13.2.0-6ubuntu1+26.1) ...
Selecting previously unselected package gcc-mingw-w64-x86-64.
Preparing to unpack .../8-gcc-mingw-w64-x86-64_13.2.0-6ubuntu1+26.1_all.deb ...
Unpacking gcc-mingw-w64-x86-64 (13.2.0-6ubuntu1+26.1) ...
Setting up binutils-mingw-w64-x86-64 (2.41.90.20240122-1ubuntu1+11.4) ...
Setting up gcc-mingw-w64-base:amd64 (13.2.0-6ubuntu1+26.1) ...
Setting up gcc-mingw-w64-x86-64-win32-runtime (13.2.0-6ubuntu1+26.1) ...
Setting up mingw-w64-common (11.0.1-3build1) ...
Setting up mingw-w64-x86-64-dev (11.0.1-3build1) ...
Setting up gcc-mingw-w64-x86-64-posix-runtime (13.2.0-6ubuntu1+26.1) ...
Setting up gcc-mingw-w64-x86-64-posix (13.2.0-6ubuntu1+26.1) ...
update-alternatives: using /usr/bin/x86_64-w64-mingw32-gcc-posix to provide /usr/bin/x86_64-w64-mingw32-gcc (x86_64-w64-mingw32-gcc) in auto mode
Setting up gcc-mingw-w64-x86-64-win32 (13.2.0-6ubuntu1+26.1) ...
update-alternatives: using /usr/bin/x86_64-w64-mingw32-gcc-win32 to provide /usr/bin/x86_64-w64-mingw32-gcc (x86_64-w64-mingw32-gcc) in auto mode
Setting up gcc-mingw-w64-x86-64 (13.2.0-6ubuntu1+26.1) ...
Processing triggers for man-db (2.12.0-4build2) ...
Not building database; man-db/auto-update is not 'true'.

Running kernel seems to be up-to-date.

No services need to be restarted.

No containers need to be restarted.

No user sessions are running outdated binaries.

No VM guests are running outdated hypervisor (qemu) binaries on this host.
rm -rf build
mkdir -p build
cc  -O2 -Wall -Wextra -Wpedantic -Werror -std=c11 -Iinclude -c core/extfs.c -o build/extfs.o
ar rcs build/libextfs.a build/extfs.o
cc  -O2 -Wall -Wextra -Wpedantic -Werror -std=c11 -Iinclude -c tools/extfs-tool.c -o build/extfs-tool.o
cc -O2  build/extfs-tool.o build/libextfs.a  -o build/extfs-tool
cc  -O2 -Wall -Wextra -Wpedantic -Werror -std=c11 -Iinclude -c tests/test_extfs.c -o build/test-extfs.o
cc -O2  build/test-extfs.o build/libextfs.a  -o build/test-extfs
cc  -O2 -Wall -Wextra -Wpedantic -Werror -std=c11 -Iinclude -c tests/mutate_image.c -o build/mutate_image.o
cc -O2  build/mutate_image.o build/libextfs.a  -o build/extfs-mutate-test
All ExtFS unit tests passed.
sh tests/integration.sh
ext2 /hello.txt resized to 4096 bytes and reopened successfully
ext2 /hello.txt resized to 7 bytes and reopened successfully
ext3 /hello.txt resized to 4096 bytes and reopened successfully
ext3 /hello.txt resized to 7 bytes and reopened successfully
ext4 /hello.txt resized to 4096 bytes and reopened successfully
ext4 /hello.txt resized to 7 bytes and reopened successfully
Ext2, ext3 and ext4 read/traversal and real-image resize tests passed.
Filesystem lifecycle/name/version contracts: PASS
/home/runner/work/ExtFS-for-Windows/ExtFS-for-Windows/build-cross/extfs.sys


## #20 — [HAL] Run ExtFS pre-MS hardening

- Type: issue
- State: closed
- Created: 2026-08-22T13:55:28Z
- Closed: 2026-08-22T14:04:55Z
- Original: https://github.com/Infiltrator-Projects/ExtFS-for-Windows/issues/20

Superseded by the simplified v3 issue-triggered gate. The push-triggered route is not reliable for connector-created commits in this repository. No filesystem source promotion occurred from this issue.

## #21 — [HAL] Run ExtFS pre-MS v3

- Type: issue
- State: closed
- Created: 2026-08-22T14:05:07Z
- Closed: 2026-08-22T23:42:52Z
- Original: https://github.com/Infiltrator-Projects/ExtFS-for-Windows/issues/21

Run the simplified detached pre-Microsoft qualification gate. Apply the audited lifecycle/name correctness fixes, portable unit tests, real ext2/ext3/ext4 mke2fs/e2fsck mutation qualification, filesystem contracts and MinGW cross-build; then validate the exact patch through WDK Release x64 and ARM64. Do not move main from the workflow. Report the detached candidate SHA only after full PASS.

## #22 — [HAL] Pre-MS ping

- Type: issue
- State: closed
- Created: 2026-08-22T14:06:35Z
- Closed: 2026-08-22T23:42:56Z
- Original: https://github.com/Infiltrator-Projects/ExtFS-for-Windows/issues/22

One-shot Actions issue-dispatch probe. No source changes.

## #23 — [HAL] Pre-MS ping

- Type: issue
- State: closed
- Created: 2026-08-22T14:07:06Z
- Closed: 2026-08-22T23:43:00Z
- Original: https://github.com/Infiltrator-Projects/ExtFS-for-Windows/issues/23

Second issue-dispatch probe after allowing the workflow to register on the default branch. No source changes.

## #24 — Qualify pre-MS filesystem hardening

- Type: pull request
- State: closed
- Created: 2026-08-22T14:14:26Z
- Closed: 2026-08-22T14:46:58Z
- Original: https://github.com/Infiltrator-Projects/ExtFS-for-Windows/pull/24
- Merged: 2026-08-22T14:46:58Z

Temporary qualification PR only. The filesystem hardening has now passed portable CI, real ext2/ext3/ext4 image qualification, filesystem-contract regression checks, and Windows WDK Release packaging on both x64 and ARM64. Final step before merge: remove the temporary materializer/cleanup logic from the PR head, retain only the permanent filesystem-contract gates, rerun the exact clean candidate, then merge. After merge, all remaining temporary pre-MS workflow files on main will be removed in one cleanup commit and the temporary branch will be removed.

### Preserved discussion

**chatgpt-codex-connector[bot] — 2026-08-22T14:14:31Z**

You have reached your Codex usage limits for code reviews. You can see your limits in the [Codex usage dashboard](https://chatgpt.com/codex/cloud/settings/usage).

## #25 — Delete temporary pre-MS qualification branch

- Type: pull request
- State: closed
- Created: 2026-08-22T14:50:13Z
- Closed: 2026-08-22T14:51:38Z
- Original: https://github.com/Infiltrator-Projects/ExtFS-for-Windows/pull/25

Housekeeping completed successfully. The registered cleanup job deleted pre-ms-qualification-temp. This PR was never intended to merge.

### Preserved discussion

**chatgpt-codex-connector[bot] — 2026-08-22T14:50:19Z**

You have reached your Codex usage limits for code reviews. You can see your limits in the [Codex usage dashboard](https://chatgpt.com/codex/cloud/settings/usage).

## #26 — Qualify single-indirect resize implementation

- Type: pull request
- State: closed
- Created: 2026-08-22T22:49:46Z
- Closed: 2026-08-22T23:13:45Z
- Original: https://github.com/Infiltrator-Projects/ExtFS-for-Windows/pull/26

Qualification-only PR. The production implementation is already on main at 34ebc65d0f1d83aa7fc87ca96837b89009829ce9. This branch adds only a trigger file so the same source can be exercised by the repository's pull-request CI. Do not merge this PR.

### Preserved discussion

**chatgpt-codex-connector[bot] — 2026-08-22T22:49:50Z**

You have reached your Codex usage limits for code reviews. You can see your limits in the [Codex usage dashboard](https://chatgpt.com/codex/cloud/settings/usage).

## #27 — Require explicit approval for ExtFS publishers

- Type: pull request
- State: closed
- Created: 2026-08-23T03:41:11Z
- Closed: 2026-08-23T03:47:35Z
- Original: https://github.com/Infiltrator-Projects/ExtFS-for-Windows/pull/27
- Merged: 2026-08-23T03:47:35Z

Removes the magic commit-message publication triggers from both ExtFS release workflows. The 0.9.5 hotfix publisher and ARM64 preview publisher are now manual-only, check out and verify exact current `main`, refuse existing tags/releases, and target the checked-out main SHA. Portable CI now rejects any future automatic trigger in these publishers.

### Preserved discussion

**chatgpt-codex-connector[bot] — 2026-08-23T03:41:17Z**

You have reached your Codex usage limits for code reviews. You can see your limits in the [Codex usage dashboard](https://chatgpt.com/codex/cloud/settings/usage).

