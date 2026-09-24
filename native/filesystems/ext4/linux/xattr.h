#ifndef INFILTRATR_EXT4_XATTR_H
#define INFILTRATR_EXT4_XATTR_H

#include <linux/posix_acl_xattr.h>
#include <linux/xattr.h>

#define EXT4_XATTR_MAGIC 0xEA020000
#define EXT4_XATTR_REFCOUNT_MAX 1024

#define EXT4_XATTR_INDEX_USER 1
#define EXT4_XATTR_INDEX_POSIX_ACL_ACCESS 2
#define EXT4_XATTR_INDEX_POSIX_ACL_DEFAULT 3
#define EXT4_XATTR_INDEX_TRUSTED 4
#define EXT4_XATTR_INDEX_LUSTRE 5
#define EXT4_XATTR_INDEX_SECURITY 6
#define EXT4_XATTR_INDEX_SYSTEM 7
#define EXT4_XATTR_INDEX_RICHACL 8
#define EXT4_XATTR_INDEX_ENCRYPTION 9
#define EXT4_XATTR_INDEX_HURD 10

struct ext4_xattr_header {
	__le32 h_magic;
	__le32 h_refcount;
	__le32 h_blocks;
	__le32 h_hash;
	__le32 h_checksum;
	__u32 h_reserved[3];
};

struct ext4_xattr_ibody_header {
	__le32 h_magic;
};

struct ext4_xattr_entry {
	__u8 e_name_len;
	__u8 e_name_index;
	__le16 e_value_offs;
	__le32 e_value_inum;
	__le32 e_value_size;
	__le32 e_hash;
	char e_name[];
};

#define EXT4_XATTR_PAD_BITS 2
#define EXT4_XATTR_PAD (1U << EXT4_XATTR_PAD_BITS)
#define EXT4_XATTR_ROUND (EXT4_XATTR_PAD - 1U)
#define EXT4_XATTR_LEN(name_length) 	(((name_length) + sizeof(struct ext4_xattr_entry) + 	  EXT4_XATTR_ROUND) & ~EXT4_XATTR_ROUND)
#define EXT4_XATTR_NEXT(entry) 	((struct ext4_xattr_entry *)((char *)(entry) + 	 EXT4_XATTR_LEN((entry)->e_name_len)))
#define EXT4_XATTR_SIZE(value_size) 	(((value_size) + EXT4_XATTR_ROUND) & ~EXT4_XATTR_ROUND)

#define IHDR(inode, raw_inode) 	((struct ext4_xattr_ibody_header *)((char *)(raw_inode) + 	 EXT4_GOOD_OLD_INODE_SIZE + EXT4_I(inode)->i_extra_isize))
#define ITAIL(inode, raw_inode) 	((void *)((char *)(raw_inode) + 	 EXT4_SB((inode)->i_sb)->s_inode_size))
#define IFIRST(header) 	((struct ext4_xattr_entry *)((header) + 1))

#define EXT4_XATTR_SIZE_MAX (1U << 24)
#define EXT4_XATTR_MIN_LARGE_EA_SIZE(block_size) 	((block_size) - EXT4_XATTR_LEN(3) - 	 sizeof(struct ext4_xattr_header) - 4)

#define BHDR(bh) 	((struct ext4_xattr_header *)(bh)->b_data)
#define ENTRY(ptr) 	((struct ext4_xattr_entry *)(ptr))
#define BFIRST(bh) ENTRY(BHDR(bh) + 1)
#define IS_LAST_ENTRY(entry) (*(__u32 *)(entry) == 0)

#define EXT4_ZERO_XATTR_VALUE ((void *)-1)

#define EXT4_INODE_HAS_XATTR_SPACE(inode) 	(EXT4_I(inode)->i_extra_isize != 0 && 	 EXT4_GOOD_OLD_INODE_SIZE + 	 EXT4_I(inode)->i_extra_isize + 	 sizeof(struct ext4_xattr_ibody_header) + 	 EXT4_XATTR_PAD <= EXT4_INODE_SIZE((inode)->i_sb))

struct ext4_xattr_info {
	const char *name;
	const void *value;
	size_t value_len;
	int name_index;
	int in_inode;
};

struct ext4_xattr_search {
	struct ext4_xattr_entry *first;
	void *base;
	void *end;
	struct ext4_xattr_entry *here;
	int not_found;
};

