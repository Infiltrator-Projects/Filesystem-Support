# Brittleness Reduction Contract

Filesystem implementations are independent deployment units. Shared orchestration may coordinate them, but it must not make one subsystem's failure undo an already-valid state owned by another subsystem.

## Transaction boundaries

A native kernel-module transaction owns only the kernel module for the selected filesystem: build prerequisites, compilation, signing, replacement, `depmod`, module loading/unloading, selection verification and rollback of that module when the kernel transaction itself fails.

A failed native-module replacement must restore both persistent and runtime state. Restoring the previous `.ko` on disk is insufficient when the previous module was loaded: rollback must reload it and verify that the kernel again selects the same module image that was active before the transaction.

Desktop identification is a separate follow-up transaction. GNOME Disks/Nemo/udev integration may report failure after a kernel transaction commits, but it must not remove, restore, unload or otherwise roll back the committed kernel module.

Per-filesystem desktop transactions own only the selected filesystem's active identification plugin and udev rule. OFS failure must not mutate FFS/SFS/SFS2/PFS3 state, and the same rule applies in every direction.

Shared launchers are package-lifecycle infrastructure. They are not owned by any individual filesystem transaction.

## Data ownership boundaries

Cross-platform filesystem identity and Linux support policy are separate authorities. `filesystem_identities()` owns stable on-media identity only: ID, name, family, description and note. It has no Linux package, module, provider or deployment-state dependency.

`linux_support_policies()` owns Linux-only access policy: packages, modules, provider class and access mode. The Linux `catalog()` is a materialized join of these two contracts plus project-native deployment policy. Windows, EFI and setup consumers therefore cannot inherit Debian policy simply by consuming filesystem identity.

Native kernel deployment membership and desktop integration membership are also separate. `data/native-filesystems.tsv` owns exactly two fields: filesystem ID and native Linux module. `data/desktop-filesystems.tsv` independently determines which of those filesystems participate in desktop identification. Desktop policy may depend on a native filesystem existing; native deployment must not depend on desktop presentation metadata.

## Desktop action boundary

The GTK shell owns presentation state only: button sensitivity and labels, confirmation dialogs, error display and refresh requests. It consumes prepared `DesktopAction` values from `desktop_actions` and must not call package-removal planning, APT execution, native-module installation/removal or `modprobe` operations directly.

`desktop_actions` owns the translation from catalogue/probe state into an explicit command value. A prepared action contains an inspectable `DesktopActionKind` plus validated package/module/filesystem payload; it must not hide executable closures or captured backend state. This keeps the action model deterministic, auditable and independently testable while the installer backend remains the only owner of privileged execution.

The installer backend remains the privileged-operation boundary. Moving an operation behind `DesktopAction` does not transfer command validation, package allowlisting, native-module validation or privilege escalation into the presentation layer.

Package removal confirmation is bound to the exact simulated package set shown to the user. The installer must simulate removal again immediately before execution and compare the resulting package multiset with the approved plan. If package state changes the plan, execution fails closed and requires a new confirmation; being merely catalogue-managed is not sufficient authority to remove a different package set.

## Probe truthfulness

A support scan distinguishes verified absence from inability to inspect the system. Failure to read required Debian package inventory, repository inventory, kernel filesystem registration, module dependency metadata, or native module selection must never be converted into a normal `missing` or `unavailable` result.

A result whose required inventory could not be verified is explicitly unreliable, is presented as incomplete, and exposes no package or module action until a later successful refresh. Partial cached inventory must never become executable policy.

## Privilege boundary

Privileged command identity is policy, not environment. `pkexec`, APT and module-management executables are resolved from fixed trusted system locations, and project helpers may be elevated only from the application-owned installed helper locations. The unprivileged process `PATH` must never decide which executable receives elevated privileges.

Arguments remain structured argv values rather than shell command text. Package and module allowlists remain deny-by-default even after the executable itself has been selected from a trusted path.

## Release promotion boundary

Release publication is a promotion transaction, not a build or filesystem-family transaction. The ordinary `build` workflow owns production of the qualified Debian package for an exact commit SHA. Release publication must consume that artifact directly and must not rebuild an alternative package from the same source.

EXT and Amiga runtime qualifications are independent evidence for the same immutable release SHA. Neither filesystem family owns release sequencing, may publish the product, or may serve as the trigger solely because its qualification happened to finish last. Every release commit requires both qualifications to succeed for that SHA.

A pushed commit is immutable release provenance even if `main` advances while its qualification is still running. Push-triggered build and runtime qualification must therefore be keyed by commit SHA and must not be cancelled by a later main push. Pull-request qualification may cancel stale revisions by PR identity because those revisions are not publication candidates.

GitHub Release publication owns tagging and publication of the already-built artifact. Central APT publication is downstream of a successful GitHub Release and must not race a sibling publication workflow or poll for an artifact another sibling may not yet have published.

Release state must flow in one direction: exact-SHA build, independent exact-SHA qualification, GitHub promotion, then central APT publication. Later stages may verify upstream identity but must not rediscover, rebuild, or silently substitute upstream state.

## Coupling rules

- Filesystem semantics remain inside each canonical filesystem engine.
- Linux module lifetime remains independent for each filesystem module.
- Presentation/integration failure cannot roll back valid kernel state.
- Package-level shared infrastructure cannot be removed while any dependent filesystem integration is active.
- Stable filesystem identity must not depend on Linux provider or package policy.
- Native module deployment must not depend on desktop-integration membership.
- The native deployment manifest contains only filesystem/module ownership; desktop fields are forbidden.
- The top-level native build graph must not parse or validate desktop-integration fields.
- The GTK shell must not import or invoke installer/package/module primitives directly.
- Prepared desktop actions must be explicit data, not opaque executable callbacks.
- Confirmed destructive plans must still match immediately before execution.
- Failed inventory probes must disable actions rather than synthesize absence.
- Privileged executable selection must not depend on the caller's `PATH`.
- Native replacement rollback must verify restoration of previous runtime selection when that module was previously loaded.
- Cross-filesystem lists must have one explicit authority; CI must consume that authority rather than duplicate handwritten lists.
- A coordinator may sequence independent transactions, but it must preserve the ownership and rollback boundary of each transaction.
- A release is promoted from the exact successful build artifact; publication must not rebuild it.
- Runtime qualification belongs to each filesystem family; release sequencing belongs to neither family.
- Push-triggered release evidence is SHA-bound and survives later main pushes; stale PR evidence may be cancelled by PR identity.
- APT publication follows successful GitHub Release publication rather than racing it.

## Failure reporting

Partial success is reported explicitly. If a native module commits and desktop integration subsequently fails, the operation reports that the module state remains committed and requires only the desktop/integration layer to be repaired. It must never present that situation as if the kernel transaction had been reverted.

Likewise, rollback must not claim that the previous module state was restored unless both the persistent module image and any previously active runtime selection have been verified. Probe failures must report inability to verify state instead of presenting a guessed support classification.
