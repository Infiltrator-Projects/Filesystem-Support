#!/usr/bin/env python3
"""
Rebuild the engineering commentary for the EXT2, EXT3 and EXT4 native drivers.

The transformation is deliberately comment-only.  It removes inherited
engineering comments, preserves leading legal/SPDX notices, writes a new
project-level commentary layer, and proves that the C token stream did not
change.  Re-running the tool is deterministic.
"""

from __future__ import annotations

import argparse
import pathlib
import re
import sys
from dataclasses import dataclass

ROOTS = (
    pathlib.Path("native/filesystems/ext2/kernel"),
    pathlib.Path("native/filesystems/ext3/kernel"),
    pathlib.Path("native/filesystems/ext4/kernel"),
)

LEGAL_WORDS = (
    "SPDX-License-Identifier",
    "Copyright",
    "GNU General Public License",
    "All Rights Reserved",
    "GPL",
)

FS_DESIGN = {
    "ext2": (
        "a deliberately strict, non-journalled EXT2 VFS implementation",
        (
            "Do not accept a journalled EXT3 volume as EXT2.",
            "Keep on-disk compatibility fields when they are required to parse or reject media correctly.",
            "Keep xattr/ACL/cache code inside ext2.ko rather than creating helper modules.",
        ),
    ),
    "ext3": (
        "a standalone EXT3 VFS implementation with its historical JBD engine embedded in ext3.ko",
        (
            "EXT3 requires its journal semantics; it is not an EXT4 compatibility registration.",
            "Preserve the journal, recovery, ordered/writeback/journal data modes and EXT3 on-disk limits.",
            "JBD and the metadata cache are private implementation code, not separately deployed modules.",
        ),
    ),
    "ext4": (
        "a full-featured EXT4 VFS implementation with JBD2 embedded in ext4.ko",
        (
            "Register and implement EXT4 only; do not route EXT2 or EXT3 mounts through this module.",
            "Preserve every valid EXT4 feature path supported by the pinned implementation.",
            "Treat journaling, extents, allocation, checksums, recovery and feature negotiation as correctness-critical state machines.",
        ),
    ),
}

