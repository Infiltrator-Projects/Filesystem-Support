# Architecture

Filesystem Support is split into four layers.

1. **Catalogue** owns filesystem identity, Debian package requirements, kernel-module aliases, capability classification, support-provider classification and limitations.
2. **Probe** performs read-only inspection of installed packages, configured APT repositories, registered filesystems, built-in kernel drivers, loaded modules and loadable modules.
3. **Installer/action backend** is the only layer allowed to request privilege escalation. Package and module names can only originate from the built-in catalogue.
4. **GTK shell** presents status and actions without owning filesystem-specific rules.

The baseline distribution is Debian stable. The program must not silently add repositories, enable repository components, or cross from stable into testing, unstable or experimental.

## Support definition

A filesystem reports **Installed** when its declared kernel/userspace requirement and required Debian packages are present.

**Available** means required packages are missing but are resolvable by `apt-cache` from the repositories already configured by the user.

**Unavailable** means either the required kernel driver is absent with no packaged path to add it, or one or more required packages are not available from the configured repositories.

Kernel detection deliberately checks three sources: `/proc/filesystems` for registered/built-in support, `/sys/module` for loaded modules and `modinfo` for loadable modules. This avoids the bootstrap error of equating "modinfo can find it" with all possible kernel support.

## Action model

Each catalogue entry is classified as one of:

- **Kernel** — support is part of the running kernel package. Loadable modules can be loaded or unloaded; drivers compiled directly into the kernel cannot be removed by this application.
- **Kernel + userspace** — kernel driver availability is separate from Debian administration/mount tooling.
- **DKMS** — Debian packages provide an out-of-tree kernel driver plus any declared userspace components.
- **Userspace/FUSE** — the Debian package is the filesystem implementation.
- **Tools only** — the Debian package provides direct access/inspection/manipulation, not a normal mount driver.

For package removal the backend first executes an unprivileged `apt-get -s remove`. It parses Debian's planned removals and refuses the operation if any planned package is outside the filesystem catalogue or if Debian marks any planned package Essential, Protected, or priority `required`. The UI displays all catalogue entries affected by shared packages before asking for confirmation. It deliberately does not run `autoremove`.

Loadable kernel modules are managed through `modprobe` and `modprobe -r`. A busy module simply fails to unload; the application never deletes `.ko` files or removes the running kernel package.

## Privilege boundary

The GUI, probes and package-removal simulation run unprivileged. Actual APT and module actions are delegated through PolicyKit. No search text, label or other free-form user input is passed to the package manager or modprobe; all package and module arguments must pass the compiled catalogue allowlist.

## Shared code

Infiltratr Common is pinned as a git submodule. Filesystem Support consumes its project identity validation, deterministic case-insensitive search primitive and canonical design metrics instead of copying those functions locally.
