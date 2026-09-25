# GPhotoFS Design and Ownership

## Classification

GPhotoFS exposes camera/PTP device storage as a userspace/FUSE namespace. It is
a device protocol adapter, not an on-disk filesystem.

The catalogue currently delegates this capability to Debian's `gphotofs`
package and libgphoto2-supported devices.

## Current state

Filesystem Support contains no first-party GPhotoFS implementation. The former
`.gitkeep` has been removed.

## Target architecture

```text
native/filesystems/gphotofs/
  DESIGN.md
  userspace/            camera/PTP namespace provider, if implemented
```

There is deliberately no `kernel/` and no disk-format `core/`.

Reusable USB/device discovery, PTP transport, cancellation and mount-service
mechanisms should be shared when their contracts are genuinely generic.

## Safety

Device responses, filenames and sizes are untrusted. Disconnects and transfer
failures must not leave partially published files presented as complete, and
path/name mapping must reject escape/collision conditions.

## Completion rule

The current entry is an external userspace device-provider catalogue contract.
