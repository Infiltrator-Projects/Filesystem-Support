#include <linux/fs.h>
#include <linux/part_stat.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/time.h>

#include "ext4.h"
#include "ext4_jbd2.h"

enum ext4_attr_kind {
	EXT4_ATTR_NOOP = 0,
	EXT4_ATTR_DIRTY_BLOCKS,
	EXT4_ATTR_SESSION_WRITES,
	EXT4_ATTR_LIFETIME_WRITES,
	EXT4_ATTR_RESERVED_CLUSTERS,
	EXT4_ATTR_SRA_RETRY_LIMIT,
	EXT4_ATTR_INODE_READAHEAD,
	EXT4_ATTR_TRIGGER_ERROR,
	EXT4_ATTR_FIRST_ERROR_TIME,
	EXT4_ATTR_LAST_ERROR_TIME,
	EXT4_ATTR_CLUSTERS_IN_GROUP,
	EXT4_ATTR_MB_ORDER,
	EXT4_ATTR_FEATURE,
	EXT4_ATTR_INT,
	EXT4_ATTR_UINT,
	EXT4_ATTR_ULONG,
	EXT4_ATTR_U64,
	EXT4_ATTR_U8,
	EXT4_ATTR_STRING,
	EXT4_ATTR_ATOMIC,
	EXT4_ATTR_JOURNAL_TASK,
};

enum ext4_attr_source {
	EXT4_ATTR_EXPLICIT = 0,
	EXT4_ATTR_SBI,
	EXT4_ATTR_DISK_SUPER,
};

struct ext4_attr {
	struct attribute attr;
	u16 kind;
	u16 source;
	u16 size;
	union {
		int offset;
		void *ptr;
	} target;
};

static const char ext4_proc_path[] = "fs/ext4";
static struct proc_dir_entry *ext4_proc_root;
static struct kobject *ext4_root;
static struct kobject *ext4_features;

static void *ext4_attr_pointer(struct ext4_attr *attr,
			       struct ext4_sb_info *sbi)
{
	switch (attr->source) {
	case EXT4_ATTR_EXPLICIT:
		return attr->target.ptr;
	case EXT4_ATTR_SBI:
		return (char *)sbi + attr->target.offset;
	case EXT4_ATTR_DISK_SUPER:
		return (char *)sbi->s_es + attr->target.offset;
	default:
		return NULL;
	}
}

static ssize_t ext4_show_session_writes(struct ext4_sb_info *sbi, char *buf)
{
	struct super_block *sb = sbi->s_buddy_cache->i_sb;
	const unsigned long sectors =
		part_stat_read(sb->s_bdev, sectors[STAT_WRITE]);

	return sysfs_emit(
		buf, "%lu\n",
		(sectors - sbi->s_sectors_written_start) >> 1);
}

static ssize_t ext4_show_lifetime_writes(struct ext4_sb_info *sbi, char *buf)
{
	struct super_block *sb = sbi->s_buddy_cache->i_sb;
	const u64 current =
		(part_stat_read(sb->s_bdev, sectors[STAT_WRITE]) -
		 sbi->s_sectors_written_start) >> 1;

	return sysfs_emit(
		buf, "%llu\n",
		(unsigned long long)(sbi->s_kbytes_written + current));
}

static ssize_t ext4_store_inode_readahead(struct ext4_sb_info *sbi,
					  const char *buf, size_t count)
{
	unsigned long value;
	int err = kstrtoul(skip_spaces(buf), 0, &value);

	if (err)
		return err;
	if (value && (!is_power_of_2(value) || value > 0x40000000UL))
		return -EINVAL;

	sbi->s_inode_readahead_blks = value;
	return count;
}

static ssize_t ext4_store_reserved_clusters(struct ext4_sb_info *sbi,
					     const char *buf,
					     size_t count)
{
	unsigned long long value;
	const ext4_fsblk_t total_clusters =
		ext4_blocks_count(sbi->s_es) >> sbi->s_cluster_bits;
	int err = kstrtoull(skip_spaces(buf), 0, &value);

	if (err)
		return err;
	if (value >= total_clusters)
		return -EINVAL;

	atomic64_set(&sbi->s_resv_clusters, value);
	return count;
}

