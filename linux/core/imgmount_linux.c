/*
 * Rufus for Linux: access to files located inside disk images
 * Copyright © 2026 Rufus Linux port contributors
 *
 * Rufus refers to files inside an ISO with the "<image.iso>|<path>" notation,
 * e.g. to read the version of "sources/install.wim" while scanning a Windows
 * ISO. On Windows, its bundled wimlib reads such files through libcdio. On
 * Linux, the image gets mounted read-only through a loop device instead (we
 * run as root), and the notation is translated to a regular path.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */
#include <windows.h>
#include <msapi_utf8_linux.h>
#include <dirent.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <linux/loop.h>

#include "rufus.h"
#include "linux_drive.h"

#define MAX_IMAGE_MOUNTS 4

static struct {
	char* image;
	char* mount_point;
} image_mounts[MAX_IMAGE_MOUNTS];
static pthread_mutex_t mount_lock = PTHREAD_MUTEX_INITIALIZER;

/* Attach an image to a read-only, auto-clearing loop device */
static int attach_loop(const char* image, char* loop_dev, size_t size)
{
	struct loop_config cfg;
	int ctl, num, img_fd = -1, loop_fd = -1, r = -1;

	ctl = open("/dev/loop-control", O_RDWR | O_CLOEXEC);
	if (ctl < 0)
		return -1;
	img_fd = open(image, O_RDONLY | O_CLOEXEC);
	if (img_fd < 0)
		goto out;
	for (int retries = 0; retries < 8; retries++) {
		num = ioctl(ctl, LOOP_CTL_GET_FREE);
		if (num < 0)
			goto out;
		snprintf(loop_dev, size, "/dev/loop%d", num);
		loop_fd = open(loop_dev, O_RDONLY | O_CLOEXEC);
		if (loop_fd < 0)
			goto out;
		memset(&cfg, 0, sizeof(cfg));
		cfg.fd = (uint32_t)img_fd;
		cfg.info.lo_flags = LO_FLAGS_READ_ONLY | LO_FLAGS_AUTOCLEAR;
		snprintf((char*)cfg.info.lo_file_name, sizeof(cfg.info.lo_file_name), "%s", image);
		if (ioctl(loop_fd, LOOP_CONFIGURE, &cfg) == 0) {
			r = 0;
			goto out;
		}
		close(loop_fd);
		loop_fd = -1;
		if (errno != EBUSY)
			goto out;
	}
out:
	if (r != 0 && loop_fd >= 0) {
		close(loop_fd);
		loop_fd = -1;
	}
	if (img_fd >= 0)
		close(img_fd);
	close(ctl);
	/* NB: the loop device is kept open, so that autoclear only happens on unmount */
	return (r == 0) ? loop_fd : -1;
}

static const char* mount_image(const char* image)
{
	static const char* fs_types[] = { "udf", "iso9660" };
	char loop_dev[32], mnt[64];
	int i, slot = -1, loop_fd;
	size_t j;

	for (i = 0; i < MAX_IMAGE_MOUNTS; i++) {
		if (image_mounts[i].image != NULL && strcmp(image_mounts[i].image, image) == 0)
			return image_mounts[i].mount_point;
		if (image_mounts[i].image == NULL && slot < 0)
			slot = i;
	}
	if (slot < 0) {
		/* Recycle the oldest slot */
		umount2(image_mounts[0].mount_point, MNT_DETACH);
		free(image_mounts[0].image);
		free(image_mounts[0].mount_point);
		memmove(&image_mounts[0], &image_mounts[1], sizeof(image_mounts[0]) * (MAX_IMAGE_MOUNTS - 1));
		slot = MAX_IMAGE_MOUNTS - 1;
		image_mounts[slot].image = NULL;
	}
	mkdir(RUFUS_RUN_DIR, 0700);
	snprintf(mnt, sizeof(mnt), "%s/image%d", RUFUS_RUN_DIR, slot);
	mkdir(mnt, 0700);
	loop_fd = attach_loop(image, loop_dev, sizeof(loop_dev));
	if (loop_fd < 0) {
		uprintf("Could not attach '%s' to a loop device: %s", image, strerror(errno));
		return NULL;
	}
	for (j = 0; j < ARRAYSIZE(fs_types); j++) {
		if (mount(loop_dev, mnt, fs_types[j], MS_RDONLY | MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL) == 0)
			break;
	}
	/* Once mounted, the mount keeps the loop device busy */
	close(loop_fd);
	if (j >= ARRAYSIZE(fs_types)) {
		uprintf("Could not mount image '%s': %s", image, strerror(errno));
		rmdir(mnt);
		return NULL;
	}
	image_mounts[slot].image = strdup(image);
	image_mounts[slot].mount_point = strdup(mnt);
	return image_mounts[slot].mount_point;
}

/* Resolve each path component case-insensitively, as on Windows' file systems */
static char* resolve_case_insensitive(const char* root, const char* rel)
{
	char *path, *copy, *tok, *save = NULL;
	size_t len;
	DIR* d;
	struct dirent* de;
	BOOL found;

	path = strdup(root);
	copy = strdup(rel);
	if (path == NULL || copy == NULL)
		goto fail;
	for (tok = strtok_r(copy, "/\\", &save); tok != NULL; tok = strtok_r(NULL, "/\\", &save)) {
		found = FALSE;
		d = opendir(path);
		if (d != NULL) {
			while ((de = readdir(d)) != NULL) {
				if (strcasecmp(de->d_name, tok) == 0) {
					tok = de->d_name;
					found = TRUE;
					break;
				}
			}
		}
		len = strlen(path) + strlen(tok) + 2;
		{
			char* np = malloc(len);
			if (np == NULL) {
				if (d != NULL)
					closedir(d);
				goto fail;
			}
			snprintf(np, len, "%s/%s", path, tok);
			free(path);
			path = np;
		}
		if (d != NULL)
			closedir(d);
		(void)found;
	}
	free(copy);
	return path;
fail:
	free(path);
	free(copy);
	return NULL;
}

char* linux_resolve_image_path(const char* path)
{
	const char* sep;
	char *image, *r = NULL;
	const char* mnt;

	if (path == NULL)
		return NULL;
	sep = strchr(path, '|');
	if (sep == NULL)
		return linux_path(path);
	image = linux_path(path);
	if (image == NULL)
		return NULL;
	image[sep - path] = 0;
	pthread_mutex_lock(&mount_lock);
	mnt = mount_image(image);
	if (mnt != NULL)
		r = resolve_case_insensitive(mnt, &sep[1]);
	pthread_mutex_unlock(&mount_lock);
	free(image);
	return r;
}

void linux_release_image_mounts(void)
{
	int i;
	pthread_mutex_lock(&mount_lock);
	for (i = 0; i < MAX_IMAGE_MOUNTS; i++) {
		if (image_mounts[i].mount_point == NULL)
			continue;
		if (umount2(image_mounts[i].mount_point, 0) != 0)
			umount2(image_mounts[i].mount_point, MNT_DETACH);
		rmdir(image_mounts[i].mount_point);
		free(image_mounts[i].image);
		free(image_mounts[i].mount_point);
		image_mounts[i].image = NULL;
		image_mounts[i].mount_point = NULL;
	}
	pthread_mutex_unlock(&mount_lock);
}
