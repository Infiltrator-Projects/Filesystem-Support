# Native Code Architecture

## Greenfield rule

The native filesystem suite is a greenfield implementation.

The current Filesystem Support 0.3.x package/FUSE/DKMS manager remains a
transition provider while native engines are developed. It does not define the
internal architecture of the new engines.

The native layer begins from a small explicit contract rather than preserving
legacy interfaces from external filesystem tools, Defragmenter or
InfiltratorFS.

## Dependency direction

```text
                        Infiltratr Common
                               ^
                               |
                     userspace adapter
                               ^
                               |
Filesystem Support app --> native format engine <-- Linux kernel adapter
                               |
                               v
                    per-filesystem semantics
```

There is intentionally no dependency from the Linux kernel adapter to
Infiltratr Common.

## Shared engine contract

The shared engine is allocation-free at its lowest layer.

`IfsIo` provides exact positioned reads/writes over a bounded byte address
space. Filesystem parsers do not know whether bytes come from an image, raw
block device, fuzz buffer or Linux block device.

`IfsByteReader` provides bounds-checked decoding over already-read records.
Userspace endian operations delegate to Infiltratr Common. Kernel builds use
Linux unaligned/endian helpers behind the same API.

`IfsFilesystemDescriptor` is the small ABI-bearing identity/capability
contract for a native engine. It contains no GTK, package-manager or kernel
object pointers.

The shared core owns mechanisms only. FAT allocation rules, AFFS checksum
policy, NTFS attributes and other filesystem semantics stay local.

## How Common is used

Common remains authoritative in userspace for genuinely generic mechanisms.

The first userspace adapter delegates exact positioned I/O to
`infiltratr_pread_full()`; it does not create another short-read/EINTR loop.
The endian bridge delegates to Common's unaligned byte loaders.

As engines grow, reuse Common for identical contracts such as checked
arithmetic, UTF-8, deterministic ASCII handling, exact I/O, parsing and path
primitives.

If a native engine later develops a stronger generic primitive, compare it
forensically with Common, improve Common's generic contract where appropriate,
then remove the duplicate. Do not create another near-equivalent helper.

## Lessons leveraged from Defragmenter

Defragmenter is not a runtime dependency.

Its native engines provide engineering evidence we deliberately carry forward:

- exact positional I/O belongs in a shared filesystem-neutral layer;
- writable userspace raw-device tools bind stable target identity/capacity
  rather than trusting a pathname;
- unknown or contradictory metadata fails closed;
- format semantics remain with the owning filesystem;
- independently manufactured fixtures are stronger evidence than self-created
  test media;
- malformed-image matrices, sanitizer builds and sacrificial-media tests prove
  different things;
- mutation success requires a separate read-only verification pass;
- write transactions need explicit durable recovery boundaries.

Filesystem-specific Defragmenter implementations are not copied into the
shared core. If a future userspace writer needs its stable target-binding
mechanism, first decide whether the generic mechanism belongs in Common.

## Lessons leveraged from InfiltratorFS

InfiltratorFS is the reference for Linux VFS/module mechanics already proven
inside this organisation: composite Kbuild modules, filesystem registration,
`fs_context`, block-device mounts, inode/file/super operations, folio/page
cache integration, lock-order documentation, module aliases and lifecycle.

Its CoW, checkpoint, allocation, compression, security and on-disk policy are
InfiltratorFS semantics and are not inherited by compatibility drivers.

## Shared kernel core policy

Initially, each filesystem module compiles the native-core objects it needs
into its own `.ko`.

That deliberately trades a small amount of duplicate machine code for no
private module ABI, no load-order dependency, no core/driver version skew and
no single shared module that can disable every filesystem.

A separate `infiltratr-fs-core.ko` is considered only after several real
drivers have proven the interface stable.

## Development harness

`fsinspect` is the single generic userspace inspection harness. At this stage
it proves that the Common-backed I/O adapter and native core can safely open
and read an image or block device without introducing filesystem-specific
utilities.

As native engines arrive, fsinspect calls their probe/parser interfaces rather
than spawning one program per filesystem.

## First implementation

ROMFS is first. It will prove:

1. format structures and probe;
2. parser against independent fixtures;
3. malformed/truncated input rejection;
4. read-only directory/file traversal;
5. Linux VFS adapter;
6. `infiltratr-romfs.ko`;
7. differential testing beside Linux ROMFS;
8. Filesystem Support installation/autoload integration.

If ROMFS exposes a bad abstraction, fix the abstraction. Greenfield means no
compatibility obligation to an unreleased native-engine API.
