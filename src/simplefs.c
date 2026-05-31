#include <linux/blkdev.h>
#include <linux/buffer_head.h>
#include <linux/cred.h>
#include <linux/crc32.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>

#include "simplefs_ioctl.h"

#define SIMPLEFS_MAGIC 0x53465331
#define SIMPLEFS_VERSION 1
#define SIMPLEFS_SECTOR_SIZE 512
#define SIMPLEFS_ROOT_INO 1
#define SIMPLEFS_FIRST_FILE_INO 2
#define SIMPLEFS_DEFAULT_NAME_LEN 32
#define SIMPLEFS_DEFAULT_FILE_SECTORS 1
#define SIMPLEFS_DEFAULT_SB1 0
#define SIMPLEFS_DEFAULT_SB2 8

static char *disk_name = "";
static ulong sb1_sector = SIMPLEFS_DEFAULT_SB1;
static ulong sb2_sector = SIMPLEFS_DEFAULT_SB2;
static uint max_name_len = SIMPLEFS_DEFAULT_NAME_LEN;
static uint max_file_sectors = SIMPLEFS_DEFAULT_FILE_SECTORS;

module_param(disk_name, charp, 0444);
module_param(sb1_sector, ulong, 0444);
module_param(sb2_sector, ulong, 0444);
module_param(max_name_len, uint, 0444);
module_param(max_file_sectors, uint, 0444);

MODULE_PARM_DESC(disk_name, "Expected block device name, for example /dev/loop0");
MODULE_PARM_DESC(sb1_sector, "Sector with primary superblock");
MODULE_PARM_DESC(sb2_sector, "Sector with backup superblock");
MODULE_PARM_DESC(max_name_len, "Maximum generated file name length");
MODULE_PARM_DESC(max_file_sectors, "Maximum file size in sectors");

struct simplefs_disk_super {
	__le32 magic;
	__le32 version;
	__le64 total_sectors;
	__le64 sb1_sector;
	__le64 sb2_sector;
	__le32 max_name_len;
	__le32 file_sectors;
	__le64 file_count;
	__le32 checksum;
	__u8 reserved[456];
};

struct simplefs_info {
	u64 total_sectors;
	u64 sb1_sector;
	u64 sb2_sector;
	u32 name_len;
	u32 file_sectors;
	u64 file_count;
};

static const struct super_operations simplefs_super_ops;
static const struct inode_operations simplefs_dir_inode_ops;
static const struct file_operations simplefs_dir_ops;
static const struct file_operations simplefs_file_ops;

static u32 simplefs_super_crc(const struct simplefs_disk_super *ds)
{
	struct simplefs_disk_super tmp = *ds;

	tmp.checksum = 0;
	return crc32(~0U, &tmp, sizeof(tmp));
}

static bool simplefs_super_valid(const struct simplefs_disk_super *ds)
{
	if (le32_to_cpu(ds->magic) != SIMPLEFS_MAGIC)
		return false;
	if (le32_to_cpu(ds->version) != SIMPLEFS_VERSION)
		return false;
	return le32_to_cpu(ds->checksum) == simplefs_super_crc(ds);
}

static bool simplefs_super_empty(const struct simplefs_disk_super *ds)
{
	const u8 *data = (const u8 *)ds;
	size_t i;

	for (i = 0; i < sizeof(*ds); i++) {
		if (data[i])
			return false;
	}
	return true;
}

static int simplefs_super_to_info(struct super_block *sb,
				  const struct simplefs_disk_super *ds,
				  struct simplefs_info *info)
{
	u64 total_sectors = le64_to_cpu(ds->total_sectors);
	u64 disk_sb1 = le64_to_cpu(ds->sb1_sector);
	u64 disk_sb2 = le64_to_cpu(ds->sb2_sector);
	u32 name_len = le32_to_cpu(ds->max_name_len);
	u32 file_sectors = le32_to_cpu(ds->file_sectors);
	u64 file_count = le64_to_cpu(ds->file_count);
	u64 usable;

	if (total_sectors != bdev_nr_sectors(sb->s_bdev))
		return -EINVAL;
	if (disk_sb1 != sb1_sector || disk_sb2 != sb2_sector)
		return -EINVAL;
	if (disk_sb1 >= total_sectors || disk_sb2 >= total_sectors ||
	    disk_sb1 == disk_sb2)
		return -EINVAL;
	if (name_len < 5 || name_len >= SIMPLEFS_MAX_IOCTL_NAME)
		return -EINVAL;
	if (!file_sectors)
		return -EINVAL;