static ssize_t ext4_store_test_error(struct ext4_sb_info *sbi,
				     const char *buf, size_t count)
{
	size_t length = count;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;
	if (length && buf[length - 1] == '\n')
		length--;
	if (length)
		ext4_error(sbi->s_sb, "%.*s", (int)length, buf);
	return count;
}

static ssize_t ext4_show_journal_task(struct ext4_sb_info *sbi, char *buf)
{
	if (!sbi->s_journal)
		return sysfs_emit(buf, "<none>\n");
	return sysfs_emit(buf, "%d\n", task_pid_vnr(sbi->s_journal->j_task));
}

#define EXT4_ATTRIBUTE(name, mode, kind_value) \
static struct ext4_attr ext4_attr_##name = { \
	.attr = { .name = __stringify(name), .mode = mode }, \
	.kind = EXT4_ATTR_##kind_value, \
}

#define EXT4_ATTRIBUTE_OFFSET(name, mode, kind_value, source_value, type, member) \
static struct ext4_attr ext4_attr_##name = { \
	.attr = { .name = __stringify(name), .mode = mode }, \
	.kind = EXT4_ATTR_##kind_value, \
	.source = EXT4_ATTR_##source_value, \
	.target.offset = offsetof(struct type, member), \
}

#define EXT4_ATTRIBUTE_STRING(name, mode, width, source_value, type, member) \
static struct ext4_attr ext4_attr_##name = { \
	.attr = { .name = __stringify(name), .mode = mode }, \
	.kind = EXT4_ATTR_STRING, \
	.source = EXT4_ATTR_##source_value, \
	.size = width, \
	.target.offset = offsetof(struct type, member), \
}

#define EXT4_ATTRIBUTE_PTR(name, mode, kind_value, pointer) \
static struct ext4_attr ext4_attr_##name = { \
	.attr = { .name = __stringify(name), .mode = mode }, \
	.kind = EXT4_ATTR_##kind_value, \
	.source = EXT4_ATTR_EXPLICIT, \
	.target.ptr = pointer, \
}

#define EXT4_ATTR_ENTRY(name) (&ext4_attr_##name.attr)

EXT4_ATTRIBUTE(delayed_allocation_blocks, 0444, DIRTY_BLOCKS);
EXT4_ATTRIBUTE(session_write_kbytes, 0444, SESSION_WRITES);
EXT4_ATTRIBUTE(lifetime_write_kbytes, 0444, LIFETIME_WRITES);
EXT4_ATTRIBUTE(reserved_clusters, 0644, RESERVED_CLUSTERS);
EXT4_ATTRIBUTE(sra_exceeded_retry_limit, 0444, SRA_RETRY_LIMIT);

EXT4_ATTRIBUTE_OFFSET(inode_readahead_blks, 0644, INODE_READAHEAD,
		      SBI, ext4_sb_info, s_inode_readahead_blks);
EXT4_ATTRIBUTE_OFFSET(mb_group_prealloc, 0644, CLUSTERS_IN_GROUP,
		      SBI, ext4_sb_info, s_mb_group_prealloc);
EXT4_ATTRIBUTE_OFFSET(mb_best_avail_max_trim_order, 0644, MB_ORDER,
		      SBI, ext4_sb_info, s_mb_best_avail_max_trim_order);

#define EXT4_RW_SBI_UINT(name, member) \
	EXT4_ATTRIBUTE_OFFSET(name, 0644, UINT, SBI, ext4_sb_info, member)
#define EXT4_RW_SBI_INT(name, member) \
	EXT4_ATTRIBUTE_OFFSET(name, 0644, INT, SBI, ext4_sb_info, member)
#define EXT4_RW_SBI_ULONG(name, member) \
	EXT4_ATTRIBUTE_OFFSET(name, 0644, ULONG, SBI, ext4_sb_info, member)
#define EXT4_RO_SBI_ATOMIC(name, member) \
	EXT4_ATTRIBUTE_OFFSET(name, 0444, ATOMIC, SBI, ext4_sb_info, member)