FILE_DOCS = {
    "Makefile": (
        "Kernel build contract",
        "Defines the single composite filesystem module and the object-level feature composition used by Kbuild.",
        "The object list is part of the module boundary: it must not accidentally recreate a helper .ko or omit a filesystem feature engine.",
    ),
    "ext2.h": (
        "EXT2 shared model",
        "Defines the private on-disk layouts, in-memory state, feature masks and cross-subsystem interfaces used by EXT2.",
        "On-disk field order and widths are format contracts; declarations that mirror disk structures must not be reordered for cosmetic reasons.",
    ),
    "ext3.h": (
        "EXT3 shared model",
        "Defines EXT3 on-disk structures, in-memory filesystem/inode state, mount policy and private subsystem interfaces.",
        "The declarations encode journal-aware filesystem invariants and therefore form the contract between VFS-facing code and the embedded JBD engine.",
    ),
    "ext4.h": (
        "EXT4 shared model",
        "Defines EXT4 feature bits, on-disk structures, in-memory state and private interfaces shared across the full driver.",
        "Feature masks and disk-layout declarations are compatibility contracts; changes require explicit reasoning about old media and read-only versus read-write support.",
    ),
    "balloc.c": (
        "Block allocation",
        "Implements free-block accounting, block-group bitmap handling and block allocation/release policy.",
        "Allocation code must keep bitmap state, group descriptors, global counters and journal state mutually consistent across success and rollback paths.",
    ),
    "ialloc.c": (
        "Inode allocation",
        "Selects block groups for new inodes and maintains inode/directory allocation accounting.",
        "Group selection is policy; bitmap and counter updates are correctness state and must remain atomic with the filesystem's transaction model.",
    ),
    "dir.c": (
        "Directory representation",
        "Parses, validates and iterates directory records and implements directory lookup-side mechanics, including indexed-directory hashing where applicable.",
        "Directory record lengths, alignment and bounds are untrusted on-disk input and must be validated before pointer arithmetic or publication to VFS.",
    ),
    "file.c": (
        "Regular-file VFS operations",
        "Implements the regular-file interface, including open/read/write/mmap/direct-I/O related paths and any small file-facing operations consolidated into this unit.",
        "I/O ordering, size visibility, writeback and error propagation must agree with the filesystem's allocation and journaling rules.",
    ),
    "inode.c": (
        "Inode mapping and lifecycle",
        "Implements inode read/write conversion, logical-to-physical block mapping, truncation, address-space operations and inode lifecycle transitions.",
        "The in-memory inode and its durable representation may advance at different times; crash-safe ordering and orphan handling must preserve a recoverable disk state.",
    ),
    "namei.c": (
        "Namespace mutation",
        "Implements create/link/unlink/rename/mkdir/rmdir/mknod and related pathname-facing VFS operations.",
        "Namespace updates may touch several inodes and directory blocks; partial failure must leave a valid namespace and a recoverable transaction.",
    ),
    "super.c": (
        "Mount, superblock and module lifecycle",
        "Owns filesystem registration, mount/reconfigure/unmount, superblock validation, feature negotiation, global counters and module startup/teardown.",
        "The mount path is a trust boundary: validate feature flags and geometry before derived arithmetic, allocation or VFS publication.",
    ),
    "xattr.c": (
        "Extended metadata",
        "Implements extended attributes, ACL/security metadata and the private metadata-block cache used by the owning filesystem.",
        "Shared xattr blocks require exact reference/accounting rules; cache state is advisory, while on-disk reference counts and transaction ordering are authoritative.",
    ),
    "xattr.h": (
        "Extended-metadata interfaces",
        "Defines the private xattr structures, indexes and interfaces shared by the EXT4 extended-metadata implementation.",
        "Encoded entry lengths, offsets and namespaces describe persistent metadata and must remain compatible with existing media.",
    ),
    "journal.h": (
        "Embedded JBD contract",
        "Defines the transaction, journal, buffer and recovery interfaces used by the standalone EXT3 implementation.",
        "Transaction state transitions and buffer ownership rules form a crash-consistency protocol; callers must not bypass the documented handle lifecycle.",
    ),
    "jbd_checkpoint.c": (
        "Journal checkpointing",
        "Retires committed transaction buffers from the journal once their home-location writes are safe.",
        "Checkpoint progress must never discard the only durable copy of metadata needed for crash recovery.",
    ),
    "jbd_commit.c": (
        "Journal commit engine",
        "Drives a transaction through ordered data handling, descriptor/log writes, commit record publication and post-commit state transition.",
        "The commit record is an ordering boundary: metadata cannot be considered durably committed before the required log writes and barriers complete.",
    ),
    "jbd_journal.c": (
        "Journal core",
        "Owns journal creation/loading/destruction, commit-thread coordination, log-space management and the embedded JBD module-private caches.",
        "Journal sequence numbers, head/tail positions and transaction ownership must remain coherent across wraparound, abort and recovery.",
    ),
    "jbd_recovery.c": (
        "Journal recovery",
        "Scans and replays a journal after an unclean shutdown, respecting transaction sequence and revoke information.",
        "Recovery consumes untrusted persistent log records; every length, sequence and block reference must be validated before replay.",
    ),
    "jbd_revoke.c": (
        "Journal revoke processing",
        "Tracks blocks that must not be replayed from older log records because a later transaction revoked their journalled contents.",
        "Revoke ordering is part of recovery correctness: losing or misordering a revoke can resurrect stale metadata after a crash.",
    ),
    "jbd_transaction.c": (
        "Journal transaction API",
        "Implements transaction handles, buffer access acquisition, dirtying, forget/revoke operations and transaction state transitions.",
        "Credit accounting bounds log consumption; handle nesting and buffer ownership must preserve forward progress and transaction isolation.",
    ),
    "ext4_jbd2.c": (
        "EXT4-to-JBD2 adapter",
        "Translates EXT4 metadata operations into JBD2 handle and buffer-access operations while centralising filesystem-specific error handling.",
        "Adapter failures must abort or propagate consistently so EXT4 never reports metadata durable when the journal rejected the operation.",
    ),
    "ext4_jbd2.h": (
        "EXT4 journaling contract",
        "Defines EXT4 journal credit calculations, handle helpers and private interfaces used to coordinate metadata updates with JBD2.",
        "Credit estimates are correctness bounds as well as performance estimates: under-accounting can deadlock or force unexpected transaction restarts.",
    ),
    "ext4_extents.h": (
        "Extent-tree representation",
        "Defines the persistent and in-memory structures used by EXT4 extent trees.",
        "Extent headers, indexes and leaves are on-disk data structures; depth, entry counts and logical/physical ranges require validation before traversal.",
    ),
    "extents.c": (
        "Extent-tree engine",
        "Implements lookup, insertion, splitting, removal, unwritten-extent conversion and tree maintenance for extent-mapped files.",
        "Tree edits must preserve ordering, non-overlap, depth and checksum/transaction invariants across every split and rollback path.",
    ),
    "extents_status.c": (
        "Extent-status cache",
        "Maintains the in-memory logical-range cache used to accelerate extent state queries and delayed-allocation tracking.",
        "The cache may be discarded and rebuilt; it must never become more authoritative than persistent extent and allocation state.",
    ),
    "extents_status.h": (
        "Extent-status interfaces",
        "Defines extent-status states, range representation and cache operations shared by EXT4 mapping/allocation paths.",
        "Status transitions describe logical range state and must agree with delayed allocation, unwritten extents and durable mapping state.",
    ),
    "fast_commit.c": (
        "Fast-commit engine",
        "Implements EXT4 fast-commit tracking, compact log emission and replay for eligible metadata changes.",
        "Fast commit is an optimisation over full JBD2 commit: ineligibility or uncertainty must fall back to the full journal rather than weaken recovery.",
    ),
    "fast_commit.h": (
        "Fast-commit record format",
        "Defines private fast-commit state and record structures used by commit and replay.",
        "Persistent record formats require strict length/type validation because recovery treats them as untrusted disk input.",
    ),
    "block_validity.c": (
        "Metadata block validation",
        "Tracks ranges reserved for filesystem metadata and rejects mappings that would alias protected metadata blocks.",
        "This code is a corruption boundary: integer overflow or incomplete range checks can turn a malformed inode into metadata overwrite.",
    ),
    "crypto.c": (
        "Filesystem encryption integration",
        "Connects EXT4 inode and filename operations to the kernel fscrypt framework.",
        "The filesystem owns persistence and policy plumbing; cryptographic primitives and key management remain responsibilities of the generic kernel framework.",
    ),
    "fsmap.c": (
        "Physical-space mapping",
        "Implements filesystem mapping queries that report ownership and free-space extents to userspace.",
        "Reported ranges must reflect allocation metadata consistently and must not expose impossible or overlapping ownership.",
    ),
    "fsmap.h": (
        "Filesystem-map interfaces",
        "Defines internal fsmap query state and callbacks used while walking EXT4 physical allocation metadata.",
        "Range endpoints use filesystem block units and require overflow-safe conversion at user/kernel boundaries.",
    ),
    "indirect.c": (
        "Indirect-block mapping",
        "Implements the legacy direct/single/double/triple-indirect mapping format still valid for non-extent EXT4 inodes.",
        "Pointer chains originate on disk; every level must be bounds-checked and transactionally updated to avoid leaks or stale references.",
    ),
    "inline.c": (
        "Inline-data support",
        "Implements storage of small file or directory payloads inside the inode/xattr area and transitions between inline and block-backed forms.",
        "Conversion changes both inode layout and data placement; interruption must leave one authoritative representation recoverable by normal journal replay.",
    ),
    "ioctl.c": (
        "EXT4 control operations",
        "Implements filesystem-specific ioctl and file-attribute operations exposed through the VFS file interface.",
        "Ioctls are an ABI boundary: validate privileges, flags, ranges and structure versions before mutating persistent state.",
    ),
    "mballoc.c": (
        "Multiblock allocator",
        "Implements EXT4's extent-oriented free-space search, preallocation, locality grouping and buddy-based allocation policy.",
        "Buddy/cache structures are derived acceleration state; persistent bitmaps and allocation counters remain authoritative and must agree at transaction boundaries.",
    ),
    "mballoc.h": (
        "Multiblock-allocation interfaces",
        "Defines allocator state, preallocation records and helper contracts shared by EXT4 allocation paths.",
        "Allocator structures encode ownership/lifetime rules that interact with per-CPU, per-group and inode-local state.",
    ),
    "migrate.c": (
        "Mapping-format migration",
        "Converts eligible inodes from legacy indirect mapping to extent mapping.",
        "Migration changes persistent mapping representation and therefore requires rollback-safe ordering so either old or new mapping remains valid after failure.",
    ),
    "mmp.c": (
        "Multiple-mount protection",
        "Implements the on-disk heartbeat used to detect concurrent read-write mounts of the same EXT4 filesystem.",
        "Heartbeat timing and sequence updates are safety signals; stale or ambiguous state must be handled conservatively.",
    ),
    "move_extent.c": (
        "Extent relocation",
        "Implements controlled swapping/movement of mapped extents between files for online defragmentation-style operations.",
        "Both files' mappings, data validity and journal state must change as one logical operation to prevent cross-file data exposure.",
    ),
    "orphan.c": (
        "Orphan-file management",
        "Tracks inodes whose truncate/unlink completion must survive a crash, including the modern orphan-file mechanism.",
        "An inode may leave orphan tracking only after the durable state no longer needs recovery-time completion.",
    ),
    "page-io.c": (
        "Writeback page I/O",
        "Builds and submits block I/O for dirty EXT4 pages/folios while coordinating journal and unwritten-extent completion.",
        "Completion ordering may transition unwritten extents and inode size state; error paths must not publish blocks as initialised prematurely.",
    ),
    "readpage.c": (
        "Read-side mapped I/O",
        "Implements mapped read submission for EXT4 file data.",
        "Logical-to-physical mappings must be validated before I/O submission and holes/unwritten extents must retain zero-fill semantics.",
    ),
    "resize.c": (
        "Online resize",
        "Implements growth of EXT filesystems, including group descriptor, bitmap and reserved metadata updates.",
        "Geometry changes touch global addressing structures; validation must prevent overlaps, overflow and partially published groups.",
    ),
    "sysfs.c": (
        "Runtime control and observability",
        "Publishes EXT4 per-filesystem attributes and tunables through sysfs.",
        "Sysfs stores are privileged control paths and must validate values before changing live allocator or error-handling behaviour.",
    ),
    "truncate.h": (
        "Truncation helpers",
        "Provides compact helper contracts used while removing mappings beyond the new end of an inode.",
        "Truncation helpers participate in block-release ordering and therefore inherit the caller's transaction and locking requirements.",
    ),
    "verity.c": (
        "fs-verity integration",
        "Connects EXT4 metadata and file operations to the kernel fs-verity framework.",
        "EXT4 persists verity metadata while the generic framework owns Merkle-tree verification semantics and cryptographic policy.",
    ),
    "include/linux/jbd2.h": (
        "Embedded JBD2 contract",
        "Defines journal, transaction, handle and journal-head state used by the EXT4-embedded JBD2 engine.",
        "The state model is a crash-consistency protocol; field ownership and transaction-state ordering are stronger contracts than ordinary in-memory bookkeeping.",
    ),
    "include/trace/events/jbd2.h": (
        "JBD2 tracing schema",
        "Defines trace events used to observe journal transaction and checkpoint behaviour without changing filesystem semantics.",
        "Tracepoints are diagnostic only: correctness must never depend on listeners, tracing enablement or trace-buffer availability.",
    ),
    "jbd2_checkpoint.c": (
        "JBD2 checkpointing",
        "Moves committed metadata toward home locations and advances the recoverable journal tail.",
        "Journal space can be reclaimed only after recovery no longer needs the corresponding logged metadata.",
    ),
    "jbd2_commit.c": (
        "JBD2 commit engine",
        "Serialises transaction metadata/data ordering, log descriptor writes and commit-record publication.",
        "The commit record is the durability boundary for a transaction and must follow the required data, metadata and barrier ordering.",
    ),
    "jbd2_journal.c": (
        "JBD2 journal core",
        "Owns journal lifecycle, log-space accounting, commit-thread coordination and internal journal caches embedded in ext4.ko.",
        "Head/tail arithmetic, sequence numbers and abort state must remain coherent across wraparound, I/O failure and recovery.",
    ),
    "jbd2_recovery.c": (
        "JBD2 recovery",
        "Scans, validates and replays committed log records after an unclean shutdown.",
        "Recovery treats the journal as untrusted persistent input and must honour revoke records before replaying metadata.",
    ),
    "jbd2_revoke.c": (
        "JBD2 revoke processing",
        "Records blocks whose older logged images must not be replayed during recovery.",
        "A lost revoke can replay stale metadata; revoke tables therefore belong to the journal's correctness state, not merely its performance state.",
    ),
    "jbd2_transaction.c": (
        "JBD2 transaction API",
        "Implements handle credit accounting, transaction attachment and buffer state transitions for journalled metadata updates.",
        "Credit, nesting and buffer ownership rules prevent log overcommit and preserve atomic transaction boundaries.",
    ),
}

