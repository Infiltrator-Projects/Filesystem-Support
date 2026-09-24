#ifndef INFILTRATR_EXT4_EXTENT_STATUS_H
#define INFILTRATR_EXT4_EXTENT_STATUS_H

#ifdef ES_DEBUG__
#define es_debug(fmt, ...) printk(fmt, ##__VA_ARGS__)
#else
#define es_debug(fmt, ...) no_printk(fmt, ##__VA_ARGS__)
#endif

#define ES_AGGRESSIVE_TEST__

enum ext4_extent_status_bit {
	ES_WRITTEN_B = 0,
	ES_UNWRITTEN_B,
	ES_DELAYED_B,
	ES_HOLE_B,
	ES_REFERENCED_B,
	ES_FLAGS,
};

#define ES_SHIFT (sizeof(ext4_fsblk_t) * 8 - ES_FLAGS)
#define ES_MASK (~((ext4_fsblk_t)0) << ES_SHIFT)

#define EXTENT_STATUS_WRITTEN (1U << ES_WRITTEN_B)
#define EXTENT_STATUS_UNWRITTEN (1U << ES_UNWRITTEN_B)
#define EXTENT_STATUS_DELAYED (1U << ES_DELAYED_B)
#define EXTENT_STATUS_HOLE (1U << ES_HOLE_B)
#define EXTENT_STATUS_REFERENCED (1U << ES_REFERENCED_B)

#define ES_TYPE_MASK 	((ext4_fsblk_t)(EXTENT_STATUS_WRITTEN | 			 EXTENT_STATUS_UNWRITTEN | 			 EXTENT_STATUS_DELAYED | 			 EXTENT_STATUS_HOLE))
#define ES_TYPE_VALID(type) ((type) && !((type) & ((type) - 1)))

struct ext4_sb_info;
struct ext4_extent;

struct extent_status {
	struct rb_node rb_node;
	ext4_lblk_t es_lblk;
	ext4_lblk_t es_len;
	ext4_fsblk_t es_pblk;
};

struct ext4_es_tree {
	struct rb_root root;
	struct extent_status *cache_es;
};

struct ext4_es_stats {
	unsigned long es_stats_shrunk;
	struct percpu_counter es_stats_cache_hits;
	struct percpu_counter es_stats_cache_misses;
	u64 es_stats_scan_time;
	u64 es_stats_max_scan_time;
	struct percpu_counter es_stats_all_cnt;
	struct percpu_counter es_stats_shk_cnt;
};

struct pending_reservation {
	struct rb_node rb_node;
	ext4_lblk_t lclu;
};

struct ext4_pending_tree {
	struct rb_root root;
};

int __init ext4_init_es(void);
void ext4_exit_es(void);
void ext4_es_init_tree(struct ext4_es_tree *tree);
void ext4_es_insert_extent(
	struct inode *inode, ext4_lblk_t lblk,
	ext4_lblk_t len, ext4_fsblk_t pblk,
	unsigned int status, int flags);
void ext4_es_cache_extent(
	struct inode *inode, ext4_lblk_t lblk,
	ext4_lblk_t len, ext4_fsblk_t pblk,
	unsigned int status);
void ext4_es_remove_extent(
	struct inode *inode, ext4_lblk_t lblk,
	ext4_lblk_t len);
void ext4_es_find_extent_range(
	struct inode *inode,
	int (*match_fn)(struct extent_status *),
	ext4_lblk_t lblk, ext4_lblk_t end,
	struct extent_status *result);
int ext4_es_lookup_extent(
	struct inode *inode, ext4_lblk_t lblk,
	ext4_lblk_t *next_lblk,
	struct extent_status *result);
bool ext4_es_scan_range(
	struct inode *inode,
	int (*matching_fn)(struct extent_status *),
	ext4_lblk_t lblk, ext4_lblk_t end);
bool ext4_es_scan_clu(
	struct inode *inode,
	int (*matching_fn)(struct extent_status *),
	ext4_lblk_t lblk);

static inline unsigned int ext4_es_status(const struct extent_status *es)
{
	return es->es_pblk >> ES_SHIFT;
}

static inline unsigned int ext4_es_type(const struct extent_status *es)
{
	return ext4_es_status(es) & ES_TYPE_MASK;
}

static inline bool ext4_es_is_written(const struct extent_status *es)
{
	return (ext4_es_type(es) & EXTENT_STATUS_WRITTEN) != 0;
}

static inline bool ext4_es_is_unwritten(const struct extent_status *es)
{
	return (ext4_es_type(es) & EXTENT_STATUS_UNWRITTEN) != 0;
}

static inline bool ext4_es_is_delayed(const struct extent_status *es)
{
	return (ext4_es_type(es) & EXTENT_STATUS_DELAYED) != 0;
}

static inline bool ext4_es_is_hole(const struct extent_status *es)
{
	return (ext4_es_type(es) & EXTENT_STATUS_HOLE) != 0;
}

static inline bool ext4_es_is_mapped(const struct extent_status *es)
{
	return ext4_es_is_written(es) || ext4_es_is_unwritten(es);
}

static inline void ext4_es_set_referenced(struct extent_status *es)
{
	es->es_pblk |=
		((ext4_fsblk_t)EXTENT_STATUS_REFERENCED) << ES_SHIFT;
}

static inline void ext4_es_clear_referenced(struct extent_status *es)
{
	es->es_pblk &=
		~(((ext4_fsblk_t)EXTENT_STATUS_REFERENCED) << ES_SHIFT);
}

static inline bool ext4_es_is_referenced(const struct extent_status *es)
{
	return (ext4_es_status(es) & EXTENT_STATUS_REFERENCED) != 0;
}

static inline ext4_fsblk_t ext4_es_pblock(const struct extent_status *es)
{
	return es->es_pblk & ~ES_MASK;
}

static inline ext4_fsblk_t
ext4_es_show_pblock(const struct extent_status *es)
{
	const ext4_fsblk_t pblock = ext4_es_pblock(es);
	return pblock == ~ES_MASK ? 0 : pblock;
}

static inline void ext4_es_store_pblock(
	struct extent_status *es, ext4_fsblk_t pblock)
{
	es->es_pblk =
		(pblock & ~ES_MASK) | (es->es_pblk & ES_MASK);
}

static inline void ext4_es_store_pblock_status(
	struct extent_status *es,
	ext4_fsblk_t pblock, unsigned int status)
{
	WARN_ON_ONCE(!ES_TYPE_VALID(status & ES_TYPE_MASK));
	es->es_pblk =
		(((ext4_fsblk_t)status << ES_SHIFT) & ES_MASK) |
		(pblock & ~ES_MASK);
}

int ext4_es_register_shrinker(struct ext4_sb_info *sbi);
void ext4_es_unregister_shrinker(struct ext4_sb_info *sbi);
int ext4_seq_es_shrinker_info_show(struct seq_file *seq, void *v);

int __init ext4_init_pending(void);
void ext4_exit_pending(void);
void ext4_init_pending_tree(struct ext4_pending_tree *tree);
void ext4_remove_pending(struct inode *inode, ext4_lblk_t lblk);
bool ext4_is_pending(struct inode *inode, ext4_lblk_t lblk);
void ext4_es_insert_delayed_extent(
	struct inode *inode, ext4_lblk_t lblk,
	ext4_lblk_t len, bool lclu_allocated,
	bool end_allocated);
void ext4_clear_inode_es(struct inode *inode);

#endif
