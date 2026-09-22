// SPDX-License-Identifier: GPL-2.0

/*
 * EXT4 — Extended-metadata interfaces
 *
 * Purpose:
 *   Defines the private xattr structures, indexes and interfaces shared by the EXT4 extended-metadata implementation.
 *
 * Filesystem model:
 *   This file belongs to a full-featured EXT4 VFS implementation with JBD2 embedded in ext4.ko.
 *
 * Correctness focus:
 *   Encoded entry lengths, offsets and namespaces describe persistent metadata and must remain compatible with existing media.
 *
 * Project rules:
 *   - Register and implement EXT4 only; do not route EXT2 or EXT3 mounts through this module.
 *   - Preserve every valid EXT4 feature path supported by the pinned implementation.
 *   - Treat journaling, extents, allocation, checksums, recovery and feature negotiation as correctness-critical state machines.
 *
 * Commentary policy:
 *   Comments explain invariants, ownership, persistence ordering and
 *   non-obvious design intent. They deliberately avoid restating C syntax.
 */

#include <linux/xattr.h>


#define EXT4_XATTR_MAGIC		0xEA020000


#define EXT4_XATTR_REFCOUNT_MAX		1024


#define EXT4_XATTR_INDEX_USER			1
#define EXT4_XATTR_INDEX_POSIX_ACL_ACCESS	2
#define EXT4_XATTR_INDEX_POSIX_ACL_DEFAULT	3
#define EXT4_XATTR_INDEX_TRUSTED		4
#define	EXT4_XATTR_INDEX_LUSTRE			5
#define EXT4_XATTR_INDEX_SECURITY	        6
#define EXT4_XATTR_INDEX_SYSTEM			7
#define EXT4_XATTR_INDEX_RICHACL		8
#define EXT4_XATTR_INDEX_ENCRYPTION		9
#define EXT4_XATTR_INDEX_HURD			10

/**
 * struct ext4_xattr_header - Private EXT4 state/data structure used by extended-metadata interfaces.
 *
 * Treat fields that mirror persistent media or cross subsystem boundaries
 * as interface contracts rather than incidental layout.
 */
struct ext4_xattr_header {
	__le32	h_magic;
	__le32	h_refcount;
	__le32	h_blocks;
	__le32	h_hash;
	__le32	h_checksum;
	__u32	h_reserved[3];
};

/**
 * struct ext4_xattr_ibody_header - Private EXT4 state/data structure used by extended-metadata interfaces.
 *
 * Treat fields that mirror persistent media or cross subsystem boundaries
 * as interface contracts rather than incidental layout.
 */
struct ext4_xattr_ibody_header {
	__le32	h_magic;
};

/**
 * struct ext4_xattr_entry - Private EXT4 state/data structure used by extended-metadata interfaces.
 *
 * Treat fields that mirror persistent media or cross subsystem boundaries
 * as interface contracts rather than incidental layout.
 */
struct ext4_xattr_entry {
	__u8	e_name_len;
	__u8	e_name_index;
	__le16	e_value_offs;
	__le32	e_value_inum;
	__le32	e_value_size;
	__le32	e_hash;
	char	e_name[];
};

#define EXT4_XATTR_PAD_BITS		2
#define EXT4_XATTR_PAD		(1<<EXT4_XATTR_PAD_BITS)
#define EXT4_XATTR_ROUND		(EXT4_XATTR_PAD-1)
#define EXT4_XATTR_LEN(name_len) \
	(((name_len) + EXT4_XATTR_ROUND + \
	sizeof(struct ext4_xattr_entry)) & ~EXT4_XATTR_ROUND)
#define EXT4_XATTR_NEXT(entry) \
	((struct ext4_xattr_entry *)( \
	 (char *)(entry) + EXT4_XATTR_LEN((entry)->e_name_len)))
#define EXT4_XATTR_SIZE(size) \
	(((size) + EXT4_XATTR_ROUND) & ~EXT4_XATTR_ROUND)