#define EXT4_RO_DISK_UINT(name, member) \
	EXT4_ATTRIBUTE_OFFSET(name, 0444, UINT, DISK_SUPER, ext4_super_block, member)
#define EXT4_RO_DISK_U8(name, member) \
	EXT4_ATTRIBUTE_OFFSET(name, 0444, U8, DISK_SUPER, ext4_super_block, member)
#define EXT4_RO_DISK_U64(name, member) \
	EXT4_ATTRIBUTE_OFFSET(name, 0444, U64, DISK_SUPER, ext4_super_block, member)
#define EXT4_RO_DISK_STRING(name, member, width) \
	EXT4_ATTRIBUTE_STRING(name, 0444, width, DISK_SUPER, ext4_super_block, member)

EXT4_RW_SBI_UINT(inode_goal, s_inode_goal);
EXT4_RW_SBI_UINT(mb_stats, s_mb_stats);
EXT4_RW_SBI_UINT(mb_max_to_scan, s_mb_max_to_scan);
EXT4_RW_SBI_UINT(mb_min_to_scan, s_mb_min_to_scan);
EXT4_RW_SBI_UINT(mb_order2_req, s_mb_order2_reqs);
EXT4_RW_SBI_UINT(mb_stream_req, s_mb_stream_request);
EXT4_RW_SBI_UINT(mb_max_linear_groups, s_mb_max_linear_groups);
EXT4_RW_SBI_UINT(extent_max_zeroout_kb, s_extent_max_zeroout_kb);
EXT4_ATTRIBUTE(trigger_fs_error, 0200, TRIGGER_ERROR);
EXT4_RW_SBI_INT(err_ratelimit_interval_ms, s_err_ratelimit_state.interval);
EXT4_RW_SBI_INT(err_ratelimit_burst, s_err_ratelimit_state.burst);
EXT4_RW_SBI_INT(warning_ratelimit_interval_ms, s_warning_ratelimit_state.interval);
EXT4_RW_SBI_INT(warning_ratelimit_burst, s_warning_ratelimit_state.burst);
EXT4_RW_SBI_INT(msg_ratelimit_interval_ms, s_msg_ratelimit_state.interval);
EXT4_RW_SBI_INT(msg_ratelimit_burst, s_msg_ratelimit_state.burst);
#ifdef CONFIG_EXT4_DEBUG
EXT4_RW_SBI_ULONG(simulate_fail, s_simulate_fail);
#endif
EXT4_RO_SBI_ATOMIC(warning_count, s_warning_count);
EXT4_RO_SBI_ATOMIC(msg_count, s_msg_count);
EXT4_RO_DISK_UINT(errors_count, s_error_count);
EXT4_RO_DISK_U8(first_error_errcode, s_first_error_errcode);
EXT4_RO_DISK_U8(last_error_errcode, s_last_error_errcode);
EXT4_RO_DISK_UINT(first_error_ino, s_first_error_ino);
EXT4_RO_DISK_UINT(last_error_ino, s_last_error_ino);
EXT4_RO_DISK_U64(first_error_block, s_first_error_block);
EXT4_RO_DISK_U64(last_error_block, s_last_error_block);
EXT4_RO_DISK_UINT(first_error_line, s_first_error_line);
EXT4_RO_DISK_UINT(last_error_line, s_last_error_line);
EXT4_RO_DISK_STRING(first_error_func, s_first_error_func, 32);
EXT4_RO_DISK_STRING(last_error_func, s_last_error_func, 32);
EXT4_ATTRIBUTE(first_error_time, 0444, FIRST_ERROR_TIME);
EXT4_ATTRIBUTE(last_error_time, 0444, LAST_ERROR_TIME);
EXT4_ATTRIBUTE(journal_task, 0444, JOURNAL_TASK);
EXT4_RW_SBI_UINT(mb_prefetch, s_mb_prefetch);
EXT4_RW_SBI_UINT(mb_prefetch_limit, s_mb_prefetch_limit);
EXT4_RW_SBI_ULONG(last_trim_minblks, s_last_trim_minblks);

static unsigned int ext4_legacy_writeback_bump = 128;
EXT4_ATTRIBUTE_PTR(max_writeback_mb_bump, 0444, UINT,
		   &ext4_legacy_writeback_bump);

