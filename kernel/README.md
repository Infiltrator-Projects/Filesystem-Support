# Kernel module build layer

This directory is the Kbuild entry point for the native filesystem suite.

Drivers will be independent modules such as `infiltratr-romfs.ko`,
`infiltratr-iso9660.ko` and `infiltratr-fat.ko`.

During the early architecture stages, required native-core source is compiled
into each driver instead of introducing a private shared-module ABI. A shared
`infiltratr-fs-core.ko` is considered only after the interface has proven
stable across several real filesystem implementations.

The userspace Infiltratr Common library is never linked into kernel modules.
Kernel builds use Linux primitives behind the same narrow engine contracts.