#define IHDR(inode, raw_inode) \
	((struct ext4_xattr_ibody_header *) \
		((void *)raw_inode + \
		EXT4_GOOD_OLD_INODE_SIZE + \
		EXT4_I(inode)->i_extra_isize))
#define ITAIL(inode, raw_inode) \
	((void *)(raw_inode) + \
	 EXT4_SB((inode)->i_sb)->s_inode_size)
#define IFIRST(hdr) ((struct ext4_xattr_entry *)((hdr)+1))


#define EXT4_XATTR_SIZE_MAX (1 << 24)


#define EXT4_XATTR_MIN_LARGE_EA_SIZE(b)					\
	((b) - EXT4_XATTR_LEN(3) - sizeof(struct ext4_xattr_header) - 4)

#define BHDR(bh) ((struct ext4_xattr_header *)((bh)->b_data))
#define ENTRY(ptr) ((struct ext4_xattr_entry *)(ptr))
#define BFIRST(bh) ENTRY(BHDR(bh)+1)
#define IS_LAST_ENTRY(entry) (*(__u32 *)(entry) == 0)

#define EXT4_ZERO_XATTR_VALUE ((void *)-1)


#define EXT4_INODE_HAS_XATTR_SPACE(inode)				\
	((EXT4_I(inode)->i_extra_isize != 0) &&				\
	 (EXT4_GOOD_OLD_INODE_SIZE + EXT4_I(inode)->i_extra_isize +	\
	  sizeof(struct ext4_xattr_ibody_header) + EXT4_XATTR_PAD <=	\
	  EXT4_INODE_SIZE((inode)->i_sb)))

struct ext4_xattr_info {
	const char *name;
	const void *value;
	size_t value_len;
	int name_index;
	int in_inode;
};

/**
 * struct ext4_xattr_search - Private EXT4 state/data structure used by extended-metadata interfaces.
 *
 * Treat fields that mirror persistent media or cross subsystem boundaries
 * as interface contracts rather than incidental layout.
 */
struct ext4_xattr_search {
	struct ext4_xattr_entry *first;
	void *base;
	void *end;
	struct ext4_xattr_entry *here;
	int not_found;
};

/**
 * struct ext4_xattr_ibody_find - Private EXT4 state/data structure used by extended-metadata interfaces.
 *
 * Treat fields that mirror persistent media or cross subsystem boundaries
 * as interface contracts rather than incidental layout.
 */
struct ext4_xattr_ibody_find {
	struct ext4_xattr_search s;
	struct ext4_iloc iloc;
};

/**
 * struct ext4_xattr_inode_array - Private EXT4 state/data structure used by extended-metadata interfaces.
 *
 * Treat fields that mirror persistent media or cross subsystem boundaries
 * as interface contracts rather than incidental layout.
 */
struct ext4_xattr_inode_array {
	unsigned int count;
	struct inode *inodes[] __counted_by(count);
};

extern const struct xattr_handler ext4_xattr_user_handler;
extern const struct xattr_handler ext4_xattr_trusted_handler;
extern const struct xattr_handler ext4_xattr_security_handler;
extern const struct xattr_handler ext4_xattr_hurd_handler;

#define EXT4_XATTR_NAME_ENCRYPTION_CONTEXT "c"