static struct attribute *ext4_attrs[] = {
	EXT4_ATTR_ENTRY(delayed_allocation_blocks),
	EXT4_ATTR_ENTRY(session_write_kbytes),
	EXT4_ATTR_ENTRY(lifetime_write_kbytes),
	EXT4_ATTR_ENTRY(reserved_clusters),
	EXT4_ATTR_ENTRY(sra_exceeded_retry_limit),
	EXT4_ATTR_ENTRY(inode_readahead_blks),
	EXT4_ATTR_ENTRY(inode_goal),
	EXT4_ATTR_ENTRY(mb_stats),
	EXT4_ATTR_ENTRY(mb_max_to_scan),
	EXT4_ATTR_ENTRY(mb_min_to_scan),
	EXT4_ATTR_ENTRY(mb_order2_req),
	EXT4_ATTR_ENTRY(mb_stream_req),
	EXT4_ATTR_ENTRY(mb_group_prealloc),
	EXT4_ATTR_ENTRY(mb_max_linear_groups),
	EXT4_ATTR_ENTRY(max_writeback_mb_bump),
	EXT4_ATTR_ENTRY(extent_max_zeroout_kb),
	EXT4_ATTR_ENTRY(trigger_fs_error),
	EXT4_ATTR_ENTRY(err_ratelimit_interval_ms),
	EXT4_ATTR_ENTRY(err_ratelimit_burst),
	EXT4_ATTR_ENTRY(warning_ratelimit_interval_ms),
	EXT4_ATTR_ENTRY(warning_ratelimit_burst),
	EXT4_ATTR_ENTRY(msg_ratelimit_interval_ms),
	EXT4_ATTR_ENTRY(msg_ratelimit_burst),
	EXT4_ATTR_ENTRY(mb_best_avail_max_trim_order),
	EXT4_ATTR_ENTRY(errors_count),
	EXT4_ATTR_ENTRY(warning_count),
	EXT4_ATTR_ENTRY(msg_count),
	EXT4_ATTR_ENTRY(first_error_ino),
	EXT4_ATTR_ENTRY(last_error_ino),
	EXT4_ATTR_ENTRY(first_error_block),
	EXT4_ATTR_ENTRY(last_error_block),
	EXT4_ATTR_ENTRY(first_error_line),
	EXT4_ATTR_ENTRY(last_error_line),
	EXT4_ATTR_ENTRY(first_error_func),
	EXT4_ATTR_ENTRY(last_error_func),
	EXT4_ATTR_ENTRY(first_error_errcode),
	EXT4_ATTR_ENTRY(last_error_errcode),
	EXT4_ATTR_ENTRY(first_error_time),
	EXT4_ATTR_ENTRY(last_error_time),
	EXT4_ATTR_ENTRY(journal_task),
#ifdef CONFIG_EXT4_DEBUG
	EXT4_ATTR_ENTRY(simulate_fail),
#endif
	EXT4_ATTR_ENTRY(mb_prefetch),
	EXT4_ATTR_ENTRY(mb_prefetch_limit),
	EXT4_ATTR_ENTRY(last_trim_minblks),
	NULL,
};

ATTRIBUTE_GROUPS(ext4);

#define EXT4_FEATURE_ATTRIBUTE(name) EXT4_ATTRIBUTE(name, 0444, FEATURE)

EXT4_FEATURE_ATTRIBUTE(lazy_itable_init);
EXT4_FEATURE_ATTRIBUTE(batched_discard);
EXT4_FEATURE_ATTRIBUTE(meta_bg_resize);
#ifdef CONFIG_FS_ENCRYPTION
EXT4_FEATURE_ATTRIBUTE(encryption);
EXT4_FEATURE_ATTRIBUTE(test_dummy_encryption_v2);
#endif
#if IS_ENABLED(CONFIG_UNICODE)
EXT4_FEATURE_ATTRIBUTE(casefold);
#endif
#ifdef CONFIG_FS_VERITY
EXT4_FEATURE_ATTRIBUTE(verity);
#endif
EXT4_FEATURE_ATTRIBUTE(metadata_csum_seed);
EXT4_FEATURE_ATTRIBUTE(fast_commit);
#if IS_ENABLED(CONFIG_UNICODE) && defined(CONFIG_FS_ENCRYPTION)
EXT4_FEATURE_ATTRIBUTE(encrypted_casefold);
#endif

