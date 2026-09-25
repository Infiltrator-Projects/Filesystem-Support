# jmtpfs Design and Ownership

## Classification

jmtpfs exposes Media Transfer Protocol (MTP) devices through a userspace/FUSE
namespace. It is a device-protocol adapter, not an on-disk filesystem.

Filesystem Support must therefore treat jmtpfs as an external userspace
provider identity rather than as a candidate native block filesystem or kernel
module.

## Current implementation state

Filesystem Support contains no first-party jmtpfs implementation.

The current product path is an external provider managed through the catalogue
and action backend. The former `.gitkeep` represented no implementation and
is removed by this layout pass.

## Intended layout

```text
native/filesystems/jmtpfs/
  DESIGN.md
  userspace/            MTP namespace/provider glue, if ever implemented
```

There is deliberately no disk-format `core/`, Linux VFS filesystem engine or
project `.ko` for this provider.

If a first-party MTP provider is later admitted, generic USB/device discovery,
request lifetime, cancellation, user-session service and mount presentation
belong in shared Filesystem Support userspace infrastructure when their
contracts are genuinely device-neutral. jmtpfs-specific code may own MTP
storage/object enumeration and translation into filesystem-style paths.

jmtpfs and go-mtpfs are alternative provider identities for the same protocol
class. They must not grow independent copies of a project-owned MTP protocol
engine.

## Safety and failure behaviour

Device replies, object names and object sizes are untrusted. A future provider
must bound transfers, handle disconnect/cancellation without exposing partial
writes as complete objects, reject path escape/collision, and keep credentials
or pairing material out of logs.

## Completion rule

The current entry is complete only as an external userspace/device-provider
catalogue contract. It is not a project-authored filesystem implementation.