	usable = total_sectors - 2;
	if (file_count > div_u64(usable, file_sectors))
		return -EINVAL;

	info->total_sectors = total_sectors;
	info->sb1_sector = disk_sb1;
	info->sb2_sector = disk_sb2;
	info->name_len = name_len;
	info->file_sectors = file_sectors;
	info->file_count = file_count;
	return 0;
}

static void simplefs_super_build(struct simplefs_disk_super *ds,
				 const struct simplefs_info *info)
{
	memset(ds, 0, sizeof(*ds));
	ds->magic = cpu_to_le32(SIMPLEFS_MAGIC);
	ds->version = cpu_to_le32(SIMPLEFS_VERSION);
	ds->total_sectors = cpu_to_le64(info->total_sectors);
	ds->sb1_sector = cpu_to_le64(info->sb1_sector);
	ds->sb2_sector = cpu_to_le64(info->sb2_sector);
	ds->max_name_len = cpu_to_le32(info->name_len);
	ds->file_sectors = cpu_to_le32(info->file_sectors);
	ds->file_count = cpu_to_le64(info->file_count);
	ds->checksum = cpu_to_le32(simplefs_super_crc(ds));
}

static int simplefs_write_sector(struct super_block *sb, u64 sector,
				 const void *data, size_t len)
{
	struct buffer_head *bh;

	if (sector >= ((struct simplefs_info *)sb->s_fs_info)->total_sectors)
		return -EINVAL;

	bh = sb_bread(sb, sector);
	if (!bh)
		return -EIO;

	memset(bh->b_data, 0, SIMPLEFS_SECTOR_SIZE);
	memcpy(bh->b_data, data, len);
	mark_buffer_dirty(bh);
	sync_dirty_buffer(bh);
	brelse(bh);
	return 0;
}

static int simplefs_read_disk_super(struct super_block *sb, u64 sector,
				    struct simplefs_disk_super *out)
{
	struct buffer_head *bh;

	bh = sb_bread(sb, sector);
	if (!bh)
		return -EIO;
	memcpy(out, bh->b_data, sizeof(*out));
	brelse(bh);
	return 0;
}

static int simplefs_write_disk_super(struct super_block *sb,
				     const struct simplefs_info *info)
{
	struct simplefs_disk_super ds;
	int ret;

	simplefs_super_build(&ds, info);
	ret = simplefs_write_sector(sb, info->sb1_sector, &ds, sizeof(ds));
	if (ret)
		return ret;
	return simplefs_write_sector(sb, info->sb2_sector, &ds, sizeof(ds));
}

static void simplefs_name_for_index(u64 index, char *buf, size_t size)
{
	snprintf(buf, size, "file%llu", index);
}

static bool simplefs_parse_name(const char *name, u64 *index)
{
	unsigned long long value = 0;
	int ret;

	if (strncmp(name, "file", 4) != 0 || !name[4])
		return false;
	ret = kstrtoull(name + 4, 10, &value);
	if (ret)
		return false;
	*index = value;
	return true;
}

static bool simplefs_is_super_sector(const struct simplefs_info *info, u64 sector)
{
	return sector == info->sb1_sector || sector == info->sb2_sector;
}

static u64 simplefs_file_sector(const struct simplefs_info *info, u64 index,
				u32 offset)
{
	u64 wanted = index * info->file_sectors + offset;
	u64 sector;

	for (sector = 0; sector < info->total_sectors; sector++) {
		if (simplefs_is_super_sector(info, sector))
			continue;
		if (wanted == 0)
			return sector;
		wanted--;
	}
	return U64_MAX;
}

static u64 simplefs_file_size(const struct simplefs_info *info)
{
	return (u64)info->file_sectors * SIMPLEFS_SECTOR_SIZE;
}

static struct inode *simplefs_get_inode(struct super_block *sb, umode_t mode,
					u64 ino)
{
	struct inode *inode;
	struct simplefs_info *info = sb->s_fs_info;

	inode = new_inode(sb);
	if (!inode)
		return NULL;

	inode->i_ino = ino;
	inode->i_mode = mode;
	inode->i_uid = current_fsuid();
	inode->i_gid = current_fsgid();
	inode_set_atime_to_ts(inode, current_time(inode));
	inode_set_mtime_to_ts(inode, current_time(inode));
	inode_set_ctime_to_ts(inode, current_time(inode));

	if (S_ISDIR(mode)) {
		inode->i_op = &simplefs_dir_inode_ops;
		inode->i_fop = &simplefs_dir_ops;
		set_nlink(inode, 2);
		inode->i_size = SIMPLEFS_SECTOR_SIZE;
	} else {
		inode->i_fop = &simplefs_file_ops;
		set_nlink(inode, 1);
		inode->i_size = simplefs_file_size(info);
	}

	return inode;
}