static struct attribute *ext4_feat_attrs[] = {
	EXT4_ATTR_ENTRY(lazy_itable_init),
	EXT4_ATTR_ENTRY(batched_discard),
	EXT4_ATTR_ENTRY(meta_bg_resize),
#ifdef CONFIG_FS_ENCRYPTION
	EXT4_ATTR_ENTRY(encryption),
	EXT4_ATTR_ENTRY(test_dummy_encryption_v2),
#endif
#if IS_ENABLED(CONFIG_UNICODE)
	EXT4_ATTR_ENTRY(casefold),
#endif
#ifdef CONFIG_FS_VERITY
	EXT4_ATTR_ENTRY(verity),
#endif
	EXT4_ATTR_ENTRY(metadata_csum_seed),
	EXT4_ATTR_ENTRY(fast_commit),
#if IS_ENABLED(CONFIG_UNICODE) && defined(CONFIG_FS_ENCRYPTION)
	EXT4_ATTR_ENTRY(encrypted_casefold),
#endif
	NULL,
};

ATTRIBUTE_GROUPS(ext4_feat);

static ssize_t ext4_show_timestamp(char *buf, __le32 low, __u8 high)
{
	const time64_t value = ((time64_t)high << 32) + le32_to_cpu(low);
	return sysfs_emit(buf, "%lld\n", value);
}

static ssize_t ext4_show_generic(struct ext4_attr *attr,
				 struct ext4_sb_info *sbi,
				 char *buf)
{
	void *ptr = ext4_attr_pointer(attr, sbi);

	if (!ptr)
		return 0;

	switch (attr->kind) {
	case EXT4_ATTR_INODE_READAHEAD:
	case EXT4_ATTR_CLUSTERS_IN_GROUP:
	case EXT4_ATTR_MB_ORDER:
	case EXT4_ATTR_UINT:
		if (attr->source == EXT4_ATTR_DISK_SUPER)
			return sysfs_emit(buf, "%u\n", le32_to_cpup(ptr));
		return sysfs_emit(buf, "%u\n", *(unsigned int *)ptr);
	case EXT4_ATTR_INT:
		return sysfs_emit(buf, "%d\n", *(int *)ptr);
	case EXT4_ATTR_ULONG:
		return sysfs_emit(buf, "%lu\n", *(unsigned long *)ptr);
	case EXT4_ATTR_U8:
		return sysfs_emit(buf, "%u\n", *(unsigned char *)ptr);
	case EXT4_ATTR_U64:
		if (attr->source == EXT4_ATTR_DISK_SUPER)
			return sysfs_emit(buf, "%llu\n", le64_to_cpup(ptr));
		return sysfs_emit(buf, "%llu\n",
				  *(unsigned long long *)ptr);
	case EXT4_ATTR_STRING:
		return sysfs_emit(buf, "%.*s\n", attr->size, (char *)ptr);
	case EXT4_ATTR_ATOMIC:
		return sysfs_emit(buf, "%d\n", atomic_read((atomic_t *)ptr));
	default:
		return 0;
	}
}

