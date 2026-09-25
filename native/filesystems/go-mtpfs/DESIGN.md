# go-mtpfs Design and Ownership

## Classification

go-mtpfs exposes Media Transfer Protocol devices through a userspace/FUSE
namespace. It is a device protocol adapter, not an on-disk filesystem.

The catalogue currently delegates this path to Debian's `go-mtpfs` package.

## Current state

There is no first-party go-mtpfs implementation in Filesystem Support. The
former `.gitkeep` has been removed.

## Target architecture

```text
native/filesystems/go-mtpfs/
  DESIGN.md
  userspace/            MTP namespace/provider glue, if implemented
```

There is deliberately no `kernel/` or disk-format `core/`.

A first-party MTP provider should share USB/device discovery, request lifetime,
credential/user-session and mount-service mechanisms where those contracts are
generic. MTP-specific object/storage enumeration and namespace mapping stay in
the MTP provider.

## Safety

Device responses, object names and object sizes are untrusted. Requests must be
bounded, cancellations and disconnects must not leave stale writes, and path
mapping must prevent namespace escape/collision.

## Completion rule

This entry is an external userspace device-provider catalogue contract only.
