# Validation

## Maintained entry points

[Architecture](ARCHITECTURE.md) owns subsystem boundaries. [Filesystem design index](FILESYSTEM_DESIGN_INDEX.md) maps each format's contract and provenance. The source, native deployment and desktop integration manifests own their separate membership sets; CI consumes those manifests.

From a recursive checkout with the required C/C++ toolchain and platform development packages:

```sh
bash tools/validate-source-contract.sh
python3 tests/repository_architecture_test.py .
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Source-contract checks use isolated temporary directories and may run concurrently. Build configuration reports platform/tool availability; compilation alone does not qualify mounted runtime behaviour.

## Qualification boundaries

The main build qualifies core/catalogue/action behaviour, native install membership, desktop APT install/reinstall/purge, the Windows manager and EXT2 driver, and managed module compilation against explicit kernel APIs. Release candidates also require reusable exact-SHA sanitizer/adversarial, EXT runtime and Amiga runtime workflows.

Those checks exercise different claims. A format's presence in the catalogue or a successful kernel compilation does not make it an installation target. Format-specific design and runtime evidence define the supported operations.

## Artifact ownership

VERSION owns release identity. The desktop qualification job creates the Debian package, verifies actual installation, embeds that exact package in the native wrapper, and seals both with SHA-256. The publisher consumes that bundle after all required lanes succeed.

Publication is owned by packaging/publish-release.sh. Only drafts may be repaired. Downloaded remote assets are compared with the qualified bundle before and after promotion. Public tags and bytes remain immutable. The central repository discovers releases independently; APT scheduling does not rebuild product artifacts.

## Environment evidence

Kernel loading, real media, firmware and physical-desktop behaviour require the appropriate isolated environment. Preserve test image identity, kernel/tool versions, exact source SHA and logs. Destructive tests use disposable media; release packaging never formats a developer's storage.
