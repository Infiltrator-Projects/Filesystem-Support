# disorderfs Design and Ownership

## Classification

disorderfs is a userspace/FUSE testing overlay. It deliberately changes
filesystem metadata/order presentation so reproducible-build and other
software can be tested against nondeterministic filesystem behaviour.

It is not an on-disk filesystem format and it is not a storage-compatibility
provider.

The current catalogue delegates this capability to Debian's `disorderfs`
package.

## Current state

Filesystem Support contains no first-party disorderfs implementation. The
former `.gitkeep` represented no source and has been removed.

## Target architecture

If project-owned equivalent functionality is ever admitted, it belongs only at
the userspace testing/provider boundary:

```text
native/filesystems/disorderfs/
  DESIGN.md
  userspace/            deterministic perturbation/testing policy, if admitted
```

There is deliberately no `kernel/` and no disk-format `core/`.

## Ownership

A future implementation may own controlled perturbation of directory ordering,
inode/metadata presentation, timestamps or other explicitly configured
observables required by the testing contract.

It must not modify the backing filesystem's allocation, on-disk structures or
recovery semantics. Generic mount-service and path-validation mechanisms should
remain shared infrastructure.

## Safety

The tool must clearly distinguish synthetic presentation changes from actual
backing-store mutation. Test perturbation should be reproducible from explicit
configuration/seed where reproducibility of the test itself matters, and path
mapping must remain confined to the configured backing tree.

## Completion rule

The present entry is complete only as an external userspace testing-provider
catalogue contract. It is not a project-authored filesystem implementation.