/**
 * ext4_write_lock_xattr - Implements an extended-metadata operation in the filesystem's xattr/ACL subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static inline void ext4_write_lock_xattr(struct inode *inode, int *save)
{
	down_write(&EXT4_I(inode)->xattr_sem);
	*save = ext4_test_inode_state(inode, EXT4_STATE_NO_EXPAND);
	ext4_set_inode_state(inode, EXT4_STATE_NO_EXPAND);
}

/**
 * ext4_write_trylock_xattr - Implements an extended-metadata operation in the filesystem's xattr/ACL subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static inline int ext4_write_trylock_xattr(struct inode *inode, int *save)
{
	if (down_write_trylock(&EXT4_I(inode)->xattr_sem) == 0)
		return 0;
	*save = ext4_test_inode_state(inode, EXT4_STATE_NO_EXPAND);
	ext4_set_inode_state(inode, EXT4_STATE_NO_EXPAND);
	return 1;
}

/**
 * ext4_write_unlock_xattr - Implements an extended-metadata operation in the filesystem's xattr/ACL subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static inline void ext4_write_unlock_xattr(struct inode *inode, int *save)
{
	if (*save == 0)
		ext4_clear_inode_state(inode, EXT4_STATE_NO_EXPAND);
	up_write(&EXT4_I(inode)->xattr_sem);
}

extern ssize_t ext4_listxattr(struct dentry *, char *, size_t);

extern int ext4_xattr_get(struct inode *, int, const char *, void *, size_t);
extern int ext4_xattr_set(struct inode *, int, const char *, const void *, size_t, int);
extern int ext4_xattr_set_handle(handle_t *, struct inode *, int, const char *, const void *, size_t, int);
extern int ext4_xattr_set_credits(struct inode *inode, size_t value_len,
				  bool is_create, int *credits);
extern int __ext4_xattr_set_credits(struct super_block *sb, struct inode *inode,
				struct buffer_head *block_bh, size_t value_len,
				bool is_create);

extern int ext4_xattr_delete_inode(handle_t *handle, struct inode *inode,
				   struct ext4_xattr_inode_array **array,
				   int extra_credits);
extern void ext4_xattr_inode_array_free(struct ext4_xattr_inode_array *array);

extern int ext4_expand_extra_isize_ea(struct inode *inode, int new_extra_isize,
			    struct ext4_inode *raw_inode, handle_t *handle);
extern void ext4_evict_ea_inode(struct inode *inode);

extern const struct xattr_handler * const ext4_xattr_handlers[];

extern int ext4_xattr_ibody_find(struct inode *inode, struct ext4_xattr_info *i,
				 struct ext4_xattr_ibody_find *is);
extern int ext4_xattr_ibody_get(struct inode *inode, int name_index,
				const char *name,
				void *buffer, size_t buffer_size);
extern int ext4_xattr_ibody_set(handle_t *handle, struct inode *inode,
				struct ext4_xattr_info *i,
				struct ext4_xattr_ibody_find *is);

extern struct mb_cache *ext4_xattr_create_cache(void);
extern void ext4_xattr_destroy_cache(struct mb_cache *);

extern int
__xattr_check_inode(struct inode *inode, struct ext4_xattr_ibody_header *header,
		    void *end, const char *function, unsigned int line);

#define xattr_check_inode(inode, header, end) \
	__xattr_check_inode((inode), (header), (end), __func__, __LINE__)

#ifdef CONFIG_EXT4_FS_SECURITY
extern int ext4_init_security(handle_t *handle, struct inode *inode,
			      struct inode *dir, const struct qstr *qstr);
#else
/**
 * ext4_init_security - Initialises subsystem state and establishes the resources required by later operations.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static inline int ext4_init_security(handle_t *handle, struct inode *inode,
				     struct inode *dir, const struct qstr *qstr)
{
	return 0;
}
#endif

#ifdef CONFIG_LOCKDEP
extern void ext4_xattr_inode_set_class(struct inode *ea_inode);
#else
/**
 * ext4_xattr_inode_set_class - Implements an extended-metadata operation in the filesystem's xattr/ACL subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static inline void ext4_xattr_inode_set_class(struct inode *ea_inode) { }
#endif

extern int ext4_get_inode_usage(struct inode *inode, qsize_t *usage);


#include <linux/posix_acl_xattr.h>

#define EXT4_ACL_VERSION	0x0001

typedef struct {
	__le16		e_tag;
	__le16		e_perm;
	__le32		e_id;
} ext4_acl_entry;

typedef struct {
	__le16		e_tag;
	__le16		e_perm;
} ext4_acl_entry_short;

typedef struct {
	__le32		a_version;
} ext4_acl_header;

/**
 * ext4_acl_size - Implements an extended-metadata operation in the filesystem's xattr/ACL subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static inline size_t ext4_acl_size(int count)
{
	if (count <= 4) {
		return sizeof(ext4_acl_header) +
		       count * sizeof(ext4_acl_entry_short);
	} else {
		return sizeof(ext4_acl_header) +
		       4 * sizeof(ext4_acl_entry_short) +
		       (count - 4) * sizeof(ext4_acl_entry);
	}
}

/**
 * ext4_acl_count - Implements an extended-metadata operation in the filesystem's xattr/ACL subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static inline int ext4_acl_count(size_t size)
{
	ssize_t s;
	size -= sizeof(ext4_acl_header);
	s = size - 4 * sizeof(ext4_acl_entry_short);
	if (s < 0) {
		if (size % sizeof(ext4_acl_entry_short))
			return -1;
		return size / sizeof(ext4_acl_entry_short);
	} else {
		if (s % sizeof(ext4_acl_entry))
			return -1;
		return s / sizeof(ext4_acl_entry) + 4;
	}
}

#ifdef CONFIG_EXT4_FS_POSIX_ACL


struct posix_acl *ext4_get_acl(struct inode *inode, int type, bool rcu);
int ext4_set_acl(struct mnt_idmap *idmap, struct dentry *dentry,
		 struct posix_acl *acl, int type);
extern int ext4_init_acl(handle_t *, struct inode *, struct inode *);

#else
#include <linux/sched.h>
#define ext4_get_acl NULL
#define ext4_set_acl NULL

/**
 * ext4_init_acl - Initialises subsystem state and establishes the resources required by later operations.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static inline int
ext4_init_acl(handle_t *handle, struct inode *inode, struct inode *dir)
{
	return 0;
}
#endif


#ifndef _LINUX_MBCACHE_H
#define _LINUX_MBCACHE_H

#include <linux/hash.h>
#include <linux/list_bl.h>
#include <linux/list.h>
#include <linux/atomic.h>
#include <linux/fs.h>

struct mb_cache;


enum {
	MBE_REFERENCED_B = 0,
	MBE_REUSABLE_B
};

/**
 * struct mb_cache_entry - Private EXT4 state/data structure used by extended-metadata interfaces.
 *
 * Treat fields that mirror persistent media or cross subsystem boundaries
 * as interface contracts rather than incidental layout.
 */