@dataclass(frozen=True)
class FileContext:
    fs: str
    path: pathlib.Path
    relative: str
    title: str
    purpose: str
    invariant: str


def filesystem_for(path: pathlib.Path) -> str:
    parts = path.parts
    return parts[2]


def file_context(path: pathlib.Path) -> FileContext:
    fs = filesystem_for(path)
    relative = str(path.relative_to(pathlib.Path("native/filesystems") / fs / "kernel"))
    title, purpose, invariant = FILE_DOCS.get(
        relative,
        FILE_DOCS.get(
            path.name,
            (
                "Filesystem implementation unit",
                f"Implements the {path.stem.replace('_', ' ')} portion of {fs.upper()}.",
                "Preserve the owning filesystem's VFS, on-disk, locking and transaction invariants across every success and failure path.",
            ),
        ),
    )
    return FileContext(fs, path, relative, title, purpose, invariant)


def is_legal(comment: str) -> bool:
    return any(word in comment for word in LEGAL_WORDS)


def extract_leading_legal(text: str) -> tuple[str, str]:
    """Preserve leading legal/SPDX comments; engineering prose is rebuilt."""
    i = 0
    kept: list[str] = []
    n = len(text)

    while True:
        ws_start = i
        while i < n and text[i] in " \t\r\n":
            i += 1
        prefix_ws = text[ws_start:i]

        if text.startswith("//", i):
            end = text.find("\n", i)
            if end < 0:
                end = n
            else:
                end += 1
            block = text[i:end]
            if is_legal(block):
                kept.append(prefix_ws + block)
                i = end
                continue
            i = ws_start
            break

        if text.startswith("/*", i):
            end = text.find("*/", i + 2)
            if end < 0:
                i = ws_start
                break
            end += 2
            if end < n and text[end] == "\n":
                end += 1
            block = text[i:end]
            if is_legal(block):
                kept.append(prefix_ws + block)
                i = end
                continue
            i = ws_start
            break

        i = ws_start
        break

    return "".join(kept).strip("\n"), text[i:]


