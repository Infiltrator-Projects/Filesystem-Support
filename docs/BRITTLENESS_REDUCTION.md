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

Release publication is a promotion transaction owned by the exact `build` workflow run that produced the package. A release commit must not start a second build, search for a matching artifact in another workflow run, or depend on `main` remaining at the release SHA while qualification finishes.

The `desktop-integration` build job produces the Debian package once and uploads it as the run's qualified package artifact. The release publisher consumes that same-run artifact directly. There is no release-specific CMake build and no substitute package may be manufactured after qualification.

EXT and Amiga runtime qualifications are independent reusable units. On a release commit the build workflow calls both units with the immutable `github.sha`; GitHub's job dependency graph, not workflow names, polling loops or timing, determines when publication may proceed. Neither filesystem family owns release sequencing and neither family may trigger publication merely because its qualification finished last.

The release publisher depends explicitly on the ordinary build jobs plus both runtime qualification calls. Tagging and GitHub Release creation therefore occur only after every required prerequisite for that exact SHA succeeds. Publication must remain idempotent: an existing tag or release is accepted only when it points to the same SHA and contains the same qualified assets.

Push-triggered build runs are keyed by commit SHA and are not cancelled by later pushes to `main`; a pushed release SHA remains valid provenance even when development continues. Pull-request runs may cancel stale revisions by PR identity because they are not publication candidates.

Central APT publication is the only remaining asynchronous workflow boundary because it crosses into the separate central repository. It starts only after the complete `build` workflow succeeds, verifies that the GitHub Release targets the same SHA once, and then requests/observes publication by the external repository. Internal GitHub Release publication must never be polled as a sibling workflow.

Release state therefore moves in one direction: exact-SHA build and package artifact -> independent exact-SHA runtime qualifications in the same dependency graph -> GitHub Release promotion -> central APT publication. Later stages may verify upstream identity but must not rediscover, rebuild or silently substitute upstream state.

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
- A release is promoted from the exact package artifact produced by its own build run; publication must not rebuild it.
- Runtime qualification belongs to each filesystem family; release sequencing belongs to the build dependency graph.
- Release qualification uses explicit reusable-workflow calls by SHA, never workflow-name discovery, polling or sibling dispatch.
- Push-triggered release evidence is SHA-bound and survives later main pushes; stale PR evidence may be cancelled by PR identity.
- APT publication follows successful completion of the build/release DAG and is the only external asynchronous promotion boundary.

## Failure reporting

Partial success is reported explicitly. If a native module commits and desktop integration subsequently fails, the operation reports that the module state remains committed and requires only the desktop/integration layer to be repaired. It must never present that situation as if the kernel transaction had been reverted.

Likewise, rollback must not claim that the previous module state was restored unless both the persistent module image and any previously active runtime selection have been verified. Probe failures must report inability to verify state instead of presenting a guessed support classification.