static struct dentry *simplefs_lookup(struct inode *dir, struct dentry *dentry,
				      unsigned int flags)
{
	struct simplefs_info *info = dir->i_sb->s_fs_info;
	struct inode *inode = NULL;
	u64 index;

	if (dentry->d_name.len > info->name_len)
		return ERR_PTR(-ENAMETOOLONG);

	if (simplefs_parse_name(dentry->d_name.name, &index) &&
	    index < info->file_count)
		inode = simplefs_get_inode(dir->i_sb, S_IFREG | 0644,
					   SIMPLEFS_FIRST_FILE_INO + index);

	d_add(dentry, inode);
	return NULL;
}

static int simplefs_iterate(struct file *file, struct dir_context *ctx)
{
	struct simplefs_info *info = file_inode(file)->i_sb->s_fs_info;
	char name[SIMPLEFS_MAX_IOCTL_NAME];
	u64 index;

	if (!dir_emit_dots(file, ctx))
		return 0;

	for (index = ctx->pos - 2; index < info->file_count; index++) {
		simplefs_name_for_index(index, name, sizeof(name));
		if (!dir_emit(ctx, name, strlen(name),
			      SIMPLEFS_FIRST_FILE_INO + index, DT_REG))
			return 0;
		ctx->pos = index + 3;
	}

	return 0;
}

static ssize_t simplefs_read(struct file *file, char __user *buf, size_t len,
			     loff_t *ppos)
{
	struct inode *inode = file_inode(file);
	struct simplefs_info *info = inode->i_sb->s_fs_info;
	u64 index = inode->i_ino - SIMPLEFS_FIRST_FILE_INO;
	u64 size = simplefs_file_size(info);
	size_t done = 0;

	if (*ppos >= size)
		return 0;
	if (len > size - *ppos)
		len = size - *ppos;

	while (done < len) {
		u64 sector_index = (*ppos + done) / SIMPLEFS_SECTOR_SIZE;
		u32 sector_offset = (*ppos + done) % SIMPLEFS_SECTOR_SIZE;
		u32 chunk = min_t(u32, SIMPLEFS_SECTOR_SIZE - sector_offset,
				  len - done);
		u64 disk_sector = simplefs_file_sector(info, index, sector_index);
		struct buffer_head *bh;

		if (disk_sector == U64_MAX)
			return done ? done : -EIO;
		bh = sb_bread(inode->i_sb, disk_sector);
		if (!bh)
			return done ? done : -EIO;
		if (copy_to_user(buf + done, bh->b_data + sector_offset, chunk)) {
			brelse(bh);
			return done ? done : -EFAULT;
		}
		brelse(bh);
		done += chunk;
	}

	*ppos += done;
	return done;
}

static ssize_t simplefs_write(struct file *file, const char __user *buf,
			      size_t len, loff_t *ppos)
{
	struct inode *inode = file_inode(file);
	struct simplefs_info *info = inode->i_sb->s_fs_info;
	u64 index = inode->i_ino - SIMPLEFS_FIRST_FILE_INO;
	u64 size = simplefs_file_size(info);
	size_t done = 0;

	if (*ppos >= size)
		return -ENOSPC;
	if (len > size - *ppos)
		len = size - *ppos;

	while (done < len) {
		u64 sector_index = (*ppos + done) / SIMPLEFS_SECTOR_SIZE;
		u32 sector_offset = (*ppos + done) % SIMPLEFS_SECTOR_SIZE;
		u32 chunk = min_t(u32, SIMPLEFS_SECTOR_SIZE - sector_offset,
				  len - done);
		u64 disk_sector = simplefs_file_sector(info, index, sector_index);
		struct buffer_head *bh;

		if (disk_sector == U64_MAX)
			return done ? done : -EIO;
		bh = sb_bread(inode->i_sb, disk_sector);
		if (!bh)
			return done ? done : -EIO;
		if (copy_from_user(bh->b_data + sector_offset, buf + done, chunk)) {
			brelse(bh);
			return done ? done : -EFAULT;
		}
		mark_buffer_dirty(bh);
		sync_dirty_buffer(bh);
		brelse(bh);
		done += chunk;
	}

	*ppos += done;
	inode_set_mtime_to_ts(inode, current_time(inode));
	inode_set_ctime_to_ts(inode, current_time(inode));
	return done;
}

