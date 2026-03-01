// SPDX-License-Identifier: Zlib
//
// Copyright (C) 2026 trustytrojan

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "devoptab.h"
#include "nitrofs_internal.h"

#define DIR DIRff
#include "fatfs_internal.h"
#undef DIR

#define FD_DESC(x) ((((uint32_t)(x))) & 0x0FFFFFFF)
#define FD_TYPE_NITRO 0x2

static int nitro_open(const char *path, int flags)
{
	if (path == NULL)
	{
		errno = EINVAL;
		return -1;
	}

	if ((flags & (O_WRONLY | O_RDWR | O_CREAT | O_TRUNC | O_APPEND)) != 0)
	{
		errno = EROFS;
		return -1;
	}

	return nitrofs_open(path);
}

static int nitro_close(int fd)
{
	return nitrofs_close(fd);
}

static ssize_t nitro_write(int fd, const char *ptr, size_t len)
{
	(void)fd;
	(void)ptr;
	(void)len;

	errno = EROFS;
	return -1;
}

static ssize_t nitro_read(int fd, char *ptr, size_t len)
{
	if (ptr == NULL)
	{
		errno = EINVAL;
		return -1;
	}

	return nitrofs_read(fd, ptr, len);
}

static off_t nitro_seek(int fd, off_t pos, int dir)
{
	return nitrofs_lseek(fd, pos, dir);
}

static int nitro_fstat(int fd, struct stat *st)
{
	if (st == NULL)
	{
		errno = EINVAL;
		return -1;
	}

	return nitrofs_fstat(fd, st);
}

static int nitro_stat(const char *file, struct stat *st)
{
	if ((file == NULL) || (st == NULL))
	{
		errno = EINVAL;
		return -1;
	}

	return nitrofs_stat(file, st);
}

static int nitro_link(const char *existing, const char *newLink)
{
	(void)existing;
	(void)newLink;
	errno = EROFS;
	return -1;
}

static int nitro_unlink(const char *name)
{
	(void)name;
	errno = EROFS;
	return -1;
}

static int nitro_chdir(const char *name)
{
	if (name == NULL)
	{
		errno = EINVAL;
		return -1;
	}

	const int result = nitrofs_chdir(name);
	if (result == FR_OK)
		return 0;

	errno = fatfs_error_to_posix(result);
	return -1;
}

static char *nitro_getcwd(char *buf, size_t size)
{
	if (buf == NULL)
	{
		int optimize_mem = 0;

		if (size == 0)
		{
			size = PATH_MAX + 1;
			optimize_mem = 1;
		}

		buf = malloc(size);
		if (buf == NULL)
		{
			errno = ENOMEM;
			return NULL;
		}

		char *const result = nitro_getcwd(buf, size);
		if (result == NULL)
		{
			free(buf);
			return NULL;
		}

		if (optimize_mem)
		{
			char *const ret = strdup(buf);
			free(buf);
			if (ret == NULL)
				errno = ENOMEM;
			return ret;
		}

		return buf;
	}

	if (size == 0)
	{
		errno = EINVAL;
		return NULL;
	}

	if (nitrofs_getcwd(buf, size - 1) != 0)
		return NULL;

	buf[size - 1] = '\0';
	return buf;
}

static int nitro_rename(const char *oldName, const char *newName)
{
	(void)oldName;
	(void)newName;
	errno = EROFS;
	return -1;
}

static int nitro_mkdir(const char *path, int mode)
{
	(void)path;
	(void)mode;
	errno = EROFS;
	return -1;
}

static DIR *nitro_diropen(DIR **dirState, const char *path)
{
	if ((dirState == NULL) || (path == NULL))
	{
		errno = EINVAL;
		return NULL;
	}

	DIR *const dir = calloc(1, sizeof(DIR));
	if (dir == NULL)
	{
		errno = ENOMEM;
		return NULL;
	}

	nitrofs_dir_state_t *const state = calloc(1, sizeof(nitrofs_dir_state_t));
	if (state == NULL)
	{
		free(dir);
		errno = ENOMEM;
		return NULL;
	}

	if (nitrofs_opendir(state, path) != 0)
	{
		free(state);
		free(dir);
		return NULL;
	}

	dir->dp = state;
	dir->index = -1;
	dir->dptype = FD_TYPE_NITRO;
	*dirState = dir;
	return dir;
}

static int nitro_dirreset(DIR *dirState)
{
	if ((dirState == NULL) || (dirState->dp == NULL))
	{
		errno = EBADF;
		return -1;
	}

	nitrofs_rewinddir((nitrofs_dir_state_t *)dirState->dp);
	dirState->index = -1;
	return 0;
}

static int nitro_dirnext(DIR *dirState, char *filename, struct stat *filestat)
{
	if ((dirState == NULL) || (dirState->dp == NULL) || (filename == NULL))
	{
		errno = EINVAL;
		return -1;
	}

	struct dirent ent;
	if (nitrofs_readdir((nitrofs_dir_state_t *)dirState->dp, &ent) != 0)
	{
		errno = ENOENT;
		return -1;
	}

	strcpy(filename, ent.d_name);
	dirState->index++;

	if (filestat != NULL)
	{
		memset(filestat, 0, sizeof(*filestat));
		filestat->st_ino = ent.d_ino;
		if (ent.d_type == DT_DIR)
			filestat->st_mode = S_IFDIR;
		else
			filestat->st_mode = S_IFREG;
	}

	return 0;
}

static int nitro_dirclose(DIR *dirState)
{
	if (dirState == NULL)
	{
		errno = EBADF;
		return -1;
	}

	free(dirState->dp);
	free(dirState);
	return 0;
}

static int nitro_ftruncate(int fd, off_t len)
{
	(void)fd;
	(void)len;
	errno = EROFS;
	return -1;
}

static int nitro_chmod(const char *path, mode_t mode)
{
	(void)path;
	(void)mode;
	errno = EROFS;
	return -1;
}

static int nitro_fchmod(int fd, mode_t mode)
{
	(void)fd;
	(void)mode;
	errno = EROFS;
	return -1;
}

static int nitro_rmdir(const char *name)
{
	(void)name;
	errno = EROFS;
	return -1;
}

static int nitro_lstat(const char *file, struct stat *st)
{
	return nitro_stat(file, st);
}

static int nitro_utimes(const char *filename, const struct timeval times[2])
{
	(void)filename;
	(void)times;
	errno = EROFS;
	return -1;
}

const devoptab_posix_t dot_nitrofs = {
	.dot = {
		.name = "nitro",
		.flags = DEVOPTAB_IS_POSIX,
		.open_r = nitro_open,
		.close_r = nitro_close,
		.write_r = nitro_write,
		.read_r = nitro_read,
		.seek_r = nitro_seek,
	},
	.fstat_r = nitro_fstat,
	.stat_r = nitro_stat,
	.link_r = nitro_link,
	.unlink_r = nitro_unlink,
	.chdir_r = nitro_chdir,
	.getcwd_r = nitro_getcwd,
	.rename_r = nitro_rename,
	.mkdir_r = nitro_mkdir,
	.diropen_r = nitro_diropen,
	.dirreset_r = nitro_dirreset,
	.dirnext_r = nitro_dirnext,
	.dirclose_r = nitro_dirclose,
	.statvfs_r = NULL,
	.ftruncate_r = nitro_ftruncate,
	.fsync_r = NULL,
	.chmod_r = nitro_chmod,
	.fchmod_r = nitro_fchmod,
	.rmdir_r = nitro_rmdir,
	.lstat_r = nitro_lstat,
	.utimes_r = nitro_utimes,
};
