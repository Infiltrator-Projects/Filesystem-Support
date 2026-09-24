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