static int simplefs_zero_file(struct super_block *sb, u64 index)
{
	struct simplefs_info *info = sb->s_fs_info;
	char zero[SIMPLEFS_SECTOR_SIZE] = { 0 };
	u32 i;
	int ret;

	for (i = 0; i < info->file_sectors; i++) {
		ret = simplefs_write_sector(sb, simplefs_file_sector(info, index, i),
					    zero, sizeof(zero));
		if (ret)
			return ret;
	}
	return 0;
}

static int simplefs_zero_all(struct super_block *sb)
{
	struct simplefs_info *info = sb->s_fs_info;
	u64 i;
	int ret;

	for (i = 0; i < info->file_count; i++) {
		ret = simplefs_zero_file(sb, i);
		if (ret)
			return ret;
	}
	return 0;
}

static int simplefs_erase(struct super_block *sb)
{
	struct simplefs_info *info = sb->s_fs_info;
	char zero[SIMPLEFS_SECTOR_SIZE] = { 0 };
	int ret;

	ret = simplefs_zero_all(sb);
	if (ret)
		return ret;

	ret = simplefs_write_sector(sb, info->sb1_sector, zero, sizeof(zero));
	if (ret)
		return ret;
	ret = simplefs_write_sector(sb, info->sb2_sector, zero, sizeof(zero));
	if (ret)
		return ret;

	info->file_count = 0;
	shrink_dcache_sb(sb);
	invalidate_inodes(sb);
	return 0;
}

static u32 simplefs_hash_file(struct super_block *sb, u64 index)
{
	struct simplefs_info *info = sb->s_fs_info;
	u32 hash = ~0U;
	u32 i;

	for (i = 0; i < info->file_sectors; i++) {
		struct buffer_head *bh;
		u64 sector = simplefs_file_sector(info, index, i);

		bh = sb_bread(sb, sector);
		if (!bh)
			continue;
		hash = crc32(hash, bh->b_data, SIMPLEFS_SECTOR_SIZE);
		brelse(bh);
	}
	return hash;
}

static long simplefs_ioctl(struct file *file, unsigned int cmd,
			   unsigned long arg)
{
	struct super_block *sb = file_inode(file)->i_sb;
	struct simplefs_info *info = sb->s_fs_info;

	switch (cmd) {
	case SIMPLEFS_IOCTL_ZERO_ALL:
		return simplefs_zero_all(sb);
	case SIMPLEFS_IOCTL_ERASE_FS:
		return simplefs_erase(sb);
	case SIMPLEFS_IOCTL_GET_META: {
		struct simplefs_meta_arg meta;
		u64 i;

		if (copy_from_user(&meta, (void __user *)arg, sizeof(meta)))
			return -EFAULT;
		meta.total = info->file_count;

		for (i = 0; i < min(meta.count, info->file_count); i++) {
			struct simplefs_meta_entry entry;

			memset(&entry, 0, sizeof(entry));
			entry.index = i;
			entry.first_sector = simplefs_file_sector(info, i, 0);
			entry.sectors = info->file_sectors;
			entry.hash = simplefs_hash_file(sb, i);
			simplefs_name_for_index(i, entry.name, sizeof(entry.name));

			if (copy_to_user((void __user *)(unsigned long)
					 (meta.entries + i * sizeof(entry)),
					 &entry, sizeof(entry)))
				return -EFAULT;
		}

		if (copy_to_user((void __user *)arg, &meta, sizeof(meta)))
			return -EFAULT;
		return 0;
	}
	case SIMPLEFS_IOCTL_GET_MAP: {
		struct simplefs_map_arg map;
		u64 index;
		u64 i;

		if (copy_from_user(&map, (void __user *)arg, sizeof(map)))
			return -EFAULT;
		map.name[SIMPLEFS_MAX_IOCTL_NAME - 1] = 0;

		if (!simplefs_parse_name(map.name, &index) || index >= info->file_count)
			return -ENOENT;

		map.total = info->file_sectors;
		for (i = 0; i < min_t(u64, map.count, info->file_sectors); i++) {
			u64 sector = simplefs_file_sector(info, index, i);

			if (copy_to_user((void __user *)(unsigned long)
					 (map.sectors + i * sizeof(sector)),
					 &sector, sizeof(sector)))
				return -EFAULT;
		}

		if (copy_to_user((void __user *)arg, &map, sizeof(map)))
			return -EFAULT;
		return 0;
	}
	default:
		return -ENOTTY;
	}
}

static const struct inode_operations simplefs_dir_inode_ops = {
	.lookup = simplefs_lookup,
};

