/*
 * Rufus for Linux: interfaces shared by the Linux platform layer
 * Copyright © 2026 Rufus Linux port contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */
#pragma once

#include <windows.h>
#include <stdint.h>

/*
 * Drive indexes
 * -------------
 * On Windows, Rufus identifies a disk by DRIVE_INDEX_MIN (0x80) + its
 * PhysicalDrive number. On Linux, dev_linux.c assigns each whole-disk block
 * device it enumerates a slot in a table, and the drive index is
 * DRIVE_INDEX_MIN + slot. The slot of a given disk is kept stable for as long
 * as the disk stays present (hotplug of other disks doesn't change it).
 */

/* Returns the device node of a drive index (e.g. "/dev/sdb"), or NULL */
const char* linux_get_disk_path(DWORD DriveIndex);
/* Returns the kernel name of a drive index (e.g. "sdb"), or NULL */
const char* linux_get_disk_kname(DWORD DriveIndex);
/* Returns the drive index for a device node or kernel name, or 0 if unknown */
DWORD linux_get_drive_index(const char* dev_or_kname);
/*
 * Returns the device node of partition n (1-based) of a disk, accounting for
 * the "p" separator required by disks whose name ends in a digit
 * (e.g. /dev/sdb1, /dev/nvme0n1p1, /dev/mmcblk0p1, /dev/loop0p1).
 */
char* linux_get_partition_path(DWORD DriveIndex, int partition_number);

/*
 * Mounting
 * --------
 * Volumes that Rufus mounts get a private mount point under RUFUS_RUN_DIR and
 * a virtual drive letter (see linux_map_drive_letter() in windows.h).
 */
#define RUFUS_RUN_DIR "/run/rufus"
void linux_cleanup_stale_mounts(void);

/* Image files ("image|path" notation used by the WIM code), see imgmount_linux.c */
char* linux_resolve_image_path(const char* path);
void linux_release_image_mounts(void);

/* Unmount all the volumes Rufus mounted (and the images it mounted) */
void linux_release_all_mounts(void);
/* Unmount every mount of a block device (kernel name, e.g. "sdb1") */
BOOL linux_unmount_device(const char* kname, BOOL bSilent);

/* Inhibit/restore desktop automounting of a disk while we work on it (drive_linux.c) */
BOOL linux_inhibit_automount(DWORD DriveIndex, BOOL inhibit);

/* Run a program (no shell), returning its exit code or -1. Output goes to the log. */
int linux_run(const char* const argv[], BOOL log_output);
/* Run a program (no shell) and capture its standard output. TRUE if it exited with 0. */
BOOL linux_run_capture(const char* const argv[], char* out, size_t out_size);

/* Hotplug monitoring (dev_linux.c): posts UM_MEDIA_CHANGE to hMainDialog */
BOOL linux_start_device_monitor(void);
void linux_stop_device_monitor(void);
/* Process search helpers (process_linux.c) */
BOOL SearchProcessAlt(char* HandleName);
DWORD GetPPID(DWORD pid);
