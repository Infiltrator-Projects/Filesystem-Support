
/*
 * EXT2 — canonical-core Linux build bridge
 *
 * Purpose:
 *   Implements the canonical portion of EXT2.
 *
 * Filesystem model:
 *   This file belongs to a deliberately strict, non-journalled EXT2 VFS implementation.
 *
 * Correctness focus:
 *   Preserve the owning filesystem's VFS, on-disk, locking and transaction invariants across every success and failure path.
 *
 * Project rules:
 *   - Do not accept a journalled EXT3 volume as EXT2.
 *   - Keep on-disk compatibility fields when they are required to parse or reject media correctly.
 *   - Keep xattr/ACL/cache code inside ext2.ko rather than creating helper modules.
 *
 * Commentary policy:
 *   Comments explain invariants, ownership, persistence ordering and
 *   non-obvious design intent. They deliberately avoid restating C syntax.
 */

#include "../core/ext2_core.c"
#include "../core/ext2_engine.c"
