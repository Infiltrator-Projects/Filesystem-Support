# OverlayFS Design and Ownership

## Classification

OverlayFS is a stacked/union filesystem that composes existing lower and upper
directory trees. It does not define a standalone disk format, but it does define
filesystem semantics such as layer lookup, whiteouts, opaque directories,
copy-up, redirects, metacopy, index/origin identity and merged-directory
behaviour.

Filesystem Support therefore treats OverlayFS as a filesystem implementation,
not as a package/provider alias. Its eventual project implementation must have
one canonical host-neutral overlay engine with thin host adapters where the
host contract requires them.

## Current implementation state

OverlayFS is currently **reference/import state**.

There is no project-authored OverlayFS canonical engine or Filesystem Support
OverlayFS module claimed by this directory. The source under
`reference/linux/` is the pinned upstream Linux OverlayFS implementation copied
by `.github/workflows/import-linux-filesystems.yml`.

Those files retain their upstream SPDX identifiers, copyrights, filenames and
implementation boundaries. They are semantic and interoperability evidence,
not Filesystem Support-authored production source.

The former top-level `kernel/` staging directory was misleading because it made
unchanged upstream Linux source look like a project-native implementation. The
reference tree is therefore kept behind the explicit provenance boundary.

## Current directory layout

```text
native/filesystems/overlayfs/
  DESIGN.md
  reference/
    linux/               pinned upstream Linux OverlayFS source, reference only
```

When an independent implementation begins, project-owned source is added beside
that reference evidence:

```text
native/filesystems/overlayfs/
  DESIGN.md
  core/                  canonical overlay/union filesystem semantics
  linux/                 thin Linux VFS adapter if required
  windows/               thin Windows adapter if a supported mapping is viable
  userspace/             optional userspace presentation adapter
  reference/linux/       preserved upstream Linux evidence
```

There must not be a second copy of copy-up, lookup, whiteout or layer-selection
semantics in each platform adapter.

## Canonical ownership

A future `core/` owns behaviour defined by the overlay filesystem contract,
including:

- lower/upper/work layer model and validated configuration;
- merged namespace lookup and directory enumeration rules;
- whiteout and opaque-directory interpretation;
- copy-up and metadata-only copy-up semantics;
- redirect and origin/index identity rules;
- hard-link and rename behaviour across layers;
- xattr names/values used as OverlayFS metadata;
- coherency/invalidation rules that are filesystem semantics rather than host
  cache objects;
- corruption, contradiction and unsupported-state rejection.

Linux dentries/inodes, mount API, page cache, credentials, workqueues and VFS
lifetime rules belong in the Linux adapter. Equivalent host objects belong only
in their own adapters.

## File-boundary rule

Upstream files such as `copy_up.c`, `namei.c`, `inode.c`, `dir.c`,
`readdir.c`, `super.c` and `util.c` remain unchanged beneath
`reference/linux/`.

Project-authored source must be cut around Filesystem Support responsibilities
and host-neutral overlay semantics. Moving, renaming or recommenting upstream
files is never evidence that their implementation has been rewritten.

## Promotion sequence

1. define the supported overlay feature/configuration contract;
2. implement host-neutral layer, lookup, whiteout and metadata validation;
3. qualify namespace merging against independently constructed layer trees;
4. implement read-only merged namespace access through the canonical engine;
5. add copy-up and mutation only with explicit atomicity/failure contracts and
   crash/interruption tests;
6. add thin host adapters over the same engine;
7. preserve upstream reference provenance until corresponding implementation
   bodies have been independently replaced and qualified.

## Safety and failure behaviour

Contradictory layer topology, unsafe recursive overlays, malformed overlay
metadata, invalid redirect/origin/index state and unsupported feature
combinations must fail closed. Mutation must never copy up or expose an object
until the destination state and metadata needed for a coherent namespace are
ready to publish.

## Reference provenance

The refresh workflow currently pins Linux v6.12.107 and copies
`fs/overlayfs` into `reference/linux/`. Refreshing that evidence may replace
only the copied reference tree and must never overwrite future project-owned
`core/`, `linux/`, `windows/` or `userspace/` source.

## Completion rule

The current tree is reference evidence only. OverlayFS becomes a Filesystem
Support first-party implementation only when the canonical engine and relevant
adapter have independent implementation and qualification evidence.
