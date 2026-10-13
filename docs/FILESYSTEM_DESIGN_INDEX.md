# Filesystem Design Documents

Filesystem design documents record on-media format facts, current ownership state and the intended canonical-engine/platform-adapter boundary. A design document does not by itself claim that the filesystem has been rewritten, made writable, made bootable or qualified as an InfiltratorOS root.

## Authorities

Filesystem-set membership is not maintained in this document.

- [`../data/source-filesystems.txt`](../data/source-filesystems.txt) is the sole authority for filesystem source/design directories owned by this repository.
- [`../data/native-filesystems.tsv`](../data/native-filesystems.tsv) is the sole authority for project-native Linux deployment membership and module identity.
- [`../data/desktop-filesystems.tsv`](../data/desktop-filesystems.tsv) is the sole authority for per-filesystem desktop integration membership.
- [`FILESYSTEM_SUPPORT_MATRIX.md`](FILESYSTEM_SUPPORT_MATRIX.md), together with the compiled catalogue identity/policy join, records the selectable product catalogue and current provider/capability state.
- Each `native/filesystems/<id>/DESIGN.md` is authoritative for that format's implementation/provenance state.

This index therefore does not repeat a second complete filesystem list. CI validates the source manifest against the directory tree and requires every source filesystem to carry a non-empty `DESIGN.md`.

## Active project-native formats

The current native deployment manifest contains the independent EXT2, EXT3, EXT4, Amiga OFS, Amiga FFS, SFS, SFS2 and PFS3 implementations. Their design documents define filesystem-specific semantics and ownership; the deployment manifest defines only the Linux filesystem/module pairs.

These are deliberately independent filesystem implementations. Similarity between related formats is not, by itself, a reason to merge their persistent semantics or runtime state. Only mechanisms that are genuinely filesystem-neutral belong in shared native/platform code.

InfiltratorFS is maintained in its separate repository. Filesystem Support contains its catalogue/provider contract, not a duplicate InfiltratorFS source tree.

## Reference/import and external-provider states

A source-manifest directory can legitimately contain only `DESIGN.md`, or `DESIGN.md` plus provenance-preserving `reference/` evidence, while no project-authored canonical engine exists. That state is intentional and must not be presented as native implementation progress.

Imported reference source remains evidence only. It is never linked into an active Filesystem Support implementation merely because it is present in the repository. Promotion requires independently authored canonical semantics, an explicit adapter boundary and qualification for every capability claimed by the format's design document.

Conversely, some catalogue formats are currently served entirely by external providers and need no local source directory. Catalogue membership and repository source-tree membership are different concerns and must not be inferred from one another.

## Retired identities

[`../data/retired-source-identities.txt`](../data/retired-source-identities.txt) is the guardrail for historical provider, transport, overlay, archive, container and other non-format source identities that must not return as native filesystem trees. The Linux-reference refresh machinery must operate only on current in-scope reference trees; it may not recreate a retired identity.

Provider names such as NTFS3, zfs-fuse or APFS provider variants never acquire an independent filesystem identity. Provider choice remains subordinate to the one canonical on-media format identity.

## Documentation rule

A filesystem design document must distinguish, where applicable:

1. on-media format facts;
2. capabilities of an external/current provider;
3. project-native implementation state;
4. Linux/Windows adapter state;
5. EFI/rEFInd reader state; and
6. InfiltratorOS root/setup qualification state.

Those states are not interchangeable. A format can be well documented but unimplemented, mountable but not safely writable, writable but not suitable as `/`, or root-capable in Linux while still lacking an EFI reader.

## Bootable-root rule

For the InfiltratorOS setup path, the format design is only one part of the contract. A filesystem is offered as an installation target only after its formatter, Linux root semantics, early-boot module availability and matching rEFInd/EFI reader are qualified together.

The setup shell consumes the same stable format identity contract as the desktop manager. It does not maintain a second filesystem list.
