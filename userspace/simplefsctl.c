#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "../src/simplefs_ioctl.h"

static int open_mount(const char *path)
{
	int fd = open(path, O_RDONLY | O_DIRECTORY);

	if (fd < 0)
		perror(path);
	return fd;
}

static void usage(const char *argv0)
{
	fprintf(stderr,
		"usage:\n"
		"  %s demo <mountpoint>\n"
		"  %s zero <mountpoint>\n"
		"  %s erase <mountpoint>\n"
		"  %s meta <mountpoint>\n"
		"  %s map <mountpoint> <file>\n",
		argv0, argv0, argv0, argv0, argv0);
}

static int cmd_zero(const char *mountpoint)
{
	int fd = open_mount(mountpoint);
	int ret;

	if (fd < 0)
		return 1;
	ret = ioctl(fd, SIMPLEFS_IOCTL_ZERO_ALL);
	if (ret < 0) {
		perror("ioctl zero");
		close(fd);
		return 1;
	}
	close(fd);
	puts("all files were zeroed");
	return 0;
}

static int cmd_erase(const char *mountpoint)
{
	int fd = open_mount(mountpoint);
	int ret;

	if (fd < 0)
		return 1;
	ret = ioctl(fd, SIMPLEFS_IOCTL_ERASE_FS);
	if (ret < 0) {
		perror("ioctl erase");
		close(fd);
		return 1;
	}
	close(fd);
	puts("superblocks were erased");
	return 0;
}

static int cmd_meta(const char *mountpoint)
{
	int fd = open_mount(mountpoint);
	struct simplefs_meta_arg arg;
	struct simplefs_meta_entry *entries = NULL;
	uint64_t i;

	if (fd < 0)
		return 1;

	memset(&arg, 0, sizeof(arg));
	if (ioctl(fd, SIMPLEFS_IOCTL_GET_META, &arg) < 0) {
		perror("ioctl meta count");
		close(fd);
		return 1;
	}

	entries = calloc(arg.total ? arg.total : 1, sizeof(*entries));
	if (!entries) {
		perror("calloc");
		close(fd);
		return 1;
	}

	arg.count = arg.total;
	arg.entries = (uint64_t)(uintptr_t)entries;
	if (ioctl(fd, SIMPLEFS_IOCTL_GET_META, &arg) < 0) {
		perror("ioctl meta");
		free(entries);
		close(fd);
		return 1;
	}

	printf("files: %" PRIu64 "\n", arg.total);
	for (i = 0; i < arg.total; i++)
		printf("%s index=%" PRIu64 " first_sector=%" PRIu64 " sectors=%u crc32=%08x\n",
		       entries[i].name, entries[i].index, entries[i].first_sector,
		       entries[i].sectors, entries[i].hash);

	free(entries);
	close(fd);
	return 0;
}

static int cmd_map(const char *mountpoint, const char *name)
{
	int fd = open_mount(mountpoint);
	struct simplefs_map_arg arg;
	uint64_t *sectors = NULL;
	uint64_t i;

	if (fd < 0)
		return 1;

	memset(&arg, 0, sizeof(arg));
	snprintf(arg.name, sizeof(arg.name), "%s", name);
	if (ioctl(fd, SIMPLEFS_IOCTL_GET_MAP, &arg) < 0) {
		perror("ioctl map count");
		close(fd);
		return 1;
	}

	sectors = calloc(arg.total ? arg.total : 1, sizeof(*sectors));
	if (!sectors) {
		perror("calloc");
		close(fd);
		return 1;
	}

	arg.count = arg.total;
	arg.sectors = (uint64_t)(uintptr_t)sectors;
	if (ioctl(fd, SIMPLEFS_IOCTL_GET_MAP, &arg) < 0) {
		perror("ioctl map");
		free(sectors);
		close(fd);
		return 1;
	}

	printf("%s:", name);
	for (i = 0; i < arg.total; i++)
		printf(" %" PRIu64, sectors[i]);
	putchar('\n');

	free(sectors);
	close(fd);
	return 0;
}

static int write_number(const char *path, uint64_t value)
{
	int fd = open(path, O_WRONLY);
	ssize_t written;

	if (fd < 0) {
		perror(path);
		return -1;
	}

	written = pwrite(fd, &value, sizeof(value), 0);
	close(fd);
	if (written != (ssize_t)sizeof(value)) {
		perror("pwrite");
		return -1;
	}
	return 0;
}

static int read_number(const char *path, uint64_t *value)
{
	int fd = open(path, O_RDONLY);
	ssize_t received;

	if (fd < 0) {
		perror(path);
		return -1;
	}

	received = pread(fd, value, sizeof(*value), 0);
	close(fd);
	if (received != (ssize_t)sizeof(*value)) {
		perror("pread");
		return -1;
	}
	return 0;
}

static int cmd_demo(const char *mountpoint)
{
	DIR *dir = opendir(mountpoint);
	struct dirent *de;
	unsigned int ok = 0;
	unsigned int fail = 0;

	if (!dir) {
		perror(mountpoint);
		return 1;
	}

	srand((unsigned int)time(NULL));
	while ((de = readdir(dir))) {
		char path[4096];
		uint64_t value;
		uint64_t received;

		if (de->d_name[0] == '.')
			continue;
		snprintf(path, sizeof(path), "%s/%s", mountpoint, de->d_name);
		value = ((uint64_t)rand() << 32) ^ (uint64_t)rand();

		if (write_number(path, value) == 0 &&
		    read_number(path, &received) == 0 &&
		    received == value) {
			ok++;
		} else {
			fprintf(stderr, "check failed for %s\n", path);
			fail++;
		}
	}

	closedir(dir);
	printf("demo result: ok=%u fail=%u\n", ok, fail);
	return fail ? 1 : 0;
}

int main(int argc, char **argv)
{
	if (argc < 3) {
		usage(argv[0]);
		return 1;
	}

	if (strcmp(argv[1], "demo") == 0)
		return cmd_demo(argv[2]);
	if (strcmp(argv[1], "zero") == 0)
		return cmd_zero(argv[2]);
	if (strcmp(argv[1], "erase") == 0)
		return cmd_erase(argv[2]);
	if (strcmp(argv[1], "meta") == 0)
		return cmd_meta(argv[2]);
	if (strcmp(argv[1], "map") == 0 && argc >= 4)
		return cmd_map(argv[2], argv[3]);

	usage(argv[0]);
	return 1;
}