static const struct file_operations simplefs_dir_ops = {
	.owner = THIS_MODULE,
	.iterate_shared = simplefs_iterate,
	.unlocked_ioctl = simplefs_ioctl,
	.llseek = generic_file_llseek,
};

static const struct file_operations simplefs_file_ops = {
	.owner = THIS_MODULE,
	.read = simplefs_read,
	.write = simplefs_write,
	.unlocked_ioctl = simplefs_ioctl,
	.llseek = generic_file_llseek,
};

static const struct super_operations simplefs_super_ops = {
	.statfs = simple_statfs,
	.drop_inode = generic_delete_inode,
};

static int simplefs_load_or_format(struct super_block *sb,
				   struct simplefs_info *info)
{
	struct simplefs_disk_super first;
	struct simplefs_disk_super second;
	bool first_ok;
	bool second_ok;
	u64 usable;
	int ret;

	info->total_sectors = bdev_nr_sectors(sb->s_bdev);
	info->sb1_sector = sb1_sector;
	info->sb2_sector = sb2_sector;
	info->name_len = clamp_t(u32, max_name_len, 5, SIMPLEFS_MAX_IOCTL_NAME - 1);
	info->file_sectors = max_t(u32, max_file_sectors, 1);

	if (info->sb1_sector >= info->total_sectors ||
	    info->sb2_sector >= info->total_sectors ||
	    info->sb1_sector == info->sb2_sector)
		return -EINVAL;

	ret = simplefs_read_disk_super(sb, info->sb1_sector, &first);
	if (ret)
		return ret;
	ret = simplefs_read_disk_super(sb, info->sb2_sector, &second);
	if (ret)
		return ret;

	first_ok = simplefs_super_valid(&first);
	second_ok = simplefs_super_valid(&second);

	if (first_ok && second_ok) {
		if (memcmp(&first, &second, sizeof(first)) != 0)
			return -EINVAL;
		return simplefs_super_to_info(sb, &first, info);
	}

	if (!simplefs_super_empty(&first) || !simplefs_super_empty(&second))
		return -EINVAL;

	usable = info->total_sectors - 2;
	info->file_count = div_u64(usable, info->file_sectors);
	if (!info->file_count)
		return -ENOSPC;

	return simplefs_write_disk_super(sb, info);
}

static int simplefs_fill_super(struct super_block *sb, void *data, int silent)
{
	struct simplefs_info *info;
	struct inode *root;
	int ret;

	ret = sb_set_blocksize(sb, SIMPLEFS_SECTOR_SIZE);
	if (!ret)
		return -EINVAL;

	info = kzalloc(sizeof(*info), GFP_KERNEL);
	if (!info)
		return -ENOMEM;
	sb->s_fs_info = info;

	ret = simplefs_load_or_format(sb, info);
	if (ret)
		goto fail;

	sb->s_magic = SIMPLEFS_MAGIC;
	sb->s_op = &simplefs_super_ops;

	root = simplefs_get_inode(sb, S_IFDIR | 0755, SIMPLEFS_ROOT_INO);
	if (!root) {
		ret = -ENOMEM;
		goto fail;
	}

	sb->s_root = d_make_root(root);
	if (!sb->s_root) {
		ret = -ENOMEM;
		goto fail;
	}

	return 0;

fail:
	kfree(info);
	sb->s_fs_info = NULL;
	return ret;
}

static struct dentry *simplefs_mount(struct file_system_type *fs_type, int flags,
				     const char *dev_name, void *data)
{
	if (disk_name && disk_name[0] && dev_name && strcmp(disk_name, dev_name) != 0)
		pr_info("simplefs: module disk_name is %s, mount device is %s\n",
			disk_name, dev_name);
	return mount_bdev(fs_type, flags, dev_name, data, simplefs_fill_super);
}

static void simplefs_kill_super(struct super_block *sb)
{
	kfree(sb->s_fs_info);
	kill_block_super(sb);
}

static struct file_system_type simplefs_type = {
	.owner = THIS_MODULE,
	.name = "simplefs",
	.mount = simplefs_mount,
	.kill_sb = simplefs_kill_super,
	.fs_flags = FS_REQUIRES_DEV,
};

static int __init simplefs_init(void)
{
	return register_filesystem(&simplefs_type);
}

static void __exit simplefs_exit(void)
{
	unregister_filesystem(&simplefs_type);
}

module_init(simplefs_init);
module_exit(simplefs_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Student");
MODULE_DESCRIPTION("Simple block backed filesystem homework for Linux 6.12");
