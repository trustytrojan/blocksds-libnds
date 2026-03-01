// SPDX-License-Identifier: Zlib
//
// Copyright (C) 2026 trustytrojan

#include <devoptab.h>

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdarg.h>
#include <sys/fcntl.h>
#include <sys/stat.h>
#include <sys/times.h>
#include <sys/unistd.h>
#include <time.h>

#define DEVOPTAB_MAX_FDS 128

typedef struct
{
	bool used;
	int device_idx;
	int device_fd;
} devoptab_fd_entry_t;

static devoptab_fd_entry_t fd_table[DEVOPTAB_MAX_FDS] = {
	{1, 0, 0},
	{1, 1, 1},
	{1, 2, 2}
};

bool current_drive_is_nitrofs = false;

// for dswifi compat
ssize_t (*socket_fn_write)(int, const void *, size_t) = NULL;
ssize_t (*socket_fn_read)(int, void *, size_t) = NULL;
int (*socket_fn_close)(int) = NULL;

static _Thread_local int callback_device_idx = -1;

static int fd_alloc(void)
{
	for (int fd = STDERR_FILENO + 1; fd < DEVOPTAB_MAX_FDS; fd++)
	{
		if (!fd_table[fd].used)
		{
			fd_table[fd].used = true;
			return fd;
		}
	}

	errno = ENFILE;
	return -1;
}

static void fd_release(int fd)
{
	if ((fd >= 0) && (fd < DEVOPTAB_MAX_FDS))
		fd_table[fd].used = false;
}

static const devoptab_fd_entry_t *fd_get_const(int fd)
{
	if ((fd < 0) || (fd >= DEVOPTAB_MAX_FDS) || !fd_table[fd].used)
	{
		errno = EBADF;
		return NULL;
	}

	return &fd_table[fd];
}

static const devoptab_t *path_get_device(const char *path, int *device_idx_out)
{
	if (path == NULL)
	{
		errno = EINVAL;
		return NULL;
	}

	const int device_idx = FindDevice(path);
	if (device_idx < 0)
	{
		errno = ENODEV;
		return NULL;
	}

	const devoptab_t *const device = GetDevice(device_idx);
	if (device == NULL)
	{
		errno = ENODEV;
		return NULL;
	}

	if (device_idx_out != NULL)
		*device_idx_out = device_idx;

	return device;
}

static const devoptab_t *fd_get_device(const devoptab_fd_entry_t *entry)
{
	const devoptab_t *const device = GetDevice(entry->device_idx);
	if (device == NULL)
	{
		errno = ENODEV;
		return NULL;
	}

	return device;
}

void *GetDeviceDataByFd(int fd)
{
	if (callback_device_idx >= 0)
	{
		const devoptab_t *const device = GetDevice(callback_device_idx);
		if (device == NULL)
		{
			errno = ENODEV;
			return NULL;
		}

		return device->deviceData;
	}

	if ((fd >= 0) && (fd < DEVOPTAB_MAX_FDS) && fd_table[fd].used)
	{
		const devoptab_t *const device = GetDevice(fd_table[fd].device_idx);
		if (device == NULL)
		{
			errno = ENODEV;
			return NULL;
		}

		return device->deviceData;
	}

	errno = EBADF;
	return NULL;
}

static const devoptab_posix_t *as_posix_device(const devoptab_t *device)
{
	if ((device->flags & DEVOPTAB_IS_POSIX) == 0)
	{
		errno = ENOSYS;
		return NULL;
	}

	return (const devoptab_posix_t *)device;
}

static const devoptab_posix_t *get_default_posix_device(int *device_idx_out)
{
	const int device_idx = GetDefaultDevice();
	if (device_idx < 0)
	{
		errno = ENODEV;
		return NULL;
	}

	const devoptab_t *const device = GetDevice(device_idx);
	if (device == NULL)
	{
		errno = ENODEV;
		return NULL;
	}

	const devoptab_posix_t *const posix = as_posix_device(device);
	if (posix == NULL)
		return NULL;

	if (device_idx_out != NULL)
		*device_idx_out = device_idx;

	return posix;
}

