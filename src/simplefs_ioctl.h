#ifndef SIMPLEFS_IOCTL_H
#define SIMPLEFS_IOCTL_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define SIMPLEFS_IOCTL_MAGIC 'S'
#define SIMPLEFS_MAX_IOCTL_NAME 256

struct simplefs_meta_entry {
	__u64 index;
	__u64 first_sector;
	__u32 sectors;
	__u32 hash;
	char name[SIMPLEFS_MAX_IOCTL_NAME];
};

struct simplefs_meta_arg {
	__u64 count;
	__u64 total;
	__u64 entries;
};

struct simplefs_map_arg {
	char name[SIMPLEFS_MAX_IOCTL_NAME];
	__u64 count;
	__u64 total;
	__u64 sectors;
};

#define SIMPLEFS_IOCTL_ZERO_ALL _IO(SIMPLEFS_IOCTL_MAGIC, 1)
#define SIMPLEFS_IOCTL_ERASE_FS _IO(SIMPLEFS_IOCTL_MAGIC, 2)
#define SIMPLEFS_IOCTL_GET_META _IOWR(SIMPLEFS_IOCTL_MAGIC, 3, struct simplefs_meta_arg)
#define SIMPLEFS_IOCTL_GET_MAP _IOWR(SIMPLEFS_IOCTL_MAGIC, 4, struct simplefs_map_arg)

#endif
