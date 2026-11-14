/*
 * Rufus for Linux: wimlib wrapper
 * Copyright © 2026 Rufus Linux port contributors
 *
 * Uses the system wimlib (libwim), and provides the UTF-8 "U" helpers that
 * Rufus' bundled copy of wimlib exposes on Windows. It also adds support for
 * Rufus' "<image.iso>|<path/in/iso>" notation, which the bundled wimlib
 * implements through libcdio: on Linux the ISO is mounted read-only instead.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */
#pragma once
#ifndef RUFUS_LINUX_WIMLIB_H
#define RUFUS_LINUX_WIMLIB_H

#include_next <wimlib.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

char* wchar_to_utf8(const wchar_t* wstr);

/*
 * Resolve a path that may use the "image|inner_path" notation into a real
 * filesystem path (the image gets mounted read-only on first use).
 * Returns a newly allocated string or NULL on error.
 */
char* linux_resolve_image_path(const char* path);
/* Unmount any image that was mounted by linux_resolve_image_path() */
void linux_release_image_mounts(void);
/* Convert a WIM internal path to use '/' separators (newly allocated) */
char* linux_wim_path(const char* path);
/* Translate a (possibly drive letter based) file system path (newly allocated) */
char* linux_path(const char* path);

static inline int
wimlib_open_wimU(const char* wim_file, int open_flags, WIMStruct** wim_ret)
{
	char* path = linux_resolve_image_path(wim_file);
	int r;
	if (path == NULL)
		return WIMLIB_ERR_OPEN;
	r = wimlib_open_wim(path, open_flags, wim_ret);
	free(path);
	return r;
}

static inline int
wimlib_extract_imageU(WIMStruct* wim, int image, const char* target, int extract_flags)
{
	char* t = linux_path(target);
	int r = wimlib_extract_image(wim, image, t, extract_flags);
	free(t);
	return r;
}

static inline int
wimlib_extract_pathsU(WIMStruct* wim, int image, const char* target,
	const char* const* paths, size_t num_paths, int extract_flags)
{
	int r = WIMLIB_ERR_NOMEM;
	size_t i;
	char* t = linux_path(target);
	char** p = (char**)calloc(num_paths, sizeof(char*));
	if (p == NULL || t == NULL)
		goto out;
	for (i = 0; i < num_paths; i++)
		p[i] = linux_wim_path(paths[i]);
	r = wimlib_extract_paths(wim, image, t, (const char* const*)p, num_paths, extract_flags);
out:
	for (i = 0; p != NULL && i < num_paths; i++)
		free(p[i]);
	free(p);
	free(t);
	return r;
}

static inline int
wimlib_add_treeU(WIMStruct* wim, int image, const char* fs_source_path,
	const char* wim_target_path, int add_flags)
{
	char *s = linux_path(fs_source_path), *t = linux_wim_path(wim_target_path);
	int r = wimlib_add_tree(wim, image, s, t, add_flags);
	free(s);
	free(t);
	return r;
}

static inline int
wimlib_splitU(WIMStruct* wim, const char* swm_name, uint64_t part_size, int write_flags)
{
	char* s = linux_path(swm_name);
	int r = wimlib_split(wim, s, part_size, write_flags);
	free(s);
	return r;
}


/* Rufus' Windows code passes UTF-16 (wchar_t) paths to some of these calls */
static inline int wimlib_open_wimW(const wchar_t* wim_file, int open_flags, WIMStruct** wim_ret)
{
	char* path = wchar_to_utf8(wim_file);
	int r = (path == NULL) ? WIMLIB_ERR_NOMEM : wimlib_open_wimU(path, open_flags, wim_ret);
	free(path);
	return r;
}
static inline int wimlib_resolve_imageW(WIMStruct* wim, const wchar_t* image_name_or_num)
{
	char* s = wchar_to_utf8(image_name_or_num);
	int r = (s == NULL) ? WIMLIB_NO_IMAGE : wimlib_resolve_image(wim, s);
	free(s);
	return r;
}
#define wimlib_open_wim(path, flags, wim) _Generic((path), \
	wchar_t*: wimlib_open_wimW, const wchar_t*: wimlib_open_wimW, default: wimlib_open_wim)(path, flags, wim)
#define wimlib_resolve_image(wim, name) _Generic((name), \
	wchar_t*: wimlib_resolve_imageW, const wchar_t*: wimlib_resolve_imageW, default: wimlib_resolve_image)(wim, name)

#endif
