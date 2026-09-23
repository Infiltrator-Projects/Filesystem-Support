# Filesystem Support on Windows

The Windows product is the Windows front end for the same master filesystem
catalogue used elsewhere in Filesystem Support.

`windows/manager/` contains the management application. It displays every
catalogue entry and enables installation only when that filesystem has a
qualified Windows native implementation. There is deliberately no hard-coded
filesystem count: additions to the master catalogue automatically appear in
the Windows manager.

The management application is not a second filesystem implementation and is
not itself a compiler. Windows driver packages are built by the WDK build
pipeline; the manager selects, installs, removes, inspects and diagnoses
qualified packages.

Planned live layout:

- `native/platform/windows/` — reusable Windows IFS/WDK adapter.
- `native/filesystems/<id>/core/` — canonical filesystem semantics shared
  with Linux.
- `windows/build/` — WDK build, package validation and signing orchestration.
- `windows/installer/` — installation/package support used by the manager.
- `windows/test/` — host readiness, Driver Verifier, diagnostics and
  filesystem-contract qualification.
- `windows/manager/` — catalogue-driven GUI.

The former ExtFS-for-Windows tree remains archived byte-for-byte as migration
evidence until the EXT2 proving path has replaced its independent EXT core.
