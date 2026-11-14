/*
 * Rufus for Linux: internal declarations shared by the compatibility layer
 * Copyright © 2026 Rufus Linux port contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */
#pragma once

#include <windows.h>

/* Win32 error codes that windows.h doesn't need to expose */
#define ERROR_NOT_SAME_DEVICE_COMPAT          17L
#define ERROR_NO_UNICODE_TRANSLATION_COMPAT   1113L

static inline void to_unix_path_inplace(char* p)
{
	for (; p != NULL && *p != 0; p++)
		if (*p == '\\')
			*p = '/';
}

/* Implemented in linux/core */
BOOL linux_get_volume_information(const char* root, char* label, DWORD label_size,
	DWORD* serial, DWORD* max_component, DWORD* fs_flags, char* fs_name, DWORD fs_name_size);
/* Name of the (unprivileged) user who launched us through pkexec/sudo */
const char* linux_invoking_user_name(void);
/* Home directory of that user */
const char* linux_invoking_user_home(void);
/* uid/gid of that user (to chown the files we create on their behalf) */
void linux_invoking_user_ids(uid_t* uid, gid_t* gid);
/* Windows LCID for a POSIX locale name (NULL = from the environment) */
uint16_t linux_locale_to_lcid(const char* locale);
