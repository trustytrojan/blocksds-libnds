// SPDX-License-Identifier: Zlib
//
// Copyright (C) 2026 trustytrojan

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "devoptab.h"

#define DIR DIRff
#include "ff.h"
#undef DIR

#include "diskio.h"

#include "fatfs_internal.h"
#include "filesystem_internal.h"

static inline FIL *fat_fd_to_fil(int fd)
{
	return FD_FAT_UNPACK(fd);
}

static int fat_open(const char *path, int flags)
{
	if (path == NULL)
	{
		errno = EINVAL;
		return -1;
	}

	BYTE mode = 0;
	int can_write = 0;

	switch (flags & (O_RDONLY | O_WRONLY | O_RDWR))
	{
		case O_RDONLY:
			mode = FA_READ;
			break;
		case O_WRONLY:
			can_write = 1;
			mode = FA_WRITE;
			break;
		case O_RDWR:
			can_write = 1;
			mode = FA_READ | FA_WRITE;
			break;
		default:
			errno = EINVAL;
			return -1;
	}

	if (can_write)
	{
		if (flags & O_CREAT)
		{
			if (flags & O_APPEND)
			{
				mode |= FA_OPEN_APPEND;
			}
			else if (flags & O_TRUNC)
			{
				if (flags & O_EXCL)
					mode |= FA_CREATE_NEW;
				else
					mode |= FA_CREATE_ALWAYS;
			}
			else
			{
				errno = EINVAL;
				return -1;
			}
		}
		else
		{
			mode |= FA_OPEN_EXISTING;
		}
	}
	else
	{
		mode |= FA_OPEN_EXISTING;
	}

	FIL *const fp = calloc(1, sizeof(FIL));
	if (fp == NULL)
	{
		errno = ENOMEM;
		return -1;
	}

	const FRESULT result = f_open(fp, path, mode);
	if (result == FR_OK)
		return FD_FAT_PACK(fp);

	free(fp);
	errno = fatfs_error_to_posix(result);
	return -1;
}

static int fat_close(int fd)
{
	FIL *const fp = fat_fd_to_fil(fd);

	const FRESULT result = f_close(fp);
	if (fp->cltbl != NULL)
		free(fp->cltbl);
	free(fp);

	if (result == FR_OK)
		return 0;

	errno = fatfs_error_to_posix(result);
	return -1;
}

static ssize_t fat_write(int fd, const char *ptr, size_t len)
{
	if (ptr == NULL)
	{
		errno = EINVAL;
		return -1;
	}

	FIL *const fp = fat_fd_to_fil(fd);
	UINT bytes_written = 0;

	const FRESULT result = f_write(fp, ptr, len, &bytes_written);
	if (result == FR_OK)
		return bytes_written;

	errno = fatfs_error_to_posix(result);
	return -1;
}

static ssize_t fat_read(int fd, char *ptr, size_t len)
{
	if (ptr == NULL)
	{
		errno = EINVAL;
		return -1;
	}

	FIL *const fp = fat_fd_to_fil(fd);
	UINT bytes_read = 0;

	const FRESULT result = f_read(fp, ptr, len, &bytes_read);
	if (result == FR_OK)
		return bytes_read;

	errno = fatfs_error_to_posix(result);
	return -1;
}

static off_t fat_seek(int fd, off_t offset, int whence)
{
	FIL *const fp = fat_fd_to_fil(fd);

	if (whence == SEEK_END)
	{
		whence = SEEK_SET;
		offset += f_size(fp);
	}
	else if (whence == SEEK_CUR)
	{
		whence = SEEK_SET;
		offset += f_tell(fp);
	}
	else if (whence != SEEK_SET)
	{
		errno = EINVAL;
		return -1;
	}

	const FRESULT result = f_lseek(fp, offset);
	if (result == FR_OK)
		return offset;

	errno = fatfs_error_to_posix(result);
	return -1;
}

static int fat_fstat(int fd, struct stat *st)
{
	if (st == NULL)
	{
		errno = EINVAL;
		return -1;
	}

	FIL *const fp = fat_fd_to_fil(fd);

	st->st_dev = fp->obj.fs->pdrv;
	st->st_ino = fp->obj.sclust;
	st->st_size = fp->obj.objsize;

#if FF_MAX_SS != FF_MIN_SS
#error "Set the block size to the right value"
#endif
	st->st_blksize = FF_MAX_SS;
	st->st_blocks = (fp->obj.objsize + FF_MAX_SS - 1) / FF_MAX_SS;
	st->st_mode = S_IFREG;
	st->st_atim.tv_sec = 0;
	st->st_mtim.tv_sec = 0;
	st->st_ctim.tv_sec = 0;

	return 0;
}

