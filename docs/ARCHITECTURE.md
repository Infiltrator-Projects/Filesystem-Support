# Architecture

Filesystem Support is split into four layers:

1. **Catalogue** owns filesystem identity, package requirements, kernel module requirements and capability notes.
2. **Probe** performs read-only inspection of installed packages and kernel module availability.
3. **Installer** is the only layer allowed to request privilege escalation; package names can only come from the built-in catalogue.
4. **GTK shell** presents status and actions without owning filesystem-specific rules.

A filesystem reports **Installed** when its declared kernel/userspace requirement and utility packages are present. **Available** means declared packages are missing. **Unavailable** means the package side is satisfied but the required kernel driver is absent.

Infiltratr Common is pinned as a git submodule and supplies project identity validation, case-insensitive search and canonical design metrics.
