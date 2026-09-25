# Native filesystem engines

Each directory below this point owns one catalogue filesystem identity. The
directory layout also states the implementation's provenance and maturity; a
copied reference tree is never presented as a Filesystem Support implementation.

## Project-authored implementation state

A conventional local filesystem that has entered project development uses:

```text
<filesystem>/
  DESIGN.md
  core/       canonical filesystem semantics
  linux/      thin Linux VFS/module adapter, when required
  windows/    thin Windows IFS/WDK adapter, when required
```

Filesystem-defined parsing, allocation, mapping, namespace, metadata,
journaling/recovery and validation belong in `core/` whenever they can be
expressed without an operating-system object. Linux and Windows adapters must
not become competing filesystem implementations.

Not every filesystem needs both adapter directories. High-level remote,
device, cloud and similar protocols use the shared Filesystem Support
userspace-service boundary where that is the correct architecture instead of
being forced into a kernel module.

## Reference/import state

A filesystem that has not yet crossed the project-authorship boundary keeps
copied implementation source under an explicit provenance directory:

```text
<filesystem>/
  DESIGN.md                 reviewed ownership and target architecture
  reference/
    <origin>/               byte-preserved third-party reference source
```

Reference files retain their upstream names, licences, copyright and provenance.
They are evidence for behaviour and compatibility, not production source and
not project-authored code. Promotion into `core/`, `linux/`, `windows/`
or a filesystem-specific userspace adapter requires independent replacement,
tests and an explicit provenance decision.

The first alphabetic structural review applies this rule to `9p/`: the Linux
V9FS source is preserved under `9p/reference/linux/`, while future
Filesystem Support 9P code will use a canonical protocol/filesystem core and
the shared userspace-service architecture.

## General rules

EXT2, EXT3 and EXT4 remain independent implementations despite shared ancestry.
The Amiga-family implementations likewise remain independently owned where
their formats are distinct.

Canonical filesystem code must not contain GTK, APT, PolicyKit, subprocess or
host VFS/IFS objects. New native implementations begin read-only and fail
closed on unknown or contradictory metadata. Write support is a separate
maturity stage with explicit recovery and destructive-test evidence.