static int fat_stat(const char *path, struct stat *st)
{
	if ((path == NULL) || (st == NULL))
	{
		errno = EINVAL;
		return -1;
	}

	FILINFO fno = { 0 };
	const FRESULT result = f_stat(path, &fno);
	if (result != FR_OK)
	{
		errno = fatfs_error_to_posix(result);
		return -1;
	}

	st->st_dev = fno.fpdrv;
	st->st_ino = fno.fclust;
	st->st_size = fno.fsize;

#if FF_MAX_SS != FF_MIN_SS
#error "Set the block size to the right value"
#endif
	st->st_blksize = FF_MAX_SS;
	st->st_blocks = (fno.fsize + FF_MAX_SS - 1) / FF_MAX_SS;
	st->st_mode = (fno.fattrib & AM_DIR) ? S_IFDIR : S_IFREG;

	time_t time = fatfs_fattime_to_timestamp(fno.fdate, fno.ftime);
	time_t crtime = fatfs_fattime_to_timestamp(fno.crdate, fno.crtime);
	st->st_atim.tv_sec = time;
	st->st_mtim.tv_sec = time;
	st->st_ctim.tv_sec = crtime;

	return 0;
}

static int fat_link(const char *oldName, const char *newName)
{
	(void)oldName;
	(void)newName;
	errno = EMLINK;
	return -1;
}

static int fat_unlink(const char *name)
{
	const FRESULT result = f_unlink(name);
	if (result == FR_OK)
		return 0;

	errno = fatfs_error_to_posix(result);
	return -1;
}

static int fat_chdir(const char *name)
{
	const FRESULT result = f_chdir(name);
	if (result == FR_OK)
		return 0;

	errno = fatfs_error_to_posix(result);
	return -1;
}

static char *fat_getcwd(char *buf, size_t size)
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

		char *const result = fat_getcwd(buf, size);
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

	const FRESULT result = f_getcwd(buf, size - 1);
	if (result != FR_OK)
	{
		errno = result == FR_NOT_ENOUGH_CORE ? ERANGE : fatfs_error_to_posix(result);
		return NULL;
	}

	buf[size - 1] = '\0';
	return buf;
}

static char *fat_getwd(char *buf)
{
	return fat_getcwd(buf, PATH_MAX);
}

static char *fat_get_current_dir_name(void)
{
	return fat_getcwd(NULL, 0);
}

static int fat_rename(const char *oldName, const char *newName)
{
	const FRESULT result = f_rename(oldName, newName);
	if (result == FR_OK)
		return 0;

	errno = fatfs_error_to_posix(result);
	return -1;
}

static int fat_mkdir(const char *path, int mode)
{
	(void)mode;

	const FRESULT result = f_mkdir(path);
	if (result == FR_OK)
		return 0;

	errno = fatfs_error_to_posix(result);
	return -1;
}

static DIR *fat_diropen(DIR **dirState, const char *path)
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

	DIRff *const state = calloc(1, sizeof(DIRff));
	if (state == NULL)
	{
		free(dir);
		errno = ENOMEM;
		return NULL;
	}

	const FRESULT result = f_opendir(state, path);
	if (result != FR_OK)
	{
		free(state);
		free(dir);
		errno = fatfs_error_to_posix(result);
		return NULL;
	}

	dir->dp = state;
	dir->index = -1;
	dir->dptype = FD_TYPE_FAT;
	*dirState = dir;
	return dir;
}

static int fat_dirreset(DIR *dirState)
{
	if ((dirState == NULL) || (dirState->dp == NULL))
	{
		errno = EBADF;
		return -1;
	}

	const FRESULT result = f_rewinddir((DIRff *)dirState->dp);
	if (result != FR_OK)
	{
		errno = fatfs_error_to_posix(result);
		return -1;
	}

	dirState->index = -1;
	return 0;
}

static int fat_dirnext(DIR *dirState, char *filename, struct stat *filestat)
{
	if ((dirState == NULL) || (dirState->dp == NULL) || (filename == NULL))
	{
		errno = EINVAL;
		return -1;
	}

	FILINFO fno = { 0 };
	const FRESULT result = f_readdir((DIRff *)dirState->dp, &fno);
	if (result != FR_OK)
	{
		errno = fatfs_error_to_posix(result);
		return -1;
	}

	if (fno.fname[0] == '\0')
	{
		errno = ENOENT;
		return -1;
	}

	strcpy(filename, fno.fname);
	dirState->index++;

	if (filestat != NULL)
	{
		memset(filestat, 0, sizeof(*filestat));
		filestat->st_ino = fno.fclust;
		filestat->st_size = fno.fsize;
		filestat->st_mode = (fno.fattrib & AM_DIR) ? S_IFDIR : S_IFREG;
	}

	return 0;
}

