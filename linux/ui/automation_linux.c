/*
 * Rufus for Linux: test automation
 * Copyright © 2026 Rufus Linux port contributors
 *
 * When RUFUS_AUTOMATION is set, Rufus drives its own user interface the way a
 * user would (selects the device and options, presses START), answers its
 * dialogs automatically, and exits once the operation has completed, with the
 * outcome written to a result file. This is used by linux/tests/run-tests.sh to
 * test the complete application end to end against loop devices.
 *
 * For safety, automation refuses to run against anything but a loop device.
 *
 * RUFUS_AUTOMATION is a ';' separated list of key=value pairs:
 *   device=/dev/loopN   (mandatory) target device
 *   result=<path>       file receiving "OK" or "ERROR <code> <message>", followed by the log
 *   boot=<type>         nonbootable|msdos|freedos|image|syslinux4|syslinux6|reactos|grub4dos|grub2|uefintfs
 *   pt=<scheme>         MBR|GPT|SFD
 *   target=<system>     BIOS|UEFI
 *   fs=<file system>    FAT|FAT32|NTFS|UDF|exFAT|ext2|ext3|ext4
 *   cluster=<bytes>     cluster size
 *   label=<label>       volume label
 *   quick=0|1           quick format
 *   extended_label=0|1  create autorun.inf/autorun.ico
 *   old_bios=0|1        old BIOS fixes
 *   validation=0|1      runtime UEFI media validation
 *   badblocks=<passes>  bad blocks check (1 or 2 passes, 0 = disabled)
 *   persistence=<MB>    persistent partition size
 *   write=iso|dd|esp    answer to the ISOHybrid write mode question
 *   wue=<mask>          answer to the Windows User Experience question
 *   zero=0|1            zero the drive (Alt-Z)
 *   cancel=<ms>         press CANCEL <ms> milliseconds after START (and confirm)
 *   action=<button>     start (default), save (save the drive to an image) or hash (checksums)
 *   file=<path>         answer to the file dialogs (e.g. the image to save the drive to)
 * The image to use is given with -i, like for regular use.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */
#include <windows.h>
#include <gtk/gtk.h>

#include "rufus.h"
#include "drive.h"
#include "missing.h"
#include "resource.h"
#include "msapi_utf8.h"
#include "ui.h"
#include "settings.h"
#include "localization.h"
#include "linux_drive.h"
#include "winctl.h"
#include "automation_linux.h"

extern BOOL zero_drive, fast_zeroing;
extern HANDLE format_thread;
extern char* linux_log_get_text(void);

static int active = -1;
static char* params = NULL;
static int state = 0, exit_code = 1;
static uint64_t state_time = 0;
static int idle_ticks = 0;

static const char* get_param(const char* key, char* buf, size_t size)
{
	const char* p = params;
	size_t klen = strlen(key);
	while (p != NULL && *p != 0) {
		const char* end = strchr(p, ';');
		size_t len = (end == NULL) ? strlen(p) : (size_t)(end - p);
		if (len > klen && strncmp(p, key, klen) == 0 && p[klen] == '=') {
			snprintf(buf, size, "%.*s", (int)(len - klen - 1), &p[klen + 1]);
			return buf;
		}
		p = (end == NULL) ? NULL : end + 1;
	}
	return NULL;
}

static int get_int_param(const char* key, int def)
{
	char buf[64];
	return (get_param(key, buf, sizeof(buf)) == NULL) ? def : (int)strtol(buf, NULL, 0);
}

BOOL linux_automation_active(void)
{
	char dev[64];
	if (active < 0) {
		const char* env = getenv("RUFUS_AUTOMATION");
		active = 0;
		if (env != NULL && env[0] != 0) {
			params = strdup(env);
			/* Safety: only ever operate on loop devices */
			if (get_param("device", dev, sizeof(dev)) == NULL || strncmp(dev, "/dev/loop", 9) != 0) {
				fprintf(stderr, "RUFUS_AUTOMATION: 'device' must be a loop device - automation disabled\n");
				exit(2);
			}
			active = 1;
		}
	}
	return (active == 1);
}

int linux_automation_exit_code(void)
{
	return exit_code;
}

int linux_automation_notification(int type, const char* title, const char* text)
{
	int r;
	switch (type & MB_TYPEMASK) {
	case MB_OKCANCEL:
		r = IDOK;
		break;
	case MB_YESNO:
	case MB_YESNOCANCEL:
		r = IDYES;
		break;
	case MB_ABORTRETRYIGNORE:
		r = IDIGNORE;
		break;
	case MB_RETRYCANCEL:
		r = IDCANCEL;
		break;
	default:
		r = IDOK;
		break;
	}
	uprintf("[automation] Dialog '%s': %s => %d", (title == NULL) ? "" : title, (text == NULL) ? "" : text, r);
	return r;
}