struct mb_cache_entry {

	struct list_head	e_list;


	struct hlist_bl_node	e_hash_list;


	atomic_t		e_refcnt;

	u32			e_key;
	unsigned long		e_flags;

	u64			e_value;
};

struct mb_cache *mb_cache_create(int bucket_bits);
void mb_cache_destroy(struct mb_cache *cache);

int mb_cache_entry_create(struct mb_cache *cache, gfp_t mask, u32 key,
			  u64 value, bool reusable);
void __mb_cache_entry_free(struct mb_cache *cache,
			   struct mb_cache_entry *entry);
void mb_cache_entry_wait_unused(struct mb_cache_entry *entry);
/**
 * mb_cache_entry_put - Implements the mb cache entry put operation within the extended-metadata interfaces subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static inline void mb_cache_entry_put(struct mb_cache *cache,
				      struct mb_cache_entry *entry)
{
	unsigned int cnt = atomic_dec_return(&entry->e_refcnt);

	if (cnt > 0) {
		if (cnt <= 2)
			wake_up_var(&entry->e_refcnt);
		return;
	}
	__mb_cache_entry_free(cache, entry);
}

struct mb_cache_entry *mb_cache_entry_delete_or_get(struct mb_cache *cache,
						    u32 key, u64 value);
struct mb_cache_entry *mb_cache_entry_get(struct mb_cache *cache, u32 key,
					  u64 value);
struct mb_cache_entry *mb_cache_entry_find_first(struct mb_cache *cache,
						 u32 key);
struct mb_cache_entry *mb_cache_entry_find_next(struct mb_cache *cache,
						struct mb_cache_entry *entry);
void mb_cache_entry_touch(struct mb_cache *cache,
			  struct mb_cache_entry *entry);

#endif