static int fat_dirclose(DIR *dirState)
{
	if (dirState == NULL)
	{
		errno = EBADF;
		return -1;
	}

	int ret = 0;
	if (dirState->dp != NULL)
	{
		const FRESULT result = f_closedir((DIRff *)dirState->dp);
		if (result != FR_OK)
		{
			errno = fatfs_error_to_posix(result);
			ret = -1;
		}
		free(dirState->dp);
	}

	free(dirState);
	return ret;
}

static int fat_statvfs(const char *path, struct statvfs *buf)
{
	FATFS *fs;
	DWORD nclst = 0;
	const FRESULT result = f_getfree(path, &nclst, &fs);
	if ((result != FR_OK) || (fs == NULL))
	{
		errno = EIO;
		return -1;
	}

	const uint8_t status = disk_status(fs->pdrv);

#if FF_MAX_SS != FF_MIN_SS
	buf->f_bsize = fs->csize * fs->ssize;
#else
	buf->f_bsize = fs->csize * FF_MAX_SS;
#endif
	buf->f_frsize = buf->f_bsize;
	buf->f_blocks = fs->n_fatent - 2;
	buf->f_bfree = nclst;
	buf->f_bavail = nclst;
	buf->f_files = 0;
	buf->f_ffree = 0;
	buf->f_favail = 0;
	buf->f_fsid = fs->fs_type;
	buf->f_flag = (FF_FS_READONLY || (status & STA_PROTECT)) ? ST_RDONLY : 0;
	buf->f_namemax = fs->fs_type >= FS_FAT32 ? 255 : 12;

	return 0;
}

static int fat_ftruncate_internal(FIL *fp, off_t length)
{
	const FSIZE_t fsize = f_size(fp);

	if ((size_t)length > (size_t)fsize)
	{
		FRESULT result = f_lseek(fp, fsize);
		if (result != FR_OK)
		{
			errno = fatfs_error_to_posix(result);
			return -1;
		}

		FSIZE_t size_diff = length - fsize;
		const char zeroes[128] = { 0 };

		while (size_diff > 128)
		{
			UINT bytes_written = 0;
			result = f_write(fp, zeroes, 128, &bytes_written);
			if ((result != FR_OK) || (bytes_written != 128))
			{
				errno = fatfs_error_to_posix(result);
				return -1;
			}
			size_diff -= 128;
		}

		if (size_diff > 0)
		{
			UINT bytes_written = 0;
			result = f_write(fp, zeroes, size_diff, &bytes_written);
			if ((result != FR_OK) || (bytes_written != size_diff))
			{
				errno = fatfs_error_to_posix(result);
				return -1;
			}
		}
	}
	else
	{
		FRESULT result = f_lseek(fp, length);
		if (result != FR_OK)
		{
			errno = fatfs_error_to_posix(result);
			return -1;
		}

		result = f_truncate(fp);
		if (result != FR_OK)
		{
			errno = fatfs_error_to_posix(result);
			return -1;
		}
	}

	return 0;
}

static int fat_ftruncate(int fd, off_t len)
{
	FIL *const fp = fat_fd_to_fil(fd);

	if ((size_t)len == (size_t)f_size(fp))
		return 0;

	const FSIZE_t prev_offset = f_tell(fp);
	const int truncate_ret = fat_ftruncate_internal(fp, len);
	const int truncate_errno = errno;

	const off_t seek_ret = fat_seek(fd, prev_offset, SEEK_SET);
	if (truncate_ret != 0)
	{
		errno = truncate_errno;
		return -1;
	}

	if (seek_ret != (off_t)prev_offset)
		return -1;

	return 0;
}

static int fat_fsync(int fd)
{
	const FRESULT result = f_sync(fat_fd_to_fil(fd));
	if (result == FR_OK)
		return 0;

	errno = fatfs_error_to_posix(result);
	return -1;
}

static int fat_rmdir(const char *name)
{
	const FRESULT result = f_rmdir(name);
	if (result == FR_OK)
		return 0;

	errno = fatfs_error_to_posix(result);
	return -1;
}

static int fat_lstat(const char *file, struct stat *st)
{
	return fat_stat(file, st);
}

static int fat_utimes(const char *filename, const struct timeval times[2])
{
	if ((filename == NULL) || (times == NULL))
	{
		errno = EINVAL;
		return -1;
	}

	FILINFO fno;
	struct tm *const modtime = localtime(&times[1].tv_sec);
	const uint32_t modstamp = fatfs_timestamp_to_fattime(modtime);
	fno.ftime = modstamp;
	fno.fdate = modstamp >> 16;

	const FRESULT result = f_utime(filename, &fno);
	if (result == FR_OK)
		return 0;

	errno = fatfs_error_to_posix(result);
	return -1;
}