int open(const char *path, int flags, ...)
{
	int device_idx;
	const devoptab_t *const device = path_get_device(path, &device_idx);
	if (device == NULL)
		return -1;

	if (device->open_r == NULL)
	{
		errno = ENOSYS;
		return -1;
	}

	if (flags & O_CREAT)
	{
		va_list args;
		va_start(args, flags);
		(void)va_arg(args, int);
		va_end(args);
	}

	const int fd = fd_alloc();
	if (fd < 0)
		return -1;

	const int device_fd = device->open_r(path, flags);
	if (device_fd < 0)
	{
		fd_release(fd);
		return -1;
	}

	fd_table[fd].device_idx = device_idx;
	fd_table[fd].device_fd = device_fd;

	return fd;
}

int close(int fd)
{
	const devoptab_fd_entry_t *const entry = fd_get_const(fd);
	if (entry == NULL)
		return -1;

	const devoptab_t *const device = fd_get_device(entry);
	if (device == NULL)
		return -1;

	int ret = 0;
	if (device->close_r != NULL)
	{
		callback_device_idx = entry->device_idx;
		ret = device->close_r(entry->device_fd);
		callback_device_idx = -1;
	}

	fd_release(fd);
	return ret;
}

ssize_t write(int fd, const void *ptr, size_t len)
{
	const devoptab_fd_entry_t *const entry = fd_get_const(fd);
	if (entry == NULL)
		return -1;

	const devoptab_t *const device = fd_get_device(entry);
	if (device == NULL)
		return -1;

	if (device->write_r == NULL)
	{
		errno = ENOSYS;
		return -1;
	}

	callback_device_idx = entry->device_idx;
	const ssize_t ret = device->write_r(entry->device_fd, (const char *)ptr, len);
	callback_device_idx = -1;
	return ret;
}

ssize_t read(int fd, void *ptr, size_t len)
{
	const devoptab_fd_entry_t *const entry = fd_get_const(fd);
	if (entry == NULL)
		return -1;

	const devoptab_t *const device = fd_get_device(entry);
	if (device == NULL)
		return -1;

	if (device->read_r == NULL)
	{
		errno = ENOSYS;
		return -1;
	}

	callback_device_idx = entry->device_idx;
	const ssize_t ret = device->read_r(entry->device_fd, (char *)ptr, len);
	callback_device_idx = -1;
	return ret;
}

off_t lseek(int fd, off_t pos, int dir)
{
	const devoptab_fd_entry_t *const entry = fd_get_const(fd);
	if (entry == NULL)
		return -1;

	const devoptab_t *const device = fd_get_device(entry);
	if (device == NULL)
		return -1;

	if (device->seek_r == NULL)
	{
		errno = ENOSYS;
		return -1;
	}

	callback_device_idx = entry->device_idx;
	const off_t ret = device->seek_r(entry->device_fd, pos, dir);
	callback_device_idx = -1;
	return ret;
}

int fstat(int fd, struct stat *st)
{
	const devoptab_fd_entry_t *const entry = fd_get_const(fd);
	if (entry == NULL)
		return -1;

	const devoptab_t *const device = fd_get_device(entry);
	if (device == NULL)
		return -1;

	const devoptab_posix_t *const posix = as_posix_device(device);
	if (posix == NULL)
		return -1;

	if (posix->fstat_r == NULL)
	{
		errno = ENOSYS;
		return -1;
	}

	callback_device_idx = entry->device_idx;
	const int ret = posix->fstat_r(entry->device_fd, st);
	callback_device_idx = -1;
	return ret;
}

int stat(const char *file, struct stat *st)
{
	const devoptab_t *const device = path_get_device(file, NULL);
	if (device == NULL)
		return -1;

	const devoptab_posix_t *const posix = as_posix_device(device);
	if (posix == NULL)
		return -1;

	if (posix->stat_r == NULL)
	{
		errno = ENOSYS;
		return -1;
	}

	return posix->stat_r(file, st);
}

