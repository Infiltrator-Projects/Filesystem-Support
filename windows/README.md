# Filesystems on Windows

The Windows product is the Windows front end for the same master filesystem
identity catalogue used elsewhere in Filesystems.

`windows/manager/` contains the management application. It displays every
catalogue entry and enables installation only when that filesystem has a
qualified Windows native implementation. There is deliberately no hard-coded
filesystem count: additions to the master catalogue automatically appear in
the Windows manager.

The management application is not a second filesystem implementation and is
not itself a compiler. Windows driver packages are built by the WDK build
pipeline; the manager selects, installs, removes, inspects and diagnoses
qualified packages.

Current architecture:

- `native/platform/windows/` — reusable Windows IFS/WDK adapter.
- `native/filesystems/<id>/core/` — canonical filesystem semantics shared
  with Linux.
- `native/filesystems/ext2/windows/` — current EXT2 proving adapter/package.
- `windows/build/` — Windows manager build plus EXT2 WDK build, validation and
  staging orchestration.
- `windows/test/` — host readiness and filesystem-contract qualification.
- `windows/manager/` — catalogue-driven GUI.

EXT2 is the current Windows proving path. Its WDK package is built and
statically validated, but it remains deliberately non-installable in the
manager until the Windows driver lifecycle, trust and runtime qualification
gates are complete.

The former ExtFS-for-Windows source tree is not a live repository authority.
Historical migration/provenance evidence and the remaining inherited EXT3/EXT4
responsibility boundaries are recorded in the maintained documentation under
`docs/`.
