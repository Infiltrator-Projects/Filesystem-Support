# AFUSE Design and Ownership

## Classification

AFUSE is a userspace/FUSE automounter, not an on-disk filesystem format.

The Filesystem Support catalogue currently classifies it as:

- access mode: userspace;
- support provider: userspace;
- Debian package: `afuse`;
- purpose: dynamically invoke filesystem clients on demand beneath a FUSE
  automount namespace.

That classification is the correct architectural boundary. AFUSE must not be
turned into a kernel module merely to make its directory resemble local disk
filesystems.

## Current implementation state

Filesystem Support does not currently contain a first-party AFUSE
implementation. The product discovers and manages the external Debian
`afuse` provider through the catalogue/action architecture.

The previous `.gitkeep` represented no implementation and no useful ownership
boundary. It has been removed. This `DESIGN.md` is now the authoritative
statement of why the directory intentionally contains no native filesystem
engine.

## Target layout

If a first-party AFUSE-equivalent capability is ever admitted, its code belongs
at the userspace boundary:

```text
native/filesystems/afuse/
  DESIGN.md
  userspace/            AFUSE-specific automount policy/provider glue, if needed
```

Protocol-neutral mount orchestration, process supervision, request validation,
credential handling and service lifetime should be promoted to the shared
Filesystem Support userspace-service infrastructure when their contracts are
not AFUSE-specific.

There is deliberately no planned `kernel/` directory and no reason to create a
disk-format `core/` unless the product later defines genuinely portable
automounter semantics that warrant a canonical engine.

## Ownership rules

AFUSE-specific behaviour, if implemented in the future, may include:

- automount namespace and trigger semantics;
- mapping from a requested path to a configured filesystem client;
- bounded configuration grammar and argument construction;
- child-client lifecycle and failure propagation;
- timeout/unmount policy that is genuinely part of the automounter contract.

The following do not belong in AFUSE-specific code when they can be shared:

- generic subprocess/process supervision;
- generic escaping and argument validation;
- generic mount-state observation;
- generic user/session service lifetime;
- GUI, APT and PolicyKit mechanics.

## Safety

An automounter can become a command-construction boundary. Any future
first-party implementation must therefore treat configured client names,
arguments and requested paths as untrusted data. It must use structured
arguments rather than shell command strings and must fail closed when a
requested client or option is outside its reviewed policy.

## Completion rule

The current catalogue integration is complete only as an external-provider
entry; it is not a first-party filesystem implementation.

A future project-owned AFUSE replacement would require an explicit roadmap
decision, a userspace service design, deterministic tests for path/configuration
handling and lifecycle behaviour, and a deliberate transition away from the
external provider. Until then, the absence of native source is intentional.