def strip_c_comments(text: str) -> str:
    """Replace C/C++ comments with whitespace while preserving strings and line structure."""
    out: list[str] = []
    i = 0
    n = len(text)
    state = "normal"

    while i < n:
        ch = text[i]
        nxt = text[i + 1] if i + 1 < n else ""

        if state == "normal":
            if ch == '"' :
                out.append(ch)
                state = "string"
                i += 1
            elif ch == "'":
                out.append(ch)
                state = "char"
                i += 1
            elif ch == "/" and nxt == "/":
                out.extend((" ", " "))
                state = "line"
                i += 2
            elif ch == "/" and nxt == "*":
                out.extend((" ", " "))
                state = "block"
                i += 2
            else:
                out.append(ch)
                i += 1
        elif state == "string":
            out.append(ch)
            if ch == "\\" and i + 1 < n:
                out.append(text[i + 1])
                i += 2
            else:
                if ch == '"':
                    state = "normal"
                i += 1
        elif state == "char":
            out.append(ch)
            if ch == "\\" and i + 1 < n:
                out.append(text[i + 1])
                i += 2
            else:
                if ch == "'":
                    state = "normal"
                i += 1
        elif state == "line":
            if ch == "\n":
                out.append("\n")
                state = "normal"
            else:
                out.append(" ")
            i += 1
        else:  # block
            if ch == "*" and nxt == "/":
                out.extend((" ", " "))
                state = "normal"
                i += 2
            else:
                out.append("\n" if ch == "\n" else " ")
                i += 1

    return "".join(out)


