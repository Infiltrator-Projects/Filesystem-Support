# Brittleness Reduction Contract

Filesystem implementations are independent deployment units. Shared orchestration may coordinate them, but it must not make one subsystem's failure undo an already-valid state owned by another subsystem.

## Transaction boundaries

A native kernel-module transaction owns only the kernel module for the selected filesystem: build prerequisites, compilation, signing, replacement, `depmod`, module loading/unloading, selection verification and rollback of that module when the kernel transaction itself fails.

Desktop identification is a separate follow-up transaction. GNOME Disks/Nemo/udev integration may report failure after a kernel transaction commits, but it must not remove, restore, unload or otherwise roll back the committed kernel module.

Per-filesystem desktop transactions own only the selected filesystem's active identification plugin and udev rule. OFS failure must not mutate FFS/SFS/SFS2/PFS3 state, and the same rule applies in every direction.

Shared launchers are package-lifecycle infrastructure. They are not owned by any individual filesystem transaction.

## Data ownership boundaries

Cross-platform filesystem identity and Linux support policy are separate authorities. `filesystem_identities()` owns stable on-media identity only: ID, name, family, description and note. It has no Linux package, module, provider or deployment-state dependency.

`linux_support_policies()` owns Linux-only access policy: packages, modules, provider class and access mode. The Linux `catalog()` is a materialized join of these two contracts plus project-native deployment policy. Windows, EFI and setup consumers therefore cannot inherit Debian policy simply by consuming filesystem identity.

Native kernel deployment membership and desktop integration membership are also separate. `data/native-filesystems.tsv` determines which project-owned Linux modules can be managed. `data/desktop-filesystems.tsv` determines which of those filesystems participate in desktop identification. Desktop policy may depend on a native filesystem existing; native deployment must not depend on desktop presentation metadata.

## Desktop action boundary

The GTK shell owns presentation state only: button sensitivity and labels, confirmation dialogs, error display and refresh requests. It consumes prepared `DesktopAction` values from `desktop_actions` and must not call package-removal planning, APT execution, native-module installation/removal or `modprobe` operations directly.

`desktop_actions` owns the translation from catalogue/probe state into an executable user action. It combines action policy with removal safety/interlocks and delegates actual privileged work to the installer backend behind an opaque executor. This keeps GTK lifetime and presentation concerns independent from Debian and kernel mutation mechanics.

The installer backend remains the privileged-operation boundary. Moving an operation behind `DesktopAction` does not transfer command validation, package allowlisting, native-module validation or privilege escalation into the presentation layer.

## Coupling rules

- Filesystem semantics remain inside each canonical filesystem engine.
- Linux module lifetime remains independent for each filesystem module.
- Presentation/integration failure cannot roll back valid kernel state.
- Package-level shared infrastructure cannot be removed while any dependent filesystem integration is active.
- Stable filesystem identity must not depend on Linux provider or package policy.
- Native module deployment must not depend on desktop-integration membership.
- The top-level native build graph must not parse or validate desktop-integration fields.
- The GTK shell must not import or invoke installer/package/module primitives directly.
- Cross-filesystem lists must have one explicit authority; CI must consume that authority rather than duplicate handwritten lists.
- A coordinator may sequence independent transactions, but it must preserve the ownership and rollback boundary of each transaction.

## Failure reporting

Partial success is reported explicitly. If a native module commits and desktop integration subsequently fails, the operation reports that the module state remains committed and requires only the desktop/integration layer to be repaired. It must never present that situation as if the kernel transaction had been reverted.