int link(const char *existing, const char *newLink)
{
	int source_idx;
	const devoptab_t *const source = path_get_device(existing, &source_idx);
	if (source == NULL)
		return -1;

	int target_idx;
	const devoptab_t *const target = path_get_device(newLink, &target_idx);
	if (target == NULL)
		return -1;

	if (source_idx != target_idx)
	{
		errno = EXDEV;
		return -1;
	}

	const devoptab_posix_t *const posix = as_posix_device(target);
	if (posix == NULL)
		return -1;

	if (posix->link_r == NULL)
	{
		errno = ENOSYS;
		return -1;
	}

	return posix->link_r(existing, newLink);
}

int unlink(const char *name)
{
	const devoptab_t *const device = path_get_device(name, NULL);
	if (device == NULL)
		return -1;

	const devoptab_posix_t *const posix = as_posix_device(device);
	if (posix == NULL)
		return -1;

	if (posix->unlink_r == NULL)
	{
		errno = ENOSYS;
		return -1;
	}

	return posix->unlink_r(name);
}

int chdir(const char *name)
{
	int device_idx;
	const devoptab_t *const device = path_get_device(name, &device_idx);
	if (device == NULL)
		return -1;

	const devoptab_posix_t *const posix = as_posix_device(device);
	if (posix == NULL)
		return -1;

	if (posix->chdir_r == NULL)
	{
		errno = ENOSYS;
		return -1;
	}

	const int ret = posix->chdir_r(name);
	if (ret == 0)
	{
		SetDefaultDevice(device_idx);

		const devoptab_t *const current = GetDevice(device_idx);
		current_drive_is_nitrofs =
			(current != NULL) && (strcmp(current->name, "nitro") == 0);
	}

	return ret;
}

char *getcwd(char *buf, size_t size)
{
	int device_idx;
	const devoptab_posix_t *const posix = get_default_posix_device(&device_idx);
	if (posix == NULL)
		return NULL;

	if (posix->getcwd_r == NULL)
	{
		errno = ENOSYS;
		return NULL;
	}

	callback_device_idx = device_idx;
	char *const ret = posix->getcwd_r(buf, size);
	callback_device_idx = -1;

	return ret;
}

char *getwd(char *buf)
{
	int device_idx;
	const devoptab_posix_t *const posix = get_default_posix_device(&device_idx);
	if (posix == NULL)
		return NULL;

	if (posix->getwd_r != NULL)
	{
		callback_device_idx = device_idx;
		char *const ret = posix->getwd_r(buf);
		callback_device_idx = -1;
		return ret;
	}

	return getcwd(buf, PATH_MAX);
}

char *get_current_dir_name(void)
{
	int device_idx;
	const devoptab_posix_t *const posix = get_default_posix_device(&device_idx);
	if (posix == NULL)
		return NULL;

	if (posix->get_current_dir_name_r != NULL)
	{
		callback_device_idx = device_idx;
		char *const ret = posix->get_current_dir_name_r();
		callback_device_idx = -1;
		return ret;
	}

	return getcwd(NULL, 0);
}

int rename(const char *oldName, const char *newName)
{
	int source_idx;
	const devoptab_t *const source = path_get_device(oldName, &source_idx);
	if (source == NULL)
		return -1;

	int target_idx;
	const devoptab_t *const target = path_get_device(newName, &target_idx);
	if (target == NULL)
		return -1;

	if (source_idx != target_idx)
	{
		errno = EXDEV;
		return -1;
	}

	const devoptab_posix_t *const posix = as_posix_device(target);
	if (posix == NULL)
		return -1;

	if (posix->rename_r == NULL)
	{
		errno = ENOSYS;
		return -1;
	}

	return posix->rename_r(oldName, newName);
}

int mkdir(const char *path, mode_t mode)
{
	const devoptab_t *const device = path_get_device(path, NULL);
	if (device == NULL)
		return -1;

	const devoptab_posix_t *const posix = as_posix_device(device);
	if (posix == NULL)
		return -1;

	if (posix->mkdir_r == NULL)
	{
		errno = ENOSYS;
		return -1;
	}

	return posix->mkdir_r(path, mode);
}

int statvfs(const char *path, struct statvfs *buf)
{
	const devoptab_t *const device = path_get_device(path, NULL);
	if (device == NULL)
		return -1;

	const devoptab_posix_t *const posix = as_posix_device(device);
	if (posix == NULL)
		return -1;

	if (posix->statvfs_r == NULL)
	{
		errno = ENOSYS;
		return -1;
	}

	return posix->statvfs_r(path, buf);
}