static ssize_t ext4_attr_show(struct kobject *kobj,
			      struct attribute *attribute,
			      char *buf)
{
	struct ext4_sb_info *sbi =
		container_of(kobj, struct ext4_sb_info, s_kobj);
	struct ext4_attr *attr =
		container_of(attribute, struct ext4_attr, attr);

	switch (attr->kind) {
	case EXT4_ATTR_DIRTY_BLOCKS:
		return sysfs_emit(
			buf, "%llu\n",
			(s64)EXT4_C2B(
				sbi,
				percpu_counter_sum(
					&sbi->s_dirtyclusters_counter)));
	case EXT4_ATTR_SESSION_WRITES:
		return ext4_show_session_writes(sbi, buf);
	case EXT4_ATTR_LIFETIME_WRITES:
		return ext4_show_lifetime_writes(sbi, buf);
	case EXT4_ATTR_RESERVED_CLUSTERS:
		return sysfs_emit(
			buf, "%llu\n",
			(unsigned long long)
				atomic64_read(&sbi->s_resv_clusters));
	case EXT4_ATTR_SRA_RETRY_LIMIT:
		return sysfs_emit(
			buf, "%llu\n",
			(unsigned long long)
				percpu_counter_sum(
					&sbi->s_sra_exceeded_retry_limit));
	case EXT4_ATTR_FEATURE:
		return sysfs_emit(buf, "supported\n");
	case EXT4_ATTR_FIRST_ERROR_TIME:
		return ext4_show_timestamp(
			buf, sbi->s_es->s_first_error_time,
			sbi->s_es->s_first_error_time_hi);
	case EXT4_ATTR_LAST_ERROR_TIME:
		return ext4_show_timestamp(
			buf, sbi->s_es->s_last_error_time,
			sbi->s_es->s_last_error_time_hi);
	case EXT4_ATTR_JOURNAL_TASK:
		return ext4_show_journal_task(sbi, buf);
	default:
		return ext4_show_generic(attr, sbi, buf);
	}
}

static ssize_t ext4_store_generic(struct ext4_attr *attr,
				  struct ext4_sb_info *sbi,
				  const char *buf,
				  size_t count)
{
	void *ptr = ext4_attr_pointer(attr, sbi);
	unsigned int uvalue;
	unsigned long ulvalue;
	int ivalue;
	int err;

	if (!ptr)
		return -EINVAL;

	switch (attr->kind) {
	case EXT4_ATTR_INT:
		err = kstrtoint(skip_spaces(buf), 0, &ivalue);
		if (err)
			return err;
		if (ivalue < 0)
			return -EINVAL;
		*(int *)ptr = ivalue;
		return count;
	case EXT4_ATTR_UINT:
		err = kstrtouint(skip_spaces(buf), 0, &uvalue);
		if (err)
			return err;
		if (attr->source == EXT4_ATTR_DISK_SUPER)
			*(__le32 *)ptr = cpu_to_le32(uvalue);
		else
			*(unsigned int *)ptr = uvalue;
		return count;
	case EXT4_ATTR_MB_ORDER:
		err = kstrtouint(skip_spaces(buf), 0, &uvalue);
		if (err)
			return err;
		if (uvalue > 64)
			return -EINVAL;
		*(unsigned int *)ptr = uvalue;
		return count;
	case EXT4_ATTR_CLUSTERS_IN_GROUP:
		err = kstrtouint(skip_spaces(buf), 0, &uvalue);
		if (err)
			return err;
		if (uvalue > sbi->s_clusters_per_group)
			return -EINVAL;
		*(unsigned int *)ptr = uvalue;
		return count;
	case EXT4_ATTR_ULONG:
		err = kstrtoul(skip_spaces(buf), 0, &ulvalue);
		if (err)
			return err;
		*(unsigned long *)ptr = ulvalue;
		return count;
	default:
		return -EOPNOTSUPP;
	}
}

static ssize_t ext4_attr_store(struct kobject *kobj,
			       struct attribute *attribute,
			       const char *buf,
			       size_t count)
{
	struct ext4_sb_info *sbi =
		container_of(kobj, struct ext4_sb_info, s_kobj);
	struct ext4_attr *attr =
		container_of(attribute, struct ext4_attr, attr);

	switch (attr->kind) {
	case EXT4_ATTR_RESERVED_CLUSTERS:
		return ext4_store_reserved_clusters(sbi, buf, count);
	case EXT4_ATTR_INODE_READAHEAD:
		return ext4_store_inode_readahead(sbi, buf, count);
	case EXT4_ATTR_TRIGGER_ERROR:
		return ext4_store_test_error(sbi, buf, count);
	default:
		return ext4_store_generic(attr, sbi, buf, count);
	}
}

static void ext4_sb_kobj_release(struct kobject *kobj)
{
	struct ext4_sb_info *sbi =
		container_of(kobj, struct ext4_sb_info, s_kobj);
	complete(&sbi->s_kobj_unregister);
}

