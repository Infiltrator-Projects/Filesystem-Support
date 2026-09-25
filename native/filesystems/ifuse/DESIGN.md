# ifuse Design and Ownership

## Classification

ifuse exposes Apple mobile-device file services through a userspace/FUSE
namespace. It is a device/service protocol adapter, not an on-disk filesystem
format.

The filesystem-like view is backed by Apple device services such as AFC rather
than by Filesystem Support parsing the device's internal APFS/HFS storage.

## Current implementation state

Filesystem Support contains no first-party ifuse implementation.

The current product path is an external userspace provider managed through the
catalogue/action backend. This directory records that ownership boundary.

## Intended layout

```text
native/filesystems/ifuse/
  DESIGN.md
  userspace/            Apple-device namespace/provider glue, if implemented
```

There is deliberately no local-disk `core/`, no Linux VFS filesystem engine
and no project kernel module here.

If a first-party provider is ever admitted, reusable USB/device discovery,
pairing/session lifetime, credential storage, request cancellation and mount
service mechanisms belong in shared userspace infrastructure when genuinely
provider-neutral.

ifuse-specific code may own the mapping between supported Apple device service
operations and the presented filesystem namespace.

## Ownership boundary

This provider must not implement APFS, HFS+ or another filesystem merely because
the device internally uses one of those formats. Device-service access and
on-disk filesystem support are separate layers.

## Safety and failure behaviour

Device replies, object names, sizes and connection state are untrusted.
A future implementation must bound transfers, reject path escape/collision,
handle disconnect/cancellation without stale writes, preserve explicit
read-only/read-write capability state and keep pairing/credential material out
of logs.

## Completion rule

The current entry is complete only as an external userspace/device-provider
catalogue contract. It is not a project-authored filesystem implementation.
