// SPDX-License-Identifier: Zlib
//
// Copyright (C) 2023 Adrian "asie" Siekierka

#include <sys/time.h>
#include <utime.h>
#include <stddef.h>

int lutimes(const char *filename, const struct timeval times[2])
{
    // FAT does not implement symbolic links; forward to utimes().
    return utimes(filename, times);
}

int utime(const char *filename, const struct utimbuf *times)
{
    if (times == NULL)
        return -1;

    // Forward to utimes().
    struct timeval otimes[2];
    otimes[1].tv_sec = times->modtime;
    return utimes(filename, otimes);
}