const devoptab_posix_t dot_fatfs = {
	.dot = {
		.name = "fat",
		.flags = DEVOPTAB_IS_POSIX,
		.open_r = fat_open,
		.close_r = fat_close,
		.write_r = fat_write,
		.read_r = fat_read,
		.seek_r = fat_seek,
	},
	.fstat_r = fat_fstat,
	.stat_r = fat_stat,
	.link_r = fat_link,
	.unlink_r = fat_unlink,
	.chdir_r = fat_chdir,
	.getcwd_r = fat_getcwd,
	.getwd_r = fat_getwd,
	.get_current_dir_name_r = fat_get_current_dir_name,
	.rename_r = fat_rename,
	.mkdir_r = fat_mkdir,
	.diropen_r = fat_diropen,
	.dirreset_r = fat_dirreset,
	.dirnext_r = fat_dirnext,
	.dirclose_r = fat_dirclose,
	.statvfs_r = fat_statvfs,
	.ftruncate_r = fat_ftruncate,
	.fsync_r = fat_fsync,
	.chmod_r = NULL,
	.fchmod_r = NULL,
	.rmdir_r = fat_rmdir,
	.lstat_r = fat_lstat,
	.utimes_r = fat_utimes,
};

const devoptab_posix_t dot_sd = {
	.dot = {
		.name = "sd",
		.flags = DEVOPTAB_IS_POSIX,
		.open_r = fat_open,
		.close_r = fat_close,
		.write_r = fat_write,
		.read_r = fat_read,
		.seek_r = fat_seek,
	},
	.fstat_r = fat_fstat,
	.stat_r = fat_stat,
	.link_r = fat_link,
	.unlink_r = fat_unlink,
	.chdir_r = fat_chdir,
	.getcwd_r = fat_getcwd,
	.getwd_r = fat_getwd,
	.get_current_dir_name_r = fat_get_current_dir_name,
	.rename_r = fat_rename,
	.mkdir_r = fat_mkdir,
	.diropen_r = fat_diropen,
	.dirreset_r = fat_dirreset,
	.dirnext_r = fat_dirnext,
	.dirclose_r = fat_dirclose,
	.statvfs_r = fat_statvfs,
	.ftruncate_r = fat_ftruncate,
	.fsync_r = fat_fsync,
	.chmod_r = NULL,
	.fchmod_r = NULL,
	.rmdir_r = fat_rmdir,
	.lstat_r = fat_lstat,
	.utimes_r = fat_utimes,
};

const devoptab_posix_t dot_nand = {
	.dot = {
		.name = "nand",
		.flags = DEVOPTAB_IS_POSIX,
		.open_r = fat_open,
		.close_r = fat_close,
		.write_r = fat_write,
		.read_r = fat_read,
		.seek_r = fat_seek,
	},
	.fstat_r = fat_fstat,
	.stat_r = fat_stat,
	.link_r = fat_link,
	.unlink_r = fat_unlink,
	.chdir_r = fat_chdir,
	.getcwd_r = fat_getcwd,
	.getwd_r = fat_getwd,
	.get_current_dir_name_r = fat_get_current_dir_name,
	.rename_r = fat_rename,
	.mkdir_r = fat_mkdir,
	.diropen_r = fat_diropen,
	.dirreset_r = fat_dirreset,
	.dirnext_r = fat_dirnext,
	.dirclose_r = fat_dirclose,
	.statvfs_r = fat_statvfs,
	.ftruncate_r = fat_ftruncate,
	.fsync_r = fat_fsync,
	.chmod_r = NULL,
	.fchmod_r = NULL,
	.rmdir_r = fat_rmdir,
	.lstat_r = fat_lstat,
	.utimes_r = fat_utimes,
};

const devoptab_posix_t dot_nand2 = {
	.dot = {
		.name = "nand2",
		.flags = DEVOPTAB_IS_POSIX,
		.open_r = fat_open,
		.close_r = fat_close,
		.write_r = fat_write,
		.read_r = fat_read,
		.seek_r = fat_seek,
	},
	.fstat_r = fat_fstat,
	.stat_r = fat_stat,
	.link_r = fat_link,
	.unlink_r = fat_unlink,
	.chdir_r = fat_chdir,
	.getcwd_r = fat_getcwd,
	.getwd_r = fat_getwd,
	.get_current_dir_name_r = fat_get_current_dir_name,
	.rename_r = fat_rename,
	.mkdir_r = fat_mkdir,
	.diropen_r = fat_diropen,
	.dirreset_r = fat_dirreset,
	.dirnext_r = fat_dirnext,
	.dirclose_r = fat_dirclose,
	.statvfs_r = fat_statvfs,
	.ftruncate_r = fat_ftruncate,
	.fsync_r = fat_fsync,
	.chmod_r = NULL,
	.fchmod_r = NULL,
	.rmdir_r = fat_rmdir,
	.lstat_r = fat_lstat,
	.utimes_r = fat_utimes,
};
