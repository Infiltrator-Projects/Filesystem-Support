# VirtioFS Design and Ownership

## Classification

VirtioFS exposes a host directory tree to a virtual machine through the FUSE
request model transported over virtio, with optional DAX shared-memory access.
It is a virtualisation/device-backed filesystem client rather than an on-disk
filesystem format.

Filesystem Support therefore separates portable request/session/namespace
semantics from host-specific virtio, DAX, cache and VFS integration.

## Current implementation state

VirtioFS is currently **reference/import state**.

There is no project-authored canonical VirtioFS engine and no Filesystem
Support VirtioFS native driver claimed by this directory. The source under
`reference/linux/` is the pinned upstream Linux `fs/fuse` tree, including
VirtioFS support, retained as protocol, integration and failure-behaviour
evidence.

Those files retain their upstream provenance, licences, filenames and source
boundaries. The former top-level `kernel/` staging directory was misleading
because it made the imported Linux FUSE/VirtioFS implementation appear to be
project-owned production source.

## Current directory layout

```text
native/filesystems/virtiofs/
  DESIGN.md
  reference/
    linux/               pinned upstream Linux FUSE/VirtioFS source, reference only
```

When an independent implementation begins:

```text
native/filesystems/virtiofs/
  DESIGN.md
  core/                  portable VirtioFS/FUSE request, identity and namespace state
  linux/                 thin Linux virtio/DAX/VFS adapter if required
  windows/               Windows virtualisation/filesystem adapter if justified
  userspace/             shared service/provider boundary where appropriate
  reference/linux/       preserved upstream evidence
```

## Canonical ownership

A future portable core may own:

- FUSE/VirtioFS request and response encoding/validation used by the supported
  client contract;
- node/file-handle identity and lifetime rules;
- namespace and metadata operation sequencing;
- capability/feature negotiation;
- cache/coherency semantics defined by the protocol;
- retry/cancellation and error-state rules that are transport-independent;
- invalid-message and hostile-host validation.

Linux virtqueues, DAX mappings, VFS/page-cache objects, kernel credentials,
workqueues and module lifetime are Linux-adapter concerns.

## File-boundary rule

The imported Linux files such as `virtio_fs.c`, `dev.c`, `inode.c`,
`dir.c`, `file.c`, `dax.c` and `fuse_i.h` remain unchanged under
`reference/linux/`.

Project-owned source must be cut around portable protocol/session/namespace and
host-adapter responsibilities rather than by mechanically renaming the Linux
FUSE translation units.

## Promotion sequence

1. define the exact VirtioFS/FUSE protocol features and security boundary;
2. implement bounded portable message and capability handling;
3. implement node/file-handle/session state against deterministic host fixtures;
4. qualify read-only namespace/data operations and hostile/malformed responses;
5. add mutation, cache/coherency and interruption handling with explicit tests;
6. add thin platform/virtio adapters over the same canonical engine;
7. qualify DAX only as an optional adapter capability, never as a separate
   filesystem semantic implementation.

## Safety and failure behaviour

The host/service is a trust boundary. Malformed lengths, identifiers, file
handles, capabilities and DAX ranges must be rejected. Interrupted or ambiguous
remote mutations must not be reported as durable success, and cache coherency
must never be silently stronger than the negotiated protocol contract.

## Reference provenance

The Linux-reference refresh copies `fs/fuse` into
`reference/linux/` because Linux VirtioFS is integrated with that shared FUSE
client implementation. Refreshes may replace only that reference evidence and
must never overwrite future project-owned `core/`, `linux/`, `windows/`
or `userspace/` source.

## Completion rule

The current tree is reference evidence only. VirtioFS becomes a Filesystem
Support first-party implementation only when its canonical engine and
applicable platform/service adapter have independent implementation and
qualification evidence.
