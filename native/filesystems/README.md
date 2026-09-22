# Native filesystem engines

Each directory below this point owns exactly one catalogue filesystem implementation.

EXT2, EXT3 and EXT4 are deliberately separate source trees. Shared ancestry is
not a reason to collapse them into one implementation; any duplicated code is
intentional until a later refactor can preserve independent module ownership.

A conventional local filesystem is split into a format engine and a Linux VFS
binding. The same format engine must run in userspace tests/fsinspect and in
the kernel module; the kernel layer must not duplicate the parser.

The format engine must not contain GTK, APT, PolicyKit, FUSE, subprocess or
Linux VFS policy.

New filesystems begin read-only and fail closed on unknown or contradictory
metadata. Write support is a separate maturity stage with its own recovery and
destructive-test evidence.

The first implementation target is `romfs/`.