struct ext4_xattr_ibody_find {
	struct ext4_xattr_search s;
	struct ext4_iloc iloc;
};

struct ext4_xattr_inode_array {
	unsigned int count;
	struct inode *inodes[] __counted_by(count);
};

int infiltratr_mbcache_init(void);
void infiltratr_mbcache_exit(void);

extern const struct xattr_handler ext4_xattr_user_handler;
extern const struct xattr_handler ext4_xattr_trusted_handler;
extern const struct xattr_handler ext4_xattr_security_handler;
extern const struct xattr_handler ext4_xattr_hurd_handler;
extern const struct xattr_handler * const ext4_xattr_handlers[];

#define EXT4_XATTR_NAME_ENCRYPTION_CONTEXT "c"

static inline void ext4_write_lock_xattr(
	struct inode *inode, int *saved_no_expand)
{
	down_write(&EXT4_I(inode)->xattr_sem);
	*saved_no_expand =
		ext4_test_inode_state(inode, EXT4_STATE_NO_EXPAND);
	ext4_set_inode_state(inode, EXT4_STATE_NO_EXPAND);
}

static inline int ext4_write_trylock_xattr(
	struct inode *inode, int *saved_no_expand)
{
	if (!down_write_trylock(&EXT4_I(inode)->xattr_sem))
		return 0;

	*saved_no_expand =
		ext4_test_inode_state(inode, EXT4_STATE_NO_EXPAND);
	ext4_set_inode_state(inode, EXT4_STATE_NO_EXPAND);
	return 1;
}

static inline void ext4_write_unlock_xattr(
	struct inode *inode, int *saved_no_expand)
{
	if (!*saved_no_expand)
		ext4_clear_inode_state(inode, EXT4_STATE_NO_EXPAND);
	up_write(&EXT4_I(inode)->xattr_sem);
}

ssize_t ext4_listxattr(struct dentry *dentry, char *buffer, size_t size);
int ext4_xattr_get(
	struct inode *inode, int name_index, const char *name,
	void *buffer, size_t buffer_size);
int ext4_xattr_set(
	struct inode *inode, int name_index, const char *name,
	const void *value, size_t value_len, int flags);
int ext4_xattr_set_handle(
	handle_t *handle, struct inode *inode,
	int name_index, const char *name,
	const void *value, size_t value_len, int flags);
int ext4_xattr_set_credits(
	struct inode *inode, size_t value_len,
	bool is_create, int *credits);
int __ext4_xattr_set_credits(
	struct super_block *sb, struct inode *inode,
	struct buffer_head *block_bh,
	size_t value_len, bool is_create);

int ext4_xattr_delete_inode(
	handle_t *handle, struct inode *inode,
	struct ext4_xattr_inode_array **array,
	int extra_credits);
void ext4_xattr_inode_array_free(
	struct ext4_xattr_inode_array *array);

int ext4_expand_extra_isize_ea(
	struct inode *inode, int new_extra_isize,
	struct ext4_inode *raw_inode, handle_t *handle);
void ext4_evict_ea_inode(struct inode *inode);

int ext4_xattr_ibody_find(
	struct inode *inode, struct ext4_xattr_info *info,
	struct ext4_xattr_ibody_find *find);
int ext4_xattr_ibody_get(
	struct inode *inode, int name_index,
	const char *name, void *buffer,
	size_t buffer_size);
int ext4_xattr_ibody_set(
	handle_t *handle, struct inode *inode,
	struct ext4_xattr_info *info,
	struct ext4_xattr_ibody_find *find);

struct mb_cache *ext4_xattr_create_cache(void);
void ext4_xattr_destroy_cache(struct mb_cache *cache);

int __xattr_check_inode(
	struct inode *inode,
	struct ext4_xattr_ibody_header *header,
	void *end, const char *function,
	unsigned int line);
#define xattr_check_inode(inode, header, end) 	__xattr_check_inode( 		(inode), (header), (end), __func__, __LINE__)

#ifdef CONFIG_EXT4_FS_SECURITY
int ext4_init_security(
	handle_t *handle, struct inode *inode,
	struct inode *dir, const struct qstr *qstr);
#else
static inline int ext4_init_security(
	handle_t *handle, struct inode *inode,
	struct inode *dir, const struct qstr *qstr)
{
	return 0;
}
#endif

