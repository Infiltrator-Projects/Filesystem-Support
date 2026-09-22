# Architecture

Filesystem Support is split into four layers.

1. **Catalogue** owns filesystem identity, Debian package requirements, kernel-module aliases, capability classification and limitations.
2. **Probe** performs read-only inspection of installed packages, configured APT repositories, registered filesystems, loaded kernel modules and loadable modules.
3. **Installer** is the only layer allowed to request privilege escalation. Package names can only originate from the built-in catalogue.
4. **GTK shell** presents status and actions without owning filesystem-specific rules.

The baseline distribution is Debian stable. The program must not silently add repositories, enable repository components, or cross from stable into testing, unstable or experimental.

## Support definition

A filesystem reports **Installed** when its declared kernel/userspace requirement and required Debian packages are present.

**Available** means required packages are missing but are resolvable by `apt-cache` from the repositories already configured by the user.

**Unavailable** means either the required kernel driver is absent with no packaged path to add it, or one or more required packages are not available from the configured repositories.

Kernel detection deliberately checks three sources: `/proc/filesystems` for registered/built-in support, `/sys/module` for loaded modules and `modinfo` for loadable modules. This avoids the bootstrap error of equating "modinfo can find it" with all possible kernel support.

## Privilege boundary

The GUI and probes run unprivileged. Installation is delegated to PolicyKit and APT. No search text, label or other free-form user input is passed to the package manager; install arguments are generated only from catalogue entries.

## Shared code

Infiltratr Common is pinned as a git submodule. Filesystem Support consumes its project identity validation, deterministic case-insensitive search primitive and canonical design metrics instead of copying those functions locally.
