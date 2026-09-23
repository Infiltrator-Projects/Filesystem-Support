# Decisions

This file records durable architectural decisions for Ext Filesystem Driver.

## ADR-001 — The filesystem core is portable; Windows is an adapter

**Decision.** ext2/ext3/ext4 parsing and supported mutation semantics live in a portable core independent of Windows kernel plumbing.

**Rationale.** On-disk correctness should be testable without requiring a loaded kernel driver, while Windows IRP/lifetime rules remain isolated at the platform boundary.

**Consequence.** The Windows driver translates native filesystem operations into portable core contracts rather than embedding a second filesystem implementation.

## ADR-002 — Unsupported write layouts fail closed

**Decision.** A layout is writable only when the implementation understands all metadata and recovery consequences required by that operation.

**Rationale.** Best-effort mutation of unfamiliar ext features can corrupt filesystems.

**Consequence.** Read support may legitimately be broader than write support, and unfamiliar write-sensitive features return an explicit unsupported result.

## ADR-003 — Durability barriers are part of the filesystem contract

**Decision.** Metadata-changing operations require the ordering/durability barriers defined by the supported ext2/ext3/ext4 mutation path.

**Rationale.** Returning success before the required dirty/mutation/clean ordering is durable creates crash states the project cannot safely claim to support.

**Consequence.** Hosts that cannot provide the necessary barrier may be refused before mutation.

## ADR-004 — Windows lifecycle correctness is not portable-core correctness

**Decision.** FCB lifetime, Memory Manager interaction, removable-media verification and IRP semantics are validated as Windows-specific responsibilities.

**Rationale.** A correct ext parser can still be an unsafe Windows filesystem driver if object lifetime or caching contracts are wrong.

**Consequence.** WDK/static analysis and real Windows testing remain independent evidence layers.

## ADR-005 — Secure Boot production loading is a release requirement

**Decision.** A production driver must load on stock supported Windows with Secure Boot enabled; disabling Secure Boot or TESTSIGNING is not the intended production path.

**Rationale.** Native filesystem software should integrate with the normal Windows trust model.

**Consequence.** Development/test signatures are explicitly distinguished from production Microsoft signing.