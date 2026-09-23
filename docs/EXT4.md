# EXT4 Native Driver

## Purpose

The EXT4 implementation is the full-featured modern extended filesystem driver owned by Filesystem Support.

Its deployment target is exactly one loadable kernel module:

```text
ext4.ko
```

The module registers only EXT4. It does not register EXT2 or EXT3 aliases and contains no compatibility-routing logic that deliberately mounts those filesystem types through the EXT4 registration path.

JBD2 and the metadata cache are implementation code embedded into `ext4.ko`; they are not separately deployed support modules.

## Source layout

The Linux implementation and remaining migration-era source now live at:

```text
native/filesystems/ext4/linux/
```

The current Linux migration tree contains 46 files.

`linux/` is the active adapter/migration location, not the permanent home of portable EXT4 semantics. As feature engines are independently rewritten, filesystem-format logic moves into `core/`; a future Windows adapter belongs under `windows/`.

EXT4 is intentionally much larger than EXT2 or EXT3 because its genuine feature engines are large and independent. The project removes tiny organisational splits, compatibility-only code and production-irrelevant tests, but it does not merge large subsystems merely to reduce a file count.

## Filesystem identity

The driver registers only:

```text
ext4
```

The source tree contains no EXT2/EXT3 registration, no `IS_EXT2_SB` or `IS_EXT3_SB` routing, no EXT2/EXT3 support-mask checks and no mount messages that describe mounting EXT2 or EXT3 through the EXT4 subsystem.

### On-disk identity limitation

EXT4 shares ancestry, magic values and some valid feature combinations with earlier extended filesystems. An EXT4 filesystem deliberately formatted with only an EXT3-compatible feature set may be indistinguishable from EXT3 using on-disk feature bits alone.

Therefore the driver can be EXT4-only in registration and implementation without inventing a false on-disk discriminator. We do not require an EXT4-only feature bit merely to make the label unambiguous, because that would reject valid EXT4 feature combinations.

## Preserved EXT4 feature set

The implementation deliberately keeps the complete current EXT4 feature machinery from the pinned Linux source, including the supported paths for:

- extents;
- 64-bit block addressing;
- delayed allocation;
- multiblock allocation;
- persistent preallocation;
- indirect-block compatibility paths that remain valid EXT4 semantics;
- extent status tracking;
- inline data;
- flexible block groups;
- meta block groups;
- bigalloc;
- huge files;
- large directories;
- directory indexing and hashing;
- directory link-count extensions;
- sparse superblocks and sparse-super2;
- metadata checksums;
- group-descriptor checksums;
- checksum seeds;
- extended inode sizes;
- extended-attribute inodes;
- user, trusted, Hurd and security xattrs;
- POSIX ACLs;
- quotas and project quotas;
- project IDs;
- encryption when the target kernel provides the generic fscrypt framework;
- fs-verity when the target kernel provides the generic verity framework;
- casefolded directories;
- multiple mount protection;
- online resize;
- extent migration;
- extent movement;
- filesystem mapping queries;
- orphan handling and orphan files;
- fast commit;
- JBD2 journal transactions, commit, checkpoint, recovery and revoke handling;
- external/internal journal handling supported by EXT4;
- DAX paths where supported by the target kernel/device;
- normal buffered, direct and mapped I/O;
- lazy inode-table initialisation;
- sysfs control and reporting;
- block-validity checking.

Feature code is removed only when it is not part of production EXT4 functionality, such as KUnit source or old cross-filesystem compatibility routing.

## Important source boundaries

Large feature engines remain separate because they are genuine independent EXT4 responsibilities.

Examples include:

- `extents.c` and `extents_status.c`;
- `mballoc.c`;
- `fast_commit.c`;
- `inline.c`;
- `resize.c`;
- `mmp.c`;
- `orphan.c`;
- `fsmap.c`;
- `page-io.c` and `readpage.c`;
- `crypto.c`;
- `verity.c`;
- the six embedded JBD2 implementation units.

Tiny organisational units are folded into their owning subsystem:

- bitmap helpers -> `balloc.c`;
- directory hash helpers -> `dir.c`;
- fsync -> `file.c`;
- symlink inode operations -> `inode.c`;
- ACL and user/trusted/security/Hurd xattr handlers -> `xattr.c`;
- mbcache -> `xattr.c`;
- ACL and mbcache declarations -> `xattr.h`.

## JBD2

JBD2 is part of the EXT4 implementation in this project.

The tree retains the substantial transaction, commit, checkpoint, recovery, revoke and journal-core files, but Kbuild links those objects directly into:

```text
ext4.ko
```

There is no separately deployed `jbd2.ko` from Filesystem Support.

The embedded JBD2 module entry/exit surface is removed and its lifetime is driven from EXT4's single module lifecycle.

## Metadata cache

The metadata block cache used by EXT4 xattrs is merged into the xattr subsystem and linked directly into `ext4.ko`.

It is not a helper application and not a separate support module.

## Production-only shaping

The EXT4 production tree deliberately excludes:

- upstream Kconfig presentation text;
- KUnit configuration;
- inode KUnit test source;
- mballoc KUnit test source;
- EXT2 registration and compatibility-routing code;
- EXT3 registration and compatibility-routing code;
- compatibility-only EXT2/EXT3 support masks.

Those items do not implement an EXT4 filesystem feature.


## Source-rewrite policy

EXT4 production source must be an Infiltrator implementation of the EXT4
on-disk and VFS contracts.  Linux and other mature implementations may be used
to learn observable behaviour and edge cases, but their implementation source
must not be copied, transformed or regenerated into the active tree.

The previous pinned-Linux import/shaping path has been retired.  In particular,
there is no longer a supported workflow that copies `fs/ext4`, JBD2 or
mbcache into this repository and edits it into the one-module layout.

The current `linux/` directory still contains migration-era source from that
earlier approach.  Existing third-party provenance must remain attached to such
files until their implementation has actually been replaced.  Each converted
unit must instead be designed around this project's own module boundaries,
failure rules and tests.

The completion condition is behavioural rather than textual: the project must
retain valid EXT4 semantics and media compatibility while no active
implementation unit depends on copied external source expression.

The architectural target remains:

- exactly one `ext4.ko`;
- EXT4 registration only;
- JBD2 functionality owned inside the EXT4 module boundary;
- no separate project JBD2 or metadata-cache module;
- no EXT2/EXT3 compatibility-routing implementation.

## Development rule

Do not simplify EXT4 by deleting legitimate EXT4 features.

The objective is a filesystem-specific implementation, not a minimal implementation.

Refactoring is acceptable when it reduces duplication or improves correctness while retaining all valid EXT4 feature combinations and the single-`ext4.ko` deployment model.
