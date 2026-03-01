// SPDX-License-Identifier: Zlib
//
// Copyright (C) 2026 trustytrojan

#include <devoptab.h>

static int defaultDevice = -1;

static ssize_t null_write(int fd, const char *ptr, size_t len)
{
	(void)fd;
	(void)ptr;
	return len;
}

const devoptab_t dot_null = {
	.name = "null",
	.write_r = null_write,
};

// Fill in the standard streams with `dot_null` so that they can never be
// taken by AddDevice().
const devoptab_t *devoptab_list[MAX_DEVICES] = {
	&dot_null,
	&dot_null,
	&dot_null,
};

int AddDevice(const devoptab_t *const device)
{
	if ((device == NULL) || (device->name == NULL))
		return -1;

	int idx = 3;
	for (; idx < MAX_DEVICES; ++idx)
	{
		if (devoptab_list[idx] == NULL)
			break;

		if (strcmp(devoptab_list[idx]->name, device->name) == 0)
			break;
	}

	if (idx == MAX_DEVICES)
		return -1;

	devoptab_list[idx] = device;

	if (defaultDevice < 0)
		defaultDevice = idx;

	return idx;
}

int RemoveDevice(const char *name)
{
	const int index = FindDevice(name);
	if (index < 0)
		return -1;

	devoptab_list[index] = NULL;

	if (defaultDevice == index)
		defaultDevice = -1;

	return 0;
}

const devoptab_t *GetDevice(int index)
{
	if ((index < 0) || (index >= MAX_DEVICES))
		return NULL;

	return devoptab_list[index];
}

int SetDefaultDevice(int index)
{
	if ((index < 0) || (index >= MAX_DEVICES) || (devoptab_list[index] == NULL))
		return -1;

	defaultDevice = index;
	return 0;
}

int GetDefaultDevice(void)
{
	return defaultDevice;
}

int FindDevice(const char *const name)
{
	if (name == NULL)
		return -1;

	const char *const separator = strchr(name, ':');
	if (separator == NULL)
		return defaultDevice;

	const int dev_namelen = separator - name;
	for (int index = 0; index < MAX_DEVICES; ++index)
	{
		if (devoptab_list[index] == NULL)
			continue;

		const int namelen = strlen(devoptab_list[index]->name);
		if ((dev_namelen == namelen) && (strncmp(devoptab_list[index]->name, name, namelen) == 0))
			return index;
	}

	return -1;
}
