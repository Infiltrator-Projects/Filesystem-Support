# Design

## First-principles position

Ext Filesystem Driver starts with the behaviour the project must own. Standards, platform frameworks and mature implementations are evidence and mechanisms, not specifications to copy blindly or semantic dependencies that may redefine the product later.

## Goals

- provide native Windows ext access through the normal I/O stack
- keep portable filesystem rules independent of Windows driver plumbing
- fail closed on unsupported write layouts
- make Secure Boot compatible production signing an explicit release requirement

## Non-goals

The project does not claim complete ext4 feature parity, and development/test signing is not equivalent to a production-signed Secure Boot release.

## Dependency and language policy

Prefer first-party C/C++ for portable/native implementation where it fits the problem. Use platform-native language/frameworks at genuine platform boundaries. Dependencies are accepted when their documented contract is stronger than reimplementation, but project-owned behaviour stays explicit and testable.

## Failure semantics

Unknown, unavailable, unsupported and invalid are distinct states. The project prefers a clear refusal to guessed success. Mutating or destructive operations require stronger preconditions and post-verification than read-only operations.

## Decision quality

A design change should state the problem, alternatives, evidence, trade-offs and validation method. Newness alone is not a benefit. Proven mechanisms remain when they are the strongest justified choice.