TOKEN_RE = re.compile(
    r'''
    "(?:\\.|[^"\\])*"
  | '(?:\\.|[^'\\])*'
  | [A-Za-z_]\w*
  | 0[xX][0-9A-Fa-f]+(?:[uUlL]+)?
  | \d+(?:\.\d*)?(?:[eEpP][+-]?\d+)?(?:[uUlLfF]+)?
  | >>=|<<=|\+\+|--|->|&&|\|\||<=|>=|==|!=|\+=|-=|\*=|/=|%=|&=|\|=|\^=|<<|>>
  | \.\.\.
  | [^\s]
    ''',
    re.VERBOSE | re.DOTALL,
)


def code_tokens(text: str) -> list[str]:
    return TOKEN_RE.findall(strip_c_comments(text))


def humanise(name: str) -> str:
    name = re.sub(r"^_+", "", name)
    name = re.sub(r"^(?:ext2|ext3|ext4|jbd2|jbd)_", "", name)
    return name.replace("_", " ")


def function_role(name: str, ctx: FileContext) -> str:
    bare = re.sub(r"^_+", "", name)
    words = set(humanise(bare).split())

    if {"count", "calculate", "calc", "sum"} & words:
        return "Computes derived filesystem state used for validation, accounting or policy decisions."
    if "fill" in words and "super" in words:
        return "Constructs and validates the mounted filesystem state before it is published to VFS."
    if "mount" in words:
        return "Implements a mount-path operation for the owning filesystem."
    if {"init", "initialize", "setup"} & words or bare.startswith(("init_", "setup_")):
        return "Initialises subsystem state and establishes the resources required by later operations."
    if {"exit", "destroy", "teardown"} & words or bare.startswith(("exit_", "destroy_")):
        return "Tears down subsystem state after users have been quiesced."
    if {"validate", "verify", "check", "valid"} & words:
        return "Validates state before it is trusted by the remainder of the filesystem."
    if {"recover", "recovery", "replay"} & words:
        return "Participates in crash recovery and reconstruction of durable filesystem state."
    if {"commit", "checkpoint"} & words:
        return "Advances journalled state toward a durable transaction or checkpoint boundary."
    if {"sync", "fsync", "flush"} & words:
        return "Drives pending state toward the durability guarantee required by the calling VFS or journal interface."
    if {"lookup", "find", "search", "get", "read", "bread", "load"} & words:
        return "Retrieves or materialises filesystem state for validation or higher-level processing without changing ownership by default."
    if {"alloc", "allocate", "new"} & words:
        return "Allocates or reserves filesystem state while maintaining the owning allocator's accounting invariants."
    if {"free", "release", "discard", "put"} & words:
        return "Releases filesystem state and reconciles the corresponding accounting or ownership metadata."
    if {"journal", "transaction", "handle"} & words:
        return "Coordinates a journal transaction or journal-owned buffer/state transition."
    if {"xattr", "acl"} & words:
        return "Implements an extended-metadata operation in the filesystem's xattr/ACL subsystem."
    if {"extent", "extents"} & words:
        return "Operates on logical-to-physical extent state while preserving extent-tree ordering and range invariants."
    if {"inode"} & words:
        return "Implements an inode operation at the boundary between VFS state and the filesystem's persistent representation."
    if {"write", "update", "set", "mark", "dirty", "clear"} & words:
        return "Updates filesystem state under the ordering and persistence rules of the surrounding subsystem."
    if {"resize", "grow"} & words:
        return "Changes filesystem geometry while preserving address-space, allocation and recovery invariants."
    if {"ioctl"} & words:
        return "Handles a filesystem-specific control operation exposed through the file API."
    if {"rename", "unlink", "link", "mkdir", "rmdir", "create", "mknod"} & words:
        return "Performs a namespace mutation that must remain transactionally consistent across all affected directory and inode state."

    return f"Implements the {humanise(name)} operation within the {ctx.title.lower()} subsystem."


