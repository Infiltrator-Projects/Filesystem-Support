# Windows build orchestration

`Build-Windows.ps1` is the Windows manager build entry point. It configures the
repository with CMake, builds the catalogue-driven manager and runs the
platform-neutral tests.

`Build-Ext2Driver.ps1` is the current WDK proving pipeline. It restores the
pinned WDK/SDK dependency set declared by `packages.config`, builds the EXT2
adapter for x64 or ARM64, runs driver code analysis, validates the release INF,
checks PE architecture, generates the catalogue from the exact staged SYS/INF
pair and writes a build report with SHA-256 identities. The checked-in INF owns
`DriverVer`; the build script consumes that value rather than maintaining a
second version/date authority.

Building and statically validating the EXT2 package does **not** make it
installable. The Windows manager continues to expose installation only after a
filesystem's driver lifecycle, trust and runtime qualification gates have been
completed. EXT2 remains the proving path for those remaining gates.

The build pipeline uses the canonical EXT2 core plus the Windows adapter. The
former ExtFS-for-Windows duplicate core is not a live build input or repository
authority; its historical provenance is documented under `docs/`.

The long-term build surface remains the `filesystem-support.exe` manager, the
reusable Windows platform adapter, and each Windows-qualified canonical
filesystem module. New filesystems should reuse the general WDK mechanisms only
where their contracts are genuinely common; format-specific semantics remain in
the corresponding filesystem core/adapter.
