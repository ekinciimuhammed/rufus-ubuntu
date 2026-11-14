/*
 * Rufus for Linux: test automation (see automation_linux.c)
 * Copyright © 2026 Rufus Linux port contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */
#pragma once

#include <windows.h>
#include "rufus.h"

BOOL linux_automation_active(void);
int linux_automation_notification(int type, const char* title, const char* text);
int linux_automation_selection(const char* title, const char* message, selection_dialog_options_t* options);
void linux_automation_start(void);
void linux_automation_completed(DWORD status);
int linux_automation_exit_code(void);
BOOL linux_automation_check_drive(DWORD DriveIndex);
char* linux_automation_file_dialog(BOOL save, const ext_t* ext, UINT* selected_ext);