def function_comment(name: str, ctx: FileContext) -> str:
    return (
        f"/**\n"
        f" * {name} - {function_role(name, ctx)}\n"
        f" *\n"
        f" * Correctness contract: preserve the locking, lifetime, range and\n"
        f" * transaction preconditions established by the surrounding {ctx.fs.upper()}\n"
        f" * subsystem. Failure handling must follow that subsystem's established\n"
        f" * rollback, abort or retry policy.\n"
        f" */\n"
    )


def struct_comment(kind: str, name: str, ctx: FileContext) -> str:
    noun = "state/data structure" if kind == "struct" else "state/value set"
    return (
        f"/**\n"
        f" * {kind} {name} - Private {ctx.fs.upper()} {noun} used by {ctx.title.lower()}.\n"
        f" *\n"
        f" * Treat fields that mirror persistent media or cross subsystem boundaries\n"
        f" * as interface contracts rather than incidental layout.\n"
        f" */\n"
    )


def file_header(ctx: FileContext) -> str:
    design, rules = FS_DESIGN[ctx.fs]
    bullet_text = "\n".join(f" *   - {rule}" for rule in rules)
    return (
        f"/*\n"
        f" * {ctx.fs.upper()} — {ctx.title}\n"
        f" *\n"
        f" * Purpose:\n"
        f" *   {ctx.purpose}\n"
        f" *\n"
        f" * Filesystem model:\n"
        f" *   This file belongs to {design}.\n"
        f" *\n"
        f" * Correctness focus:\n"
        f" *   {ctx.invariant}\n"
        f" *\n"
        f" * Project rules:\n"
        f"{bullet_text}\n"
        f" *\n"
        f" * Commentary policy:\n"
        f" *   Comments explain invariants, ownership, persistence ordering and\n"
        f" *   non-obvious design intent. They deliberately avoid restating C syntax.\n"
        f" */\n\n"
    )


