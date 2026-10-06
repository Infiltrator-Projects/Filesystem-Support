# Native filesystem engines

This tree is reserved for real on-media filesystem formats that belong to the
current 41-format Filesystem Support contract. A directory exists here only
when this repository carries implementation or retained reference material for
that filesystem identity. Catalogue identities provided by another project or
external provider do not require a duplicate source tree here.

Provider names, network or cloud clients, overlays, archive namespaces,
encryption/container layers, device namespaces and alternate FUSE/DKMS access
paths are not filesystem identities and must not reappear as directories under
`native/filesystems/`.

The directory layout also states an implementation's provenance and maturity;
a copied reference tree is never presented as a Filesystem Support
implementation.

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

Not every filesystem needs both adapter directories. The presence of an
adapter is determined by the implementation actually owned by this repository,
not by creating a parallel tree for every possible Linux access provider.

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

## General rules

EXT2, EXT3 and EXT4 remain independent implementations despite shared ancestry.
The Amiga-family implementations likewise remain independently owned where
their formats are distinct.

Canonical filesystem code must not contain GTK, APT, PolicyKit, subprocess or
host VFS/IFS objects. New native implementations begin read-only and fail
closed on unknown or contradictory metadata. Write support is a separate
maturity stage with explicit recovery and destructive-test evidence.

The authoritative list of selectable filesystem identities is
`docs/FILESYSTEM_SUPPORT_MATRIX.md`. The project-native Linux deployment subset
is separately and centrally defined by `data/native-filesystems.tsv`; neither
contract is inferred from whatever directories happen to exist here.

## Canonical source ownership

For the completed reference set — EXT2, EXT3, EXT4, OFS, FFS and SFS — each
filesystem has one canonical Filesystem Support implementation. Active source
for those six must be project-authored and maintained by Shannon Smith.
Alternate FUSE providers, vendor copies, imported driver bodies and duplicate
implementation trees are not accepted.

A source audit finding another implementation's author, copyright or provenance
in an active implementation is a replacement defect, not a documentation-only
defect. The affected code must be independently reimplemented before
attribution can disappear.