int ftruncate(int fd, off_t len)
{
	const devoptab_fd_entry_t *const entry = fd_get_const(fd);
	if (entry == NULL)
		return -1;

	const devoptab_t *const device = fd_get_device(entry);
	if (device == NULL)
		return -1;

	const devoptab_posix_t *const posix = as_posix_device(device);
	if (posix == NULL)
		return -1;

	if (posix->ftruncate_r == NULL)
	{
		errno = ENOSYS;
		return -1;
	}

	callback_device_idx = entry->device_idx;
	const int ret = posix->ftruncate_r(entry->device_fd, len);
	callback_device_idx = -1;
	return ret;
}

int fsync(int fd)
{
	const devoptab_fd_entry_t *const entry = fd_get_const(fd);
	if (entry == NULL)
		return -1;

	const devoptab_t *const device = fd_get_device(entry);
	if (device == NULL)
		return -1;

	const devoptab_posix_t *const posix = as_posix_device(device);
	if (posix == NULL)
		return -1;

	if (posix->fsync_r == NULL)
	{
		errno = ENOSYS;
		return -1;
	}

	callback_device_idx = entry->device_idx;
	const int ret = posix->fsync_r(entry->device_fd);
	callback_device_idx = -1;
	return ret;
}

int chmod(const char *path, mode_t mode)
{
	const devoptab_t *const device = path_get_device(path, NULL);
	if (device == NULL)
		return -1;

	const devoptab_posix_t *const posix = as_posix_device(device);
	if (posix == NULL)
		return -1;

	if (posix->chmod_r == NULL)
	{
		errno = ENOSYS;
		return -1;
	}

	return posix->chmod_r(path, mode);
}

int fchmod(int fd, mode_t mode)
{
	const devoptab_fd_entry_t *const entry = fd_get_const(fd);
	if (entry == NULL)
		return -1;

	const devoptab_t *const device = fd_get_device(entry);
	if (device == NULL)
		return -1;

	const devoptab_posix_t *const posix = as_posix_device(device);
	if (posix == NULL)
		return -1;

	if (posix->fchmod_r == NULL)
	{
		errno = ENOSYS;
		return -1;
	}

	callback_device_idx = entry->device_idx;
	const int ret = posix->fchmod_r(entry->device_fd, mode);
	callback_device_idx = -1;
	return ret;
}

int rmdir(const char *name)
{
	const devoptab_t *const device = path_get_device(name, NULL);
	if (device == NULL)
		return -1;

	const devoptab_posix_t *const posix = as_posix_device(device);
	if (posix == NULL)
		return -1;

	if (posix->rmdir_r == NULL)
	{
		errno = ENOSYS;
		return -1;
	}

	return posix->rmdir_r(name);
}

int lstat(const char *file, struct stat *st)
{
	const devoptab_t *const device = path_get_device(file, NULL);
	if (device == NULL)
		return -1;

	const devoptab_posix_t *const posix = as_posix_device(device);
	if (posix == NULL)
		return -1;

	if (posix->lstat_r == NULL)
	{
		errno = ENOSYS;
		return -1;
	}

	return posix->lstat_r(file, st);
}

int utimes(const char *filename, const struct timeval times[2])
{
	const devoptab_t *const device = path_get_device(filename, NULL);
	if (device == NULL)
		return -1;

	const devoptab_posix_t *const posix = as_posix_device(device);
	if (posix == NULL)
		return -1;

	if (posix->utimes_r == NULL)
	{
		errno = ENOSYS;
		return -1;
	}

	return posix->utimes_r(filename, times);
}

int truncate(const char *path, off_t len)
{
	if (path == NULL)
	{
		errno = EINVAL;
		return -1;
	}

	const int fd = open(path, O_WRONLY);
	if (fd < 0)
		return -1;

	const int truncate_ret = ftruncate(fd, len);
	const int truncate_errno = errno;

	const int close_ret = close(fd);
	if ((truncate_ret == 0) && (close_ret < 0))
		return -1;

	if (truncate_ret < 0)
		errno = truncate_errno;

	return truncate_ret;
}
