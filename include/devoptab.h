// SPDX-License-Identifier: Zlib
//
// Copyright (C) 2026 trustytrojan

#ifndef LIBNDS_DEVOPTAB_H__
#define LIBNDS_DEVOPTAB_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <sys/times.h>
#include <sys/unistd.h>
#include <time.h>

#include "sys/dirent.h"

// "devoptab" (device operation table?) API inspired by devkitPro's newlib implementation.

typedef struct
{
	const char *name;
	uint32_t flags;
	void *deviceData;
	int (*open_r)(const char *path, int flags);
	int (*close_r)(int);
	ssize_t (*write_r)(int, const char *ptr, size_t len);
	ssize_t (*read_r)(int fd, char *ptr, size_t len);
	off_t (*seek_r)(int fd, off_t pos, int dir);
} devoptab_t;

typedef struct
{
	devoptab_t dot;

	int (*fstat_r)(int fd, struct stat *st);
	int (*stat_r)(const char *file, struct stat *st);
	int (*link_r)(const char *existing, const char *newLink);
	int (*unlink_r)(const char *name);
	int (*chdir_r)(const char *name);
	char *(*getcwd_r)(char *buf, size_t size);
	int (*rename_r)(const char *oldName, const char *newName);
	int (*mkdir_r)(const char *path, int mode);

	DIR *(*diropen_r)(DIR **dirState, const char *path);
	int (*dirreset_r)(DIR *dirState);
	int (*dirnext_r)(DIR *dirState, char *filename, struct stat *filestat);
	int (*dirclose_r)(DIR *dirState);
	int (*statvfs_r)(const char *path, struct statvfs *buf);
	int (*ftruncate_r)(int fd, off_t len);
	int (*fsync_r)(int fd);

	int (*chmod_r)(const char *path, mode_t mode);
	int (*fchmod_r)(int fd, mode_t mode);
	int (*rmdir_r)(const char *name);
	int (*lstat_r)(const char *file, struct stat *st);
	int (*utimes_r)(const char *filename, const struct timeval times[2]);
} devoptab_posix_t;

#define DEVOPTAB_IS_POSIX 0x1

#define MAX_DEVICES 16

extern const devoptab_t dot_null;
extern const devoptab_t *devoptab_list[MAX_DEVICES];

// Adds your device to the first empty slot in the list and returns its index.
// Returns -1 if no more devices can be added.
int AddDevice(const devoptab_t *d);

// Removes the device from `devoptab_list` if `FindDevice()` finds it using `name`.
int RemoveDevice(const char *name);

// `name` should be suffixed with a `:`. This makes the function compatible
// with full paths to files starting with a device specifier like `dev:/path/to/file`.
// Returns the index of the devoptab in `devoptab_list`, or -1 if not found.
int FindDevice(const char *name);

// Returns the device at the specified index, or NULL if out of range / missing.
const devoptab_t *GetDevice(int index);

// Sets the default device returned by FindDevice() when no device prefix is present.
int SetDefaultDevice(int index);

// Returns the current default device index, or -1 if not set.
int GetDefaultDevice(void);

// Returns the `deviceData` associated with `fd` 's device.
// If called from inside a devoptab callback, `fd` can be the device-local file descriptor.
// Otherwise, `fd` is interpreted as a libc-level file descriptor.
void *GetDeviceDataByFd(int fd);

#ifdef __cplusplus
}
#endif

#endif // LIBNDS_DEVOPTAB_H__