int linux_automation_selection(const char* title, const char* message, selection_dialog_options_t* options)
{
	char buf[16];
	int r = 1;
	(void)message;
	if (options->style == BS_AUTOCHECKBOX) {
		/* Windows User Experience options */
		r = get_int_param("wue", 0);
	} else if (get_param("write", buf, sizeof(buf)) != NULL) {
		/* ISO vs DD (vs ESP) */
		int count = (int)options->choices.Index;
		if (strcmp(buf, "dd") == 0)
			r = 1 << (count - 1);
		else if (strcmp(buf, "esp") == 0 && count >= 2)
			r = 1 << 1;
		else
			r = 1;
	}
	uprintf("[automation] Selection '%s' => 0x%X", (title == NULL) ? "" : title, r);
	return r;
}

/* File dialogs: answer with the 'file' parameter, selecting the matching extension */
char* linux_automation_file_dialog(BOOL save, const ext_t* ext, UINT* selected_ext)
{
	char file[MAX_PATH];
	size_t i, len, best = 0, best_len = 0;

	if (get_param("file", file, sizeof(file)) == NULL) {
		uprintf("[automation] %s file dialog: no 'file' parameter => cancelled", save ? "Save" : "Open");
		return NULL;
	}
	len = strlen(file);
	for (i = 0; ext != NULL && i < ext->count; i++) {
		/* Extensions are of the form "*.ext": keep the longest match (".vhdx" vs ".vhd") */
		const char* e = &ext->extension[i][1];
		size_t elen = strlen(e);
		if (elen <= len && elen > best_len && strcasecmp(&file[len - elen], e) == 0) {
			best = i + 1;
			best_len = elen;
		}
	}
	if (selected_ext != NULL && best != 0)
		*selected_ext = (UINT)best;
	uprintf("[automation] %s file dialog => '%s'", save ? "Save" : "Open", file);
	return strdup(file);
}

static void fail(const char* msg)
{
	uprintf("[automation] FAILED: %s", msg);
	linux_automation_completed(RUFUS_ERROR(ERROR_GEN_FAILURE));
}

static BOOL select_combo(HWND hCtrl, int id, int data, const char* what)
{
	int i, n = ComboBox_GetCount(hCtrl);
	for (i = 0; i < n; i++) {
		if (ComboBox_GetItemData(hCtrl, i) == data) {
			if (ComboBox_GetCurSel(hCtrl) != i) {
				ComboBox_SetCurSel(hCtrl, i);
				SendMessage(hMainDialog, WM_COMMAND, MAKEWPARAM(id, CBN_SELCHANGE), (LPARAM)hCtrl);
			}
			return TRUE;
		}
	}
	uprintf("[automation] '%s' value %d is not available", what, data);
	return FALSE;
}

static void set_check(int id, const char* key)
{
	int v = get_int_param(key, -1);
	if (v >= 0) {
		CheckDlgButton(hMainDialog, id, v ? BST_CHECKED : BST_UNCHECKED);
		SendMessage(hMainDialog, WM_COMMAND, MAKEWPARAM(id, BN_CLICKED), 0);
	}
}

static int lookup(const char* value, const char* const* names, int count)
{
	int i;
	for (i = 0; i < count; i++)
		if (strcasecmp(value, names[i]) == 0)
			return i;
	return -1;
}

