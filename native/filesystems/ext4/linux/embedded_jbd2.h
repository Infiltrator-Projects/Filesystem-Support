// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INFILTRATR_EXT4_EMBEDDED_JBD2_H
#define INFILTRATR_EXT4_EMBEDDED_JBD2_H

/*
 * Private lifecycle surface for JBD2 embedded inside ext4.ko.
 *
 * JBD2 is not deployed as a second module in Filesystem Support; EXT4 owns
 * startup and teardown of the embedded journal engine.
 */
int infiltratr_jbd2_init(void);
void infiltratr_jbd2_exit(void);

#endif