static void ext4_feature_kobj_release(struct kobject *kobj)
{
	kfree(kobj);
}

static const struct sysfs_ops ext4_sysfs_ops = {
	.show = ext4_attr_show,
	.store = ext4_attr_store,
};

static const struct kobj_type ext4_sb_ktype = {
	.default_groups = ext4_groups,
	.sysfs_ops = &ext4_sysfs_ops,
	.release = ext4_sb_kobj_release,
};

static const struct kobj_type ext4_feature_ktype = {
	.default_groups = ext4_feat_groups,
	.sysfs_ops = &ext4_sysfs_ops,
	.release = ext4_feature_kobj_release,
};

void ext4_notify_error_sysfs(struct ext4_sb_info *sbi)
{
	mutex_lock(&sbi->s_error_notify_mutex);
	if (sbi->s_kobj.state_in_sysfs)
		sysfs_notify(&sbi->s_kobj, NULL, "errors_count");
	mutex_unlock(&sbi->s_error_notify_mutex);
}

int ext4_register_sysfs(struct super_block *sb)
{
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	int err;

	init_completion(&sbi->s_kobj_unregister);
	mutex_lock(&sbi->s_error_notify_mutex);
	err = kobject_init_and_add(
		&sbi->s_kobj, &ext4_sb_ktype, ext4_root, "%s", sb->s_id);
	mutex_unlock(&sbi->s_error_notify_mutex);
	if (err) {
		kobject_put(&sbi->s_kobj);
		wait_for_completion(&sbi->s_kobj_unregister);
		return err;
	}

	if (ext4_proc_root)
		sbi->s_proc = proc_mkdir(sb->s_id, ext4_proc_root);
	if (!sbi->s_proc)
		return 0;

	proc_create_single_data(
		"options", S_IRUGO, sbi->s_proc,
		ext4_seq_options_show, sb);
	proc_create_single_data(
		"es_shrinker_info", S_IRUGO, sbi->s_proc,
		ext4_seq_es_shrinker_info_show, sb);
	proc_create_single_data(
		"fc_info", 0444, sbi->s_proc,
		ext4_fc_info_show, sb);
	proc_create_seq_data(
		"mb_groups", S_IRUGO, sbi->s_proc,
		&ext4_mb_seq_groups_ops, sb);
	proc_create_single_data(
		"mb_stats", 0444, sbi->s_proc,
		ext4_seq_mb_stats_show, sb);
	proc_create_seq_data(
		"mb_structs_summary", 0444, sbi->s_proc,
		&ext4_mb_seq_structs_summary_ops, sb);
	return 0;
}

void ext4_unregister_sysfs(struct super_block *sb)
{
	struct ext4_sb_info *sbi = EXT4_SB(sb);

	if (sbi->s_proc)
		remove_proc_subtree(sb->s_id, ext4_proc_root);

	mutex_lock(&sbi->s_error_notify_mutex);
	kobject_del(&sbi->s_kobj);
	mutex_unlock(&sbi->s_error_notify_mutex);
}

int __init ext4_init_sysfs(void)
{
	int err;

	ext4_root = kobject_create_and_add("ext4", fs_kobj);
	if (!ext4_root)
		return -ENOMEM;

	ext4_features = kzalloc(sizeof(*ext4_features), GFP_KERNEL);
	if (!ext4_features) {
		err = -ENOMEM;
		goto fail_root;
	}

	err = kobject_init_and_add(
		ext4_features, &ext4_feature_ktype,
		ext4_root, "features");
	if (err)
		goto fail_features;

	ext4_proc_root = proc_mkdir(ext4_proc_path, NULL);
	return 0;

fail_features:
	kobject_put(ext4_features);
	ext4_features = NULL;
fail_root:
	kobject_put(ext4_root);
	ext4_root = NULL;
	return err;
}

void ext4_exit_sysfs(void)
{
	kobject_put(ext4_features);
	ext4_features = NULL;
	kobject_put(ext4_root);
	ext4_root = NULL;
	remove_proc_entry(ext4_proc_path, NULL);
	ext4_proc_root = NULL;
}