def preprocessor_ranges(text: str) -> list[tuple[int, int]]:
    """Return physical ranges occupied by preprocessor directives, including continuations."""
    ranges: list[tuple[int, int]] = []
    offset = 0
    active_start: int | None = None

    for line in text.splitlines(keepends=True):
        stripped = line.lstrip()
        if active_start is None and stripped.startswith("#"):
            active_start = offset

        if active_start is not None:
            logical = line.rstrip("\r\n").rstrip()
            if not logical.endswith("\\"):
                ranges.append((active_start, offset + len(line)))
                active_start = None

        offset += len(line)

    if active_start is not None:
        ranges.append((active_start, len(text)))
    return ranges


def preprocessor_fingerprint(text: str) -> list[tuple[str, ...]]:
    """Capture logical preprocessor directives so comment insertion cannot alter macro shape."""
    clean = strip_c_comments(text)
    logical = clean.replace("\\\n", "")
    result: list[tuple[str, ...]] = []
    for line in logical.splitlines():
        if line.lstrip().startswith("#"):
            result.append(tuple(TOKEN_RE.findall(line)))
    return result


def top_level_insertions(text: str, ctx: FileContext) -> list[tuple[int, str]]:
    """Find top-level function/type definitions in comment-free C."""
    insertions: dict[int, str] = {}
    pp_ranges = preprocessor_ranges(text)
    depth = 0
    state = "normal"
    i = 0
    n = len(text)
    stmt_start = 0

    def skip_space_back(pos: int) -> int:
        while pos >= 0 and text[pos].isspace():
            pos -= 1
        return pos

    def last_pp_end_before(pos: int) -> int:
        end = 0
        for start, stop in pp_ranges:
            if stop <= pos:
                end = max(end, stop)
            elif start < pos < stop:
                return stop
            else:
                break
        return end

    while i < n:
        ch = text[i]

        if state == "normal":
            if ch == '"':
                state = "string"
                i += 1
                continue
            if ch == "'":
                state = "char"
                i += 1
                continue

            if ch == "{":
                if depth == 0:
                    candidate_start = max(stmt_start, last_pp_end_before(i))
                    candidate = text[candidate_start:i]
                    stripped = candidate.strip()
                    insertion_pos = candidate_start + (len(candidate) - len(candidate.lstrip()))

                    # A generated block comment must never land inside a
                    # preprocessor directive or its backslash continuation.
                    if any(start <= insertion_pos < stop for start, stop in pp_ranges):
                        insertion_pos = max(
                            stop for start, stop in pp_ranges
                            if start <= insertion_pos < stop
                        )

                    # Function definition: identify the outermost final parameter
                    # list and the identifier immediately before it.
                    close = skip_space_back(i - 1)
                    while close >= stmt_start and text[close] != ")":
                        close -= 1
                    if close >= stmt_start and text[close] == ")":
                        pdepth = 1
                        open_pos = close - 1
                        while open_pos >= stmt_start:
                            if text[open_pos] == ")":
                                pdepth += 1
                            elif text[open_pos] == "(":
                                pdepth -= 1
                                if pdepth == 0:
                                    break
                            open_pos -= 1
                        if open_pos >= stmt_start:
                            before = text[candidate_start:open_pos]
                            # A top-level initialiser can contain calls before its
                            # opening brace; it is not a function definition.
                            if "=" not in before:
                                m = re.search(r"([A-Za-z_]\w*)\s*$", before)
                                if m and m.group(1) not in {
                                    "if", "for", "while", "switch", "return",
                                    "sizeof", "typeof", "defined",
                                }:
                                    insertions[insertion_pos] = function_comment(m.group(1), ctx)
                    else:
                        m = re.search(r"\b(struct|enum|union)\s+([A-Za-z_]\w*)\s*$", stripped)
                        if m:
                            insertions[insertion_pos] = struct_comment(m.group(1), m.group(2), ctx)
                depth += 1
                i += 1
                continue

            if ch == "}":
                depth = max(0, depth - 1)
                if depth == 0:
                    stmt_start = i + 1
                i += 1
                continue

            if depth == 0 and ch == ";":
                stmt_start = i + 1

            i += 1
            continue

        if state == "string":
            if ch == "\\" and i + 1 < n:
                i += 2
            else:
                if ch == '"':
                    state = "normal"
                i += 1
            continue

        if state == "char":
            if ch == "\\" and i + 1 < n:
                i += 2
            else:
                if ch == "'":
                    state = "normal"
                i += 1
            continue

    return sorted(insertions.items())


