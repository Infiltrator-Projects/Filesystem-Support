# EXT Family Commentary Standard

## Scope

This standard applies to the owned EXT2, EXT3 and EXT4 kernel implementations under:

```text
native/filesystems/ext2/kernel/
native/filesystems/ext3/kernel/
native/filesystems/ext4/kernel/
```

The commentary layer is rebuilt by `tools/recomment-ext-family.py` after source import and shaping.

## Objective

Comments are engineering documentation, not a paraphrase of the C syntax.

The expected level is postgraduate systems engineering: comments should make the implementation easier to reason about in terms of invariants, state transitions, ownership, concurrency, persistence ordering, recovery and on-disk compatibility.

A useful comment should answer at least one of these questions:

- What persistent or in-memory invariant is being protected?
- Who owns the state and when may ownership change?
- Which lock, transaction, lifetime or ordering context is assumed?
- What makes an error path recoverable?
- Which data comes from untrusted persistent media and therefore requires validation?
- Which state is authoritative and which state is merely a cache or optimisation?
- Where is the crash-consistency or durability boundary?
- Why is a compatibility field retained even when another filesystem is not implemented here?
- Why is a subsystem kept separate rather than mechanically merged?

Comments should not explain obvious assignments, loops or conditionals.

## File-level documentation

Every C/header implementation file begins with an architectural comment that states:

1. the subsystem responsibility;
2. the filesystem model;
3. the principal correctness concern;
4. project-specific ownership rules;
5. the commentary policy.

This gives a reader the local design context before reading implementation details.

## Function-level documentation

Every top-level function definition receives a documentation block.

The block describes the function's role in the subsystem and reiterates the correctness contract that applies to its locking, lifetime, range and transaction context.

Function comments are intentionally concise. The source itself remains the authority for exact parameter and return semantics; commentary is used for design intent and non-obvious constraints rather than duplicating the function signature.

## Type-level documentation

Top-level private structures, unions and enumerations receive documentation when they define subsystem state.

Fields that mirror persistent media, transaction state or cross-subsystem interfaces are treated as contracts. Their layout or ordering must not be changed merely for cosmetic refactoring.

## Filesystem-specific principles

### EXT2

EXT2 is a strict non-journalled filesystem implementation.

Comments must reinforce that:

- journalled EXT3 media is rejected rather than silently mounted as EXT2;
- historical on-disk fields may still be required to parse or reject media correctly;
- xattr, ACL and cache implementation code remains inside `ext2.ko`.

### EXT3

EXT3 is an independent journalled filesystem implementation based on the last standalone Linux EXT3 semantics.

Comments must reinforce that:

- EXT3 is not an alias or compatibility registration for EXT4;
- JBD is part of the implementation and is embedded in `ext3.ko`;
- journal recovery and all three data modes are filesystem correctness mechanisms;
- journal credits, transaction state and revoke semantics are crash-consistency contracts.

### EXT4

EXT4 is the complete modern implementation.

Comments must reinforce that:

- the driver registers EXT4 only;
- valid EXT4 feature combinations are retained;
- JBD2 is embedded implementation code rather than a separately deployed support module;
- extents, delayed allocation, multiblock allocation, checksums, fast commit, orphan handling, encryption/verity integration and recovery are independent correctness-sensitive subsystems;
- derived caches are never more authoritative than persistent allocation or mapping state.

## Legal and provenance text

Leading SPDX, copyright and licence/provenance notices are not treated as engineering commentary and are deliberately preserved.

All other inherited engineering commentary is removed before the project commentary layer is generated.

## Semantic safety

The commentary transformation is required to be code-neutral.

For every C/header file the tool compares the lexical C token stream before and after rewriting. It also fingerprints logical preprocessor directives so a generated block comment cannot accidentally change a macro continuation.

For Makefiles the non-comment build lines are compared directly.

If any executable/build semantics change, the commentary rewrite fails.

## CI enforcement

The normal build workflow runs:

```text
python3 tools/recomment-ext-family.py --check
```

A manually edited or stale commentary layer therefore fails CI until it is regenerated or intentionally incorporated into the canonical generator.

This is deliberate: comments are maintained as part of the architecture rather than allowed to drift independently from the source-shaping process.
