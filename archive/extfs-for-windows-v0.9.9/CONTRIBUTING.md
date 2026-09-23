# Contributing

## Engineering standard

Work on Ext Filesystem Driver begins with ownership: identify which layer owns the behaviour, what contract changes and how it will be validated.

## Required practice

1. Read README.md, docs/ARCHITECTURE.md and docs/DESIGN.md.
2. Reuse the appropriate first-party shared capability rather than creating a private duplicate.
3. Keep platform code at platform boundaries and domain semantics in their owner.
4. Add regression coverage for changed behaviour.
5. Update roadmap, validation and specialist documentation when support boundaries move.

## Verification

Run the project's normal build/test path and ensure relevant CI remains green. A skipped mandatory gate is not a pass. Manual evidence should be recorded honestly at the environment actually tested.

## Repository policy

main is the working branch. Published tags/releases are immutable identities. Keep changes reviewable as one coherent implementation/test/documentation unit.
