# 9P Design and Ownership

## Classification

9P is a high-level remote resource-sharing/filesystem protocol rather than a
local on-disk block filesystem. It therefore falls under the Filesystem Support
rule that remote/device protocols use the coherent userspace-service boundary
when that is the technically correct integration point.

The project must not create a private Linux VFS implementation merely because
Linux already contains V9FS. Linux V9FS is useful reference and interoperability
evidence, not the permanent Filesystem Support source architecture.

## Protocol family

The implementation must treat the negotiated protocol dialect as explicit
session state. The relevant family includes the base 9P2000 protocol and the
Unix/POSIX-oriented extensions commonly known as 9P2000.u and 9P2000.L.

The canonical engine must own protocol/filesystem semantics that are independent
of the host operating system, including:

- protocol-version negotiation and capability state;
- fid lifecycle and walk/open/create/remove/rename semantics;
- Qid identity/version/type handling;
- namespace and directory-entry interpretation;
- request/reply framing and bounded message validation;
- file metadata and attribute translation at the protocol-semantic level;
- lock, xattr and ACL semantics only where the negotiated dialect defines them;
- cache-coherency policy inputs that are protocol facts rather than host cache
  objects;
- corruption, malformed-message and range rejection.

Host-specific VFS objects, Linux netfs/fscache state and kernel credential
objects are not canonical 9P semantics.

## Target project layout

When the independent implementation begins, the permanent shape is:

```text
native/filesystems/9p/
  DESIGN.md
  core/                 canonical 9P protocol/filesystem semantics
  userspace/            9P provider glue for the shared userspace service
  reference/
    linux/               copied upstream Linux V9FS evidence only
```

Reusable socket/transport, event-loop, credential and mount-presentation
mechanisms belong in shared Filesystem Support platform/userspace
infrastructure when their contract is genuinely protocol-neutral.

A future platform-specific adapter may be added only when a host requires
9P-specific glue that cannot live in the shared userspace service. It must not
duplicate the canonical protocol engine.

## Current implementation state

9P is currently **reference/import state**.

There is no project-authored 9P canonical engine and no Filesystem Support 9P
provider claimed by this directory. The files under `reference/linux/` are the
pinned Linux V9FS reference source copied by
`.github/workflows/import-linux-filesystems.yml`.

Those reference files retain their upstream SPDX identifiers, copyrights,
filenames and implementation boundaries. They are intentionally not renamed
into Filesystem Support responsibility names because doing so would imply an
authorship or architecture transition that has not occurred.

The reference tree is not linked by the product CMake build and is not a
Filesystem Support native module.

## Promotion rule

9P may move from reference/import state only through an explicit rewrite:

1. document the protocol/dialect contract and unsupported states;
2. introduce a host-neutral `core/` API with bounded message parsing;
3. qualify it with independent protocol fixtures and malformed-input tests;
4. implement the userspace provider through the shared service boundary;
5. prove interoperability against independent 9P servers/clients;
6. preserve upstream reference provenance until the corresponding behaviour has
   been independently replaced;
7. only then classify project-owned implementation files as authored source.

Renaming or moving copied V9FS files is never evidence that their implementation
has been rewritten.

## Safety and failure behaviour

Unknown protocol versions, contradictory lengths, invalid fid state, malformed
directory data and unsupported negotiated features must fail closed. A remote
peer is untrusted input.

Credentials and host security identities are adapter inputs. The canonical
engine must not silently equate Linux uid/gid/kernel credential objects with a
portable security model.

## Current reference provenance

The refresh workflow currently pins the Linux reference source to Linux
v6.12.107 and copies `fs/9p` into `reference/linux/`. Refreshing that
reference may update evidence, but it must not overwrite future project-authored
`core/` or `userspace/` implementation.
