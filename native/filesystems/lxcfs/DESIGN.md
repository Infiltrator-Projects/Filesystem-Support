# LXCFS Design and Ownership

## Classification

LXCFS is a userspace/FUSE virtual-filesystem provider for container-aware views
of selected procfs/sysfs/cgroup-derived information. It is not an on-disk
filesystem and does not own the host filesystems or kernel interfaces from
which its data is derived.

## Current implementation state

Filesystem Support contains no first-party LXCFS implementation.

The current product path is an external userspace provider managed through the
catalogue and action backend. The former `.gitkeep` represented no source and
is removed by this layout pass.

## Intended layout

```text
native/filesystems/lxcfs/
  DESIGN.md
  userspace/            container virtual-namespace provider, if implemented
```

There is deliberately no disk-format `core/`, Linux block-filesystem module or
project `.ko` here.

## Ownership

A future first-party implementation may own container-specific projection and
virtualisation policy for supported proc/sys/cgroup data, namespace mapping and
request lifecycle. It must not duplicate procfs, sysfs, cgroup or underlying
filesystem implementations.

Generic service lifetime, credential handling, path validation and structured
virtual-file presentation should remain shared infrastructure when reusable.

## Safety and failure behaviour

Container identity, namespace/cgroup state and requested paths are untrusted
inputs. A provider must prevent cross-container information exposure, reject
path escape, use bounded parsing, and define behaviour when host state changes
mid-request.

## Completion rule

The current entry is complete only as an external userspace virtual-filesystem
provider contract. It is not a project-authored storage filesystem.
