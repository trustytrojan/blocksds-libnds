// SPDX-License-Identifier: Zlib
//
// Copyright (C) 2023 Antonio Niño Díaz
// Copyright (C) 2023 Adrian "asie" Siekierka

#include <stdio.h>

#include <devoptab.h>

bool stdin_buf_empty = false;
// Buffers so that we can send to the console full ANSI escape sequences.
#define OUTPUT_BUFFER_SIZE 16
static char stdout_buf[OUTPUT_BUFFER_SIZE + 1];
static char stderr_buf[OUTPUT_BUFFER_SIZE + 1];
static uint16_t stdout_buf_len = 0;
static uint16_t stderr_buf_len = 0;

static int putc_buffered(char c, char *buf, uint16_t *buf_len, int fd)
{
    const devoptab_t *const device = devoptab_list[fd];
    if ((device == NULL) || (device->write_r == NULL))
        return c;

    if ((c == 0x1B) || (*buf_len > 0))
    {
        buf[*buf_len] = c;
        (*buf_len)++;
        buf[*buf_len] = 0;

        if ((*buf_len == OUTPUT_BUFFER_SIZE) || (c == '\n') || (c == '\r') ||
            ((c >= 'A') && (c <= 'Z')) || ((c >= 'a') && (c <= 'z')))
        {
            device->write_r(fd, buf, *buf_len);
            *buf_len = 0;
        }
    }
    else
    {
        device->write_r(fd, &c, 1);
    }

    return c;
}

static int stderr_putc_buffered(char c, FILE *file)
{
    (void)file;

    return putc_buffered(c, stderr_buf, &stderr_buf_len, STDERR_FILENO);
}

static int stdout_putc_buffered(char c, FILE *file)
{
    (void)file;

    if ((devoptab_list[STDOUT_FILENO] == NULL) ||
        (devoptab_list[STDOUT_FILENO]->write_r == NULL))
    {
        return stderr_putc_buffered(c, file);
    }

    return putc_buffered(c, stdout_buf, &stdout_buf_len, STDOUT_FILENO);
}

static int stdin_getc_keyboard(FILE *file)
{
    (void)file;

    const devoptab_t *const device = devoptab_list[STDIN_FILENO];
    if ((device == NULL) || (device->read_r == NULL))
        return -1;

    char c = 0;
    const ssize_t ret = device->read_r(STDIN_FILENO, &c, 1);
    if (ret <= 0)
        return -1;

    return c;
}

static FILE __stdin = FDEV_SETUP_STREAM(NULL, stdin_getc_keyboard, NULL,
                                        _FDEV_SETUP_READ);
static FILE __stdout = FDEV_SETUP_STREAM(stdout_putc_buffered, NULL, NULL,
                                         _FDEV_SETUP_WRITE);
static FILE __stderr = FDEV_SETUP_STREAM(stderr_putc_buffered, NULL, NULL,
                                         _FDEV_SETUP_WRITE);

FILE *const stdin = &__stdin;
FILE *const stdout = &__stdout;
FILE *const stderr = &__stderr;
