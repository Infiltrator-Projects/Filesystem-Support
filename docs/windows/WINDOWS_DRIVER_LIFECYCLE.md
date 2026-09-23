# Windows filesystem-driver lifecycle

## Purpose

This document preserves and generalises the durable Windows driver engineering
learned in the former ExtFS-for-Windows repository. The old EXT-specific
installer and signing scripts are not production inputs.

## Build and package

A Windows filesystem module is built from the canonical filesystem engine plus
the Windows IFS adapter. The WDK build must use warnings as errors and driver
code analysis. The staged package must contain the exact release-controlled
INF and the final SYS.

Before publication:

1. validate the SYS PE machine against the requested x64 or ARM64 target;
2. validate the INF with InfVerif;
3. generate the CAT from the exact staged SYS and INF with Inf2Cat;
4. record SHA-256 hashes for SYS and CAT;
5. never compile an archived standalone filesystem engine into a live driver.

`windows/build/Build-Ext2Driver.ps1` is the first implementation of this
pipeline.

## Signing

Development signing and production signing are different trust paths.

For disposable development machines, a deliberately created test-signing
certificate may be used only when the operator has explicitly chosen a test
environment. The package CAT must be regenerated after the final SYS is
embedded-signed so its hashes describe the exact shipped bytes. Signature
inspection must be bounded and must verify catalogue membership.

Production distribution must follow Microsoft's supported driver-signing
process. Filesystem Support must not instruct production users to disable
Secure Boot or enable TESTSIGNING. A production installer must verify the
trusted signature and target architecture before changing driver state.

For Microsoft Hardware Program submission, stage the validated INF, SYS, CAT
and matching symbols in a dedicated CAB subdirectory. The submission CAB may
be signed with the organisation's registered signing identity where required.
Microsoft's returned signed package, not the pre-submission development
package, is the production input.

## Installation contract

Installation is a privileged action and remains disabled in the Filesystem
Support manager until that filesystem has passed the Windows qualification
gate.

For a qualified package, installation must:

- require elevation;
- verify native Windows architecture and the driver's PE machine;
- reject a pending-restart state when it could invalidate driver lifecycle
  assumptions;
- verify the expected production signature/trust state;
- install a filesystem service with the exact service image path, service type,
  demand-start policy and `File System` load-order group;
- validate the resulting service configuration before attempting to load it;
- surface recent Service Control Manager and Code Integrity diagnostics when
  load fails;
- never weaken Secure Boot automatically.

Removal must stop/delete the filesystem service when possible, handle the
resident-until-reboot case explicitly, remove only the selected driver's
payload, and never silently change global TESTSIGNING state.

## Qualification

The Windows qualification suite is separate from filesystem-format
qualification. It includes:

- clean build from a fresh checkout;
- WDK code analysis;
- INF/CAT/architecture validation;
- Driver Verifier on a disposable system;
- mount/dismount and removable-media lifecycle;
- repeated open/cleanup/close;
- FCB/CCB/VCB lifetime and share-access behaviour;
- Cache Manager and Memory Manager interactions;
- read/write failure handling and forced media removal;
- diagnostic collection after failures.

`windows/test/Enable-DriverVerifier.ps1`,
`windows/test/Disable-DriverVerifier.ps1`,
`windows/test/Test-HostReadiness.ps1` and
`windows/test/Collect-Diagnostics.ps1` are the live generic qualification
helpers.

## Retired standalone mechanisms

The former ExtFS NSIS setup, EXT-specific service names, self-signed release
installers and standalone publication workflows are retired. Their exact
v0.9.9 sources remain in the migration archive for provenance. Their durable
safety rules are represented above and in the live generic Filesystem Support
build/test paths.