#ifdef CONFIG_LOCKDEP
void ext4_xattr_inode_set_class(struct inode *ea_inode);
#else
static inline void ext4_xattr_inode_set_class(struct inode *ea_inode)
{
}
#endif

int ext4_get_inode_usage(struct inode *inode, qsize_t *usage);

#define EXT4_ACL_VERSION 0x0001

typedef struct {
	__le16 e_tag;
	__le16 e_perm;
	__le32 e_id;
} ext4_acl_entry;

typedef struct {
	__le16 e_tag;
	__le16 e_perm;
} ext4_acl_entry_short;

typedef struct {
	__le32 a_version;
} ext4_acl_header;

static inline size_t ext4_acl_size(int count)
{
	if (count <= 4)
		return sizeof(ext4_acl_header) +
		       count * sizeof(ext4_acl_entry_short);

	return sizeof(ext4_acl_header) +
	       4 * sizeof(ext4_acl_entry_short) +
	       (count - 4) * sizeof(ext4_acl_entry);
}

static inline int ext4_acl_count(size_t size)
{
	ssize_t remaining;

	if (size < sizeof(ext4_acl_header))
		return -1;

	size -= sizeof(ext4_acl_header);
	remaining = size -
		4 * sizeof(ext4_acl_entry_short);
	if (remaining < 0) {
		if (size % sizeof(ext4_acl_entry_short))
			return -1;
		return size / sizeof(ext4_acl_entry_short);
	}
	if (remaining % sizeof(ext4_acl_entry))
		return -1;
	return remaining / sizeof(ext4_acl_entry) + 4;
}

#ifdef CONFIG_EXT4_FS_POSIX_ACL
struct posix_acl *ext4_get_acl(
	struct inode *inode, int type, bool rcu);
int ext4_set_acl(
	struct mnt_idmap *idmap, struct dentry *dentry,
	struct posix_acl *acl, int type);
int ext4_init_acl(
	handle_t *handle, struct inode *inode,
	struct inode *dir);
#else
#define ext4_get_acl NULL
#define ext4_set_acl NULL
static inline int ext4_init_acl(
	handle_t *handle, struct inode *inode,
	struct inode *dir)
{
	return 0;
}
#endif

#ifndef INFILTRATR_EXT4_MBCACHE_H
#define INFILTRATR_EXT4_MBCACHE_H

#include <linux/atomic.h>
#include <linux/fs.h>
#include <linux/hash.h>
#include <linux/list.h>
#include <linux/list_bl.h>

struct mb_cache;

enum ext4_mbcache_entry_flag {
	MBE_REFERENCED_B = 0,
	MBE_REUSABLE_B,
};

struct mb_cache_entry {
	struct list_head e_list;
	struct hlist_bl_node e_hash_list;
	atomic_t e_refcnt;
	u32 e_key;
	unsigned long e_flags;
	u64 e_value;
};

struct mb_cache *mb_cache_create(int bucket_bits);
void mb_cache_destroy(struct mb_cache *cache);
int mb_cache_entry_create(
	struct mb_cache *cache, gfp_t mask,
	u32 key, u64 value, bool reusable);
void __mb_cache_entry_free(
	struct mb_cache *cache,
	struct mb_cache_entry *entry);
void mb_cache_entry_wait_unused(
	struct mb_cache_entry *entry);

static inline void mb_cache_entry_put(
	struct mb_cache *cache,
	struct mb_cache_entry *entry)
{
	const unsigned int refs =
		atomic_dec_return(&entry->e_refcnt);

	if (refs) {
		if (refs <= 2)
			wake_up_var(&entry->e_refcnt);
		return;
	}
	__mb_cache_entry_free(cache, entry);
}

struct mb_cache_entry *mb_cache_entry_delete_or_get(
	struct mb_cache *cache, u32 key, u64 value);
struct mb_cache_entry *mb_cache_entry_get(
	struct mb_cache *cache, u32 key, u64 value);
struct mb_cache_entry *mb_cache_entry_find_first(
	struct mb_cache *cache, u32 key);
struct mb_cache_entry *mb_cache_entry_find_next(
	struct mb_cache *cache,
	struct mb_cache_entry *entry);
void mb_cache_entry_touch(
	struct mb_cache *cache,
	struct mb_cache_entry *entry);

#endif
#endif
