# Filesystem Support

Filesystem Support is a native Debian desktop utility for discovering and enabling filesystem implementations without requiring users to remember package names, kernel modules or FUSE drivers.

The baseline is **Debian stable**. As of September 2026 that is Debian 13 "trixie". Derivatives can benefit where they retain Debian-compatible package names and APT behaviour, but the catalogue is deliberately authored against Debian first.

The program presents filesystem and filesystem-like mount implementations by human name, detects support already present on the running system, checks whether missing packages are actually available from the user's configured Debian repositories, and exposes safe Install/Remove package actions plus Load/Unload actions for loadable kernel modules. Drivers compiled directly into the running kernel are reported as built-in rather than pretending they can be uninstalled.

## Scope

The catalogue covers more than 100 Debian-stable filesystem and storage-namespace implementations: native Linux filesystems, historical Unix/workstation formats, Microsoft and Apple formats, Amiga and retro filesystems, optical/image formats, clustered/distributed filesystems, network and cloud mounts, FUSE implementations, encrypted overlays, archive mounts, device filesystems and virtualisation filesystems.

The catalogue intentionally includes kernel-only support where Debian may ship the driver but no separate userspace package. It also records important limitations such as read-only support, experimental write support, deprecated drivers and formats whose userspace tools are not in Debian stable.

The complete, enumerated support contract is maintained in
[`docs/FILESYSTEM_SUPPORT_MATRIX.md`](docs/FILESYSTEM_SUPPORT_MATRIX.md). CI verifies that every compiled catalogue ID is represented there exactly once.

## Safety and package policy

- The GUI itself remains unprivileged.
- Read-only probing uses the local kernel, `dpkg-query` and `apt-cache`.
- Install requests are delegated to PolicyKit and APT.
- Package names come only from the built-in catalogue.
- The application does not add repositories, enable Debian components, or silently pull packages from testing/unstable/experimental.
- A package missing from the configured repositories is reported as unavailable rather than pretending it can be installed.
- Package removal is simulated first and is blocked if Debian would remove an unrelated or protected system package.
- The application never performs automatic `apt autoremove`.
- Loadable kernel modules may be loaded/unloaded; built-in drivers are never presented as removable.
- Experimental implementations are labelled explicitly.

## Shared library pin

Filesystem Support is pinned to Infiltratr Common **1.19.22**, commit
`302c44eb7436803dee020667453a9a0681da8bbf`.

## Design

The application separates its filesystem catalogue, read-only support probing, installation backend and GTK shell. Adding another filesystem should normally be a catalogue change rather than a new GUI code path.

## Licence

GPL-3.0-or-later.

## Releases

A successful release commit publishes a GitHub release with:

- `Filesystem-Support-<version>-amd64.deb`
- `Filesystem-Support-<version>-amd64.run`
- `RELEASE_SHA256SUMS.txt`
- GitHub's immutable source ZIP and TAR.GZ archives for the release tag.
