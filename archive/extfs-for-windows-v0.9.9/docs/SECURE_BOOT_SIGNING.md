<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Secure Boot signing and release policy

Ext Filesystem Driver is intended to run on normal Windows systems with UEFI Secure Boot enabled. Disabling Secure Boot is a development-only fallback and is not an acceptable production requirement.

## Release classes

1. Development/test build
   - Uses a project-generated test certificate.
   - Intended only for driver development.
   - Not a public release target.

2. Secure-Boot preproduction build
   - Uses a Microsoft Hardware Dev Center preproduction signature.
   - Intended for controlled validation with Secure Boot enabled on a provisioned test machine.

3. Production build
   - Uses a Microsoft Hardware Dev Center production signature obtained through the Windows Hardware Compatibility Program (WHCP/HLK path).
   - This is the normal public-release target and is expected to load on stock supported Windows systems with Secure Boot enabled, subject to Windows driver policy.

## Hardware Dev Center submission

`windows/Build-HardwareSubmission.ps1` builds and validates the driver, stages `extfs.sys`, `extfs.inf`, `extfs.pdb`, and `extfs.cat` in a driver subdirectory, and creates a CAB suitable for Hardware Dev Center submission.

The CAB must be signed with the EV code-signing certificate registered to the Hardware Dev Center account before submission. Microsoft signs the returned driver package and regenerates/replaces the catalog during processing.

Example:

```powershell
.\windows\Build-HardwareSubmission.ps1 -Platform x64
```

With an available EV certificate in the current-user certificate store:

```powershell
.\windows\Build-HardwareSubmission.ps1 -Platform x64 -EvCertificateThumbprint <THUMBPRINT>
```

## Production installer rule

A public ExtFS installer must never enable TESTSIGNING, import the project test certificate, or ask the user to disable Secure Boot. The production installer must consume only the Microsoft-signed driver package returned by Hardware Dev Center.

The current 0.9.x self-signed installer remains an experimental development checkpoint until the Microsoft production-signing step is completed.

## Shared dependency

The Windows adapter remains pinned to Infiltratr Common 1.19.24. Driver signing does not alter that dependency boundary.
