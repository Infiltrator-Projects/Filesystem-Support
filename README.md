# Filesystem Support

Filesystem Support is a native Debian desktop utility for discovering and enabling filesystem implementations without requiring users to remember package names, kernel modules or FUSE drivers.

The baseline is **Debian stable**. As of September 2026 that is Debian 13 "trixie". Derivatives can benefit where they retain Debian-compatible package names and APT behaviour, but the catalogue is deliberately authored against Debian first.

The program presents filesystem and filesystem-like mount implementations by human name, detects support already present on the running system, checks whether missing packages are actually available from the user's configured Debian repositories, and exposes safe Install/Remove package actions plus Load/Unload actions for loadable kernel modules. Drivers compiled directly into the running kernel are reported as built-in rather than pretending they can be uninstalled.

## Scope

The catalogue covers more than 100 Debian-stable filesystem and storage-namespace implementations: native Linux filesystems, historical Unix/workstation formats, Microsoft and Apple formats, Amiga and retro filesystems, optical/image formats, clustered/distributed filesystems, network and cloud mounts, FUSE implementations, encrypted overlays, archive mounts, device filesystems and virtualisation filesystems.

The catalogue intentionally includes kernel-only support where Debian may ship the driver but no separate userspace package. It also records important limitations such as read-only support, experimental write support, deprecated drivers and formats whose userspace tools are not in Debian stable.

The complete, enumerated support contract is maintained in
[`docs/FILESYSTEM_SUPPORT_MATRIX.md`](docs/FILESYSTEM_SUPPORT_MATRIX.md). CI verifies that every compiled catalogue ID is represented there exactly once.

The long-term native implementation strategy is defined in
[`docs/NATIVE_FILESYSTEM_VISION.md`](docs/NATIVE_FILESYSTEM_VISION.md). The target is a coherent suite of Infiltrator-owned filesystem implementations: native out-of-tree VFS modules for conventional local filesystems, without requiring a custom kernel, and a shared userspace storage service only for targets that are fundamentally high-level network/device protocols.

The greenfield source architecture is defined in
[`docs/NATIVE_CODE_ARCHITECTURE.md`](docs/NATIVE_CODE_ARCHITECTURE.md). The repository now contains the first compilable kernel/userspace-neutral native core, a Common-backed userspace I/O adapter, the `fsinspect` development harness and the Kbuild/module layout that future native filesystems will use.

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

Filesystem Support is pinned to Infiltratr Common **1.19.38**, commit
`7070c5812b50821fd7580101cb2289a3184f6b2c`.

## Design

The application separates its filesystem catalogue, read-only support probing, installation backend and GTK shell. Adding another filesystem should normally be a catalogue change rather than a new GUI code path.

The Linux shell follows the Infiltrator OS control-centre visual language used by System Settings: a branded application header with integrated search, persistent platform/status navigation, a dashboard home page with live support metrics and quick actions, and card-based catalogue views. Filesystem actions remain driven by the same probe and installer contracts; the richer shell changes presentation and navigation without duplicating package or kernel-module policy.

## Licence

GPL-3.0-or-later.

## Releases

The main Debian package carries inactive desktop templates for each native Amiga
filesystem. **Install native** installs that filesystem’s own `.ko`, its scoped
udev identification rule and its single-filesystem Disks naming file together.
**Remove native** removes that same set and refreshes connected devices. OFS,
FFS, SFS, SFS2 and PFS3 each have separate active files; installing one cannot
activate another. Installing the manager on a fresh system activates no format.

Desktop mounts of OFS, FFS, SFS and SFS2 pass the requesting user’s UID and
primary GID through their own `setuid`/`setgid` mount options. This makes the
volume writable by that user while preserving existing Amiga protection bits
and other users’ access restrictions. An upgrade updates the active rules;
already mounted volumes receive the new ownership on their next mount. PFS3
remains read-only because its Linux adapter does not yet implement mutation.

GNOME Disks uses a launcher that loads only the independently installed naming
files for drivers present on the running kernel. The stock UDisks library and
original Disks executable retain their distribution ownership and updates.
Removing the final active filesystem restores the stock Disks launch path.
The naming files do not propagate into programs launched by Disks.

The retired supplemental desktop package is removed automatically during the
upgrade to 0.5.7. Existing native drivers receive only their matching desktop
files. Known APT cache copies of the retired package are cleared. No additional
Infiltrator desktop Debian package is built or distributed.

The native `.run` contains only the same qualified main Debian package and
installs it through APT. Its transaction permits removal of the retired package
and refuses removal of unrelated packages.

A successful release commit publishes:

- `Filesystem-Support-<version>-amd64.deb`
- `Filesystem-Support-<version>-amd64.run`
- `RELEASE_SHA256SUMS.txt`
- GitHub’s source ZIP and TAR.GZ archives for the release tag.

Release cleanup withdraws the retired desktop DEBs, the older installers that
depend on or embed them, and their dedicated CI artifacts. The APT catalogue
publishes only the main package. Git history records the correction; downloads
and backups made elsewhere remain outside repository control.
