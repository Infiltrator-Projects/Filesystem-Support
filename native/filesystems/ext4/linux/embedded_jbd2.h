/*
 * Copyright (C) 2026 Shannon Smith
 *
 * Infiltrator Filesystem Support EXT4 Linux adapter: embedded_jbd2.h.
 * Project-maintained implementation for the canonical EXT4 driver.
 */

#ifndef INFILTRATR_EXT4_EMBEDDED_JBD2_H
#define INFILTRATR_EXT4_EMBEDDED_JBD2_H

/*
 * EXT4 owns the embedded JBD2 lifecycle. The journal engine is linked into
 * ext4.ko and is never registered as an independently deployed filesystem
 * support module.
 */
int infiltratr_jbd2_init(void);
void infiltratr_jbd2_exit(void);

#endif