def normalise_blank_lines(text: str) -> str:
    text = re.sub(r"[ \t]+\n", "\n", text)
    text = re.sub(r"\n{4,}", "\n\n\n", text)
    return text.strip("\n") + "\n"


def recomment_c(path: pathlib.Path, original: str) -> str:
    ctx = file_context(path)
    legal, body = extract_leading_legal(original)
    comment_free = strip_c_comments(body)
    comment_free = normalise_blank_lines(comment_free)

    insertions = top_level_insertions(comment_free, ctx)
    pieces: list[str] = []
    cursor = 0
    for pos, comment in insertions:
        pieces.append(comment_free[cursor:pos])
        pieces.append(comment)
        cursor = pos
    pieces.append(comment_free[cursor:])
    rebuilt_body = "".join(pieces)

    prefix = (legal + "\n\n") if legal else ""
    rebuilt = prefix + file_header(ctx) + rebuilt_body
    rebuilt = normalise_blank_lines(rebuilt)

    if code_tokens(original) != code_tokens(rebuilt):
        raise RuntimeError(f"{path}: comment rewrite changed the C token stream")
    if preprocessor_fingerprint(original) != preprocessor_fingerprint(rebuilt):
        raise RuntimeError(f"{path}: comment rewrite changed preprocessor directive structure")
    return rebuilt


def recomment_makefile(path: pathlib.Path, original: str) -> str:
    ctx = file_context(path)
    kept = []
    body = []
    for line in original.splitlines():
        if line.lstrip().startswith("#"):
            if "SPDX-License-Identifier" in line:
                kept.append(line)
            continue
        body.append(line)

    design, rules = FS_DESIGN[ctx.fs]
    header = [
        *kept,
        f"# {ctx.fs.upper()} — {ctx.title}",
        "#",
        f"# {ctx.purpose}",
        f"# This build describes {design}.",
        f"# Correctness: {ctx.invariant}",
        "#",
        "# Project rules:",
        *[f"#   - {rule}" for rule in rules],
        "#",
    ]
    rebuilt = "\n".join(header + body).strip() + "\n"

    def make_semantics(text: str) -> list[str]:
        return [
            line.rstrip()
            for line in text.splitlines()
            if line.strip() and not line.lstrip().startswith("#")
        ]

    if make_semantics(original) != make_semantics(rebuilt):
        raise RuntimeError(f"{path}: comment rewrite changed Makefile semantics")
    return rebuilt


def transform_once(path: pathlib.Path, original: str) -> str:
    if path.name == "Makefile":
        return recomment_makefile(path, original)
    return recomment_c(path, original)


def canonical_commentary(path: pathlib.Path, original: str) -> str:
    """Iterate to a stable layout so --write and --check share one canonical form."""
    current = original
    for _ in range(6):
        rebuilt = transform_once(path, current)
        if rebuilt == current:
            return rebuilt
        current = rebuilt
    raise RuntimeError(f"{path}: commentary layout did not converge")


def iter_files() -> list[pathlib.Path]:
    files: list[pathlib.Path] = []
    for root in ROOTS:
        if not root.is_dir():
            raise RuntimeError(f"missing filesystem tree: {root}")
        for path in sorted(root.rglob("*")):
            if path.is_file() and (path.suffix in {".c", ".h"} or path.name == "Makefile"):
                files.append(path)
    return files


def main() -> int:
    parser = argparse.ArgumentParser()
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--write", action="store_true", help="rewrite comments in place")
    mode.add_argument("--check", action="store_true", help="verify the committed commentary is canonical")
    args = parser.parse_args()

    files = iter_files()
    if not files:
        raise RuntimeError("no EXT family source files found")

    changed = 0
    function_docs = 0
    type_docs = 0

    for path in files:
        original = path.read_text(encoding="utf-8")
        rebuilt = canonical_commentary(path, original)
        function_docs += len(re.findall(r"^ \* [A-Za-z_]\w* - ", rebuilt, re.MULTILINE))
        type_docs += len(re.findall(r"^ \* (?:struct|enum|union) [A-Za-z_]\w* - ", rebuilt, re.MULTILINE))
        if rebuilt != original:
            changed += 1
            if args.write:
                path.write_text(rebuilt, encoding="utf-8")
            else:
                print(f"commentary drift: {path}", file=sys.stderr)

    print(
        f"EXT commentary audit: files={len(files)} "
        f"changed={changed} function_docs={function_docs} type_docs={type_docs}"
    )

    if args.check and changed:
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
