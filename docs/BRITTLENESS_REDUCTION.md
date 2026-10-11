# Brittleness Reduction Contract

Filesystem implementations are independent deployment units. Shared orchestration may coordinate them, but it must not make one subsystem's failure undo an already-valid state owned by another subsystem.

## Transaction boundaries

A native kernel-module transaction owns only the kernel module for the selected filesystem: build prerequisites, compilation, signing, replacement, `depmod`, module loading/unloading, selection verification and rollback of that module when the kernel transaction itself fails.

Desktop identification is a separate follow-up transaction. GNOME Disks/Nemo/udev integration may report failure after a kernel transaction commits, but it must not remove, restore, unload or otherwise roll back the committed kernel module.

Per-filesystem desktop transactions own only the selected filesystem's active identification plugin and udev rule. OFS failure must not mutate FFS/SFS/SFS2/PFS3 state, and the same rule applies in every direction.

Shared launchers are package-lifecycle infrastructure. They are not owned by any individual filesystem transaction.

## Coupling rules

- Filesystem semantics remain inside each canonical filesystem engine.
- Linux module lifetime remains independent for each filesystem module.
- Presentation/integration failure cannot roll back valid kernel state.
- Package-level shared infrastructure cannot be removed while any dependent filesystem integration is active.
- Cross-filesystem lists must have one explicit authority; CI must consume that authority rather than duplicate handwritten lists.
- A coordinator may sequence independent transactions, but it must preserve the ownership and rollback boundary of each transaction.

## Failure reporting

Partial success is reported explicitly. If a native module commits and desktop integration subsequently fails, the operation reports that the module state remains committed and requires only the desktop/integration layer to be repaired. It must never present that situation as if the kernel transaction had been reverted.