static BOOL apply_options(void)
{
	static const char* const boot_names[BT_MAX] = { "nonbootable", "msdos", "freedos", "image", "syslinux4",
		"syslinux6", "reactos", "grub4dos", "grub2", "uefintfs" };
	static const char* const pt_names[] = { "MBR", "GPT", "SFD" };
	static const char* const tt_names[] = { "BIOS", "UEFI" };
	extern const char* FileSystemLabel[FS_MAX];
	char buf[128];
	int v;

	if (get_param("boot", buf, sizeof(buf)) != NULL) {
		v = lookup(buf, boot_names, BT_MAX);
		if (v < 0)
			return FALSE;
		/* Some boot types are only listed in advanced mode */
		if (v >= BT_SYSLINUX_V4 && !advanced_mode_device)
			SendMessage(hMainDialog, WM_COMMAND, IDC_ADVANCED_DRIVE_PROPERTIES, 0);
		if (!select_combo(hBootType, IDC_BOOT_SELECTION, v, "boot"))
			return FALSE;
	}
	if (get_param("pt", buf, sizeof(buf)) != NULL) {
		v = lookup(buf, pt_names, ARRAYSIZE(pt_names));
		if (v < 0 || !select_combo(hPartitionScheme, IDC_PARTITION_TYPE, v, "pt"))
			return FALSE;
	}
	if (get_param("target", buf, sizeof(buf)) != NULL) {
		v = lookup(buf, tt_names, ARRAYSIZE(tt_names));
		if (v < 0 || !select_combo(hTargetSystem, IDC_TARGET_SYSTEM, v, "target"))
			return FALSE;
	}
	/* Like a user would, show the advanced format options first if they are needed
	 * (the ext file systems are only listed when these options are displayed) */
	if (!advanced_mode_format && (get_int_param("quick", -1) >= 0 || get_int_param("extended_label", -1) >= 0 ||
		get_int_param("badblocks", -1) >= 0 || (get_param("fs", buf, sizeof(buf)) != NULL && strncasecmp(buf, "ext", 3) == 0)))
		SendMessage(hMainDialog, WM_COMMAND, IDC_ADVANCED_FORMAT_OPTIONS, 0);
	if (get_param("fs", buf, sizeof(buf)) != NULL) {
		v = lookup(buf, FileSystemLabel, FS_MAX);
		if (v < 0 || !select_combo(hFileSystem, IDC_FILE_SYSTEM, v, "fs"))
			return FALSE;
	}
	v = get_int_param("cluster", 0);
	if (v != 0 && !select_combo(hClusterSize, IDC_CLUSTER_SIZE, v, "cluster"))
		return FALSE;
	if (get_param("label", buf, sizeof(buf)) != NULL)
		SetWindowTextU(hLabel, buf);
	if (get_int_param("old_bios", -1) >= 0 && !advanced_mode_device)
		SendMessage(hMainDialog, WM_COMMAND, IDC_ADVANCED_DRIVE_PROPERTIES, 0);
	set_check(IDC_OLD_BIOS_FIXES, "old_bios");
	set_check(IDC_UEFI_MEDIA_VALIDATION, "validation");
	set_check(IDC_QUICK_FORMAT, "quick");
	set_check(IDC_EXTENDED_LABEL, "extended_label");
	v = get_int_param("badblocks", 0);
	CheckDlgButton(hMainDialog, IDC_BAD_BLOCKS, (v > 0) ? BST_CHECKED : BST_UNCHECKED);
	if (v > 0)
		ComboBox_SetCurSel(GetDlgItem(hMainDialog, IDC_NB_PASSES), (v >= 2) ? 1 : 0);
	v = get_int_param("persistence", 0);
	if (v > 0) {
		persistence_unit_selection = 0;
		persistence_size = (uint64_t)v * MB;
		SetPersistenceSize();
	}
	if (get_int_param("zero", 0)) {
		zero_drive = TRUE;
		fast_zeroing = FALSE;
	}
	return TRUE;
}

static gboolean cancel_cb(gpointer data)
{
	(void)data;
	if (state == 3 && format_thread != NULL) {
		uprintf("[automation] Pressing CANCEL");
		SendMessage(hMainDialog, WM_COMMAND, IDCANCEL, 0);
	} else {
		uprintf("[automation] Nothing to cancel");
	}
	return G_SOURCE_REMOVE;
}

