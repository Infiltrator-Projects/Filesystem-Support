# Native filesystem engines

Each directory below this point owns exactly one filesystem format or one
deliberately grouped format family.

A conventional local filesystem is split into a format engine and a Linux VFS
binding. The same format engine must run in userspace tests/fsinspect and in
the kernel module; the kernel layer must not duplicate the parser.

The format engine must not contain GTK, APT, PolicyKit, FUSE, subprocess or
Linux VFS policy.

New filesystems begin read-only and fail closed on unknown or contradictory
metadata. Write support is a separate maturity stage with its own recovery and
destructive-test evidence.

The first implementation target is `romfs/`.
