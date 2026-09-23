# Windows build orchestration

`Build-Windows.ps1` is the live Windows build entry point for Filesystem Support.

Today it configures the repository with CMake, builds the catalogue-driven Windows manager and runs the platform-neutral tests. It deliberately does not build or install a filesystem kernel driver yet because no Windows filesystem module has passed the shared-engine qualification gate.

As EXT2 reaches the next migration milestone, this directory will absorb the general WDK mechanisms preserved from ExtFS-for-Windows: pinned WDK/SDK tool acquisition, x64 and ARM64 builds, warnings-as-errors, driver code analysis, INF validation, PE architecture validation, catalogue generation, signing/staging and reproducible package manifests.

Those mechanisms must be generalized around the Windows adapter/module model. The archived `extfs.vcxproj` must not be revived unchanged because it directly compiles the duplicate `core/extfs*.c` implementation.

The long-term build produces the `filesystem-support.exe` manager, the reusable Windows platform adapter, and each Windows-qualified canonical filesystem module. A filesystem becomes installable in the manager only after its corresponding driver package has completed qualification.