static gboolean automation_tick(gpointer data)
{
	char dev[64] = "";
	int i, n;
	(void)data;

	switch (state) {
	case 0:		/* Wait for the initial device enumeration and image scan */
		if (op_in_progress || format_thread != NULL) {
			idle_ticks = 0;
			break;
		}
		if (++idle_ticks < 3)
			break;
		get_param("device", dev, sizeof(dev));
		n = ComboBox_GetCount(hDeviceList);
		for (i = 0; i < n; i++) {
			const char* path = linux_get_disk_path((DWORD)ComboBox_GetItemData(hDeviceList, i));
			if (path != NULL && strcmp(path, dev) == 0)
				break;
		}
		if (i >= n) {
			if (GetTickCount64() - state_time > 20000) {
				fail("target device not listed");
				return G_SOURCE_REMOVE;
			}
			break;
		}
		if (ComboBox_GetCurSel(hDeviceList) != i) {
			ComboBox_SetCurSel(hDeviceList, i);
			SendMessage(hMainDialog, WM_COMMAND, MAKEWPARAM(IDC_DEVICE, CBN_SELCHANGE), (LPARAM)hDeviceList);
		}
		state = 1;
		idle_ticks = 0;
		break;
	case 1:		/* Set the options */
		if (op_in_progress) {
			idle_ticks = 0;
			break;
		}
		if (++idle_ticks < 2)
			break;
		if (!apply_options()) {
			fail("could not apply the requested options");
			return G_SOURCE_REMOVE;
		}
		state = 2;
		idle_ticks = 0;
		break;
	case 2:		/* Start */
		if (op_in_progress || ++idle_ticks < 2)
			break;
		get_param("action", dev, sizeof(dev));
		if (strcmp(dev, "save") == 0 || strcmp(dev, "hash") == 0) {
			BOOL is_save = (strcmp(dev, "save") == 0);
			if (is_save && (ComboBox_GetCurSel(hDeviceList) < 0 ||
				!linux_automation_check_drive((DWORD)ComboBox_GetItemData(hDeviceList, ComboBox_GetCurSel(hDeviceList)))))
				return G_SOURCE_REMOVE;
			uprintf("[automation] Pressing %s", is_save ? "SAVE" : "HASH");
			state = 3;
			SendMessage(hMainDialog, WM_COMMAND, is_save ? IDC_SAVE : IDC_HASH, 0);
			return G_SOURCE_REMOVE;
		}
		/* Like Alt-Z on Windows, zeroing the drive doesn't depend on the state of the START button */
		if (!zero_drive && !IsWindowEnabled(GetDlgItem(hMainDialog, IDC_START))) {
			if (idle_ticks > 30) {
				fail("the START button is disabled");
				return G_SOURCE_REMOVE;
			}
			break;
		}
		if (ComboBox_GetCurSel(hDeviceList) < 0 ||
			!linux_automation_check_drive((DWORD)ComboBox_GetItemData(hDeviceList, ComboBox_GetCurSel(hDeviceList))))
			return G_SOURCE_REMOVE;
		uprintf("[automation] Pressing START");
		SendMessage(hMainDialog, WM_COMMAND, IDC_START, 0);
		state = 3;
		if (get_int_param("cancel", 0) > 0)
			g_timeout_add(get_int_param("cancel", 0), cancel_cb, NULL);
		return G_SOURCE_REMOVE;
	default:
		return G_SOURCE_REMOVE;
	}
	return G_SOURCE_CONTINUE;
}

/* Make sure that the drive about to be written is the requested loop device */
BOOL linux_automation_check_drive(DWORD DriveIndex)
{
	char dev[64];
	const char* path;

	if (!linux_automation_active())
		return TRUE;
	path = linux_get_disk_path(DriveIndex);
	get_param("device", dev, sizeof(dev));
	if (path == NULL || strcmp(path, dev) != 0 || strncmp(path, "/dev/loop", 9) != 0) {
		uprintf("[automation] Refusing to write to '%s' (expected '%s')", (path == NULL) ? "?" : path, dev);
		fail("wrong target device");
		return FALSE;
	}
	return TRUE;
}

void linux_automation_start(void)
{
	if (!linux_automation_active())
		return;
	uprintf("[automation] Started: %s", params);
	state = 0;
	state_time = GetTickCount64();
	g_timeout_add(300, automation_tick, NULL);
}

static gboolean quit_cb(gpointer data)
{
	(void)data;
	SendMessage(hMainDialog, WM_COMMAND, IDCANCEL, 0);
	return G_SOURCE_REMOVE;
}

/* Called when an operation completes (UM_FORMAT_COMPLETED), with the final status */
void linux_automation_completed(DWORD status)
{
	char path[MAX_PATH];
	FILE* f;
	char* log;

	if (!linux_automation_active() || state < 0)
		return;
	state = -1;
	exit_code = IS_ERROR(status) ? 1 : 0;
	uprintf("[automation] Completed with status 0x%08X", status);
	if (get_param("result", path, sizeof(path)) != NULL) {
		f = fopen(path, "w");
		if (f != NULL) {
			if (IS_ERROR(status))
				fprintf(f, "ERROR 0x%08X %s\n", status, StrError(status, TRUE));
			else
				fprintf(f, "OK\n");
			log = linux_log_get_text();
			if (log != NULL) {
				fputs(log, f);
				free(log);
			}
			fclose(f);
		}
	}
	/* Quit once the completion has been fully processed */
	g_timeout_add(500, quit_cb, NULL);
}
