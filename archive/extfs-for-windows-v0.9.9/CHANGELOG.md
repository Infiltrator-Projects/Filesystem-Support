# Changelog

This file records user-visible, compatibility, architecture and validation changes for Ext Filesystem Driver.

## Unreleased

- Canonical documentation baseline aligned with the Infiltrator project family.

## 0.9.9 — 2026-09-23

- Advance the exact `third_party/infiltratr-common` pin from Common 1.19.2 to released Common 1.19.24.
- Preserve the kernel dependency boundary: ExtFS still consumes only `include/infiltratr/compiler.h`, whose annotation macros are functionally unchanged across this upgrade.
- Update build validation and maintained dependency documentation to require the exact released Common version without changing filesystem-driver behaviour.

## Historical source

Git tags and GitHub Releases remain authoritative for exact historical source and release assets. Existing specialist release notes remain valid where present.

## Policy

Record behaviour changes, fixes, compatibility changes and support-boundary changes. Do not invent historical detail that cannot be tied to a release or commit.
