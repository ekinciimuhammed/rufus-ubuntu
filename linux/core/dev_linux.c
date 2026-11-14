/*
 * Rufus for Linux: device detection and enumeration
 * Copyright © 2014-2026 Pete Batard <pete@akeo.ie>
 * Copyright © 2026 Rufus Linux port contributors
 *
 * This is the Linux counterpart of src/dev.c. The enumeration rules (which
 * devices get listed or eliminated, and why) as well as the log messages are
 * the same as on Windows. Only the way the information is obtained differs:
 * - SetupAPI/CfgMgr -> /sys/block and /sys/bus/usb
 * - Hub IOCTLs (VID:PID, speed) -> USB device attributes in sysfs
 * - Drive letters -> the kernel name of the disk (e.g. "sdb")
 * - WM_DEVICECHANGE -> a NETLINK_KOBJECT_UEVENT monitor thread
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <assert.h>
#include <dirent.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <linux/fs.h>
#include <linux/netlink.h>

#include "rufus.h"
#include "missing.h"
#include "resource.h"
#include "settings.h"
#include "msapi_utf8.h"
#include "localization.h"
#include "drive.h"
#include "linux_drive.h"

/* From src/dev.h (which is otherwise all Windows DDK definitions) */
#define USB_SPEED_UNKNOWN			0
#define USB_SPEED_LOW_1_5M			1	// USB 1.0
#define USB_SPEED_FULL_12M			2	// USB 1.1
#define USB_SPEED_HIGH_480M			3	// USB 2.0
#define USB_SPEED_SUPER_5G			4	// USB 3.0
#define USB_SPEED_SUPER_10G			5	// USB 3.1
#define USB_SPEED_SUPER_20G			6	// USB 3.2
#define USB_SPEED_SUPER_40G			7	// USB 4.0
#define USB_SPEED_SUPER_80G			8	// USB 4.1
#define USB_SPEED_MAX				9

typedef struct usb_device_props {
	int32_t  vid;
	int32_t  pid;
	uint32_t  current_speed;
	uint32_t  max_speed;
	uint32_t  port;
	BOOLEAN   is_USB;
	BOOLEAN   is_SCSI;
	BOOLEAN   is_CARD;
	BOOLEAN   is_UASP;
	BOOLEAN   is_VHD;
	BOOLEAN   is_Removable;
} usb_device_props;

#ifndef WM_NEXTDLGCTL
#define WM_NEXTDLGCTL 0x0028
#endif

extern RUFUS_DRIVE rufus_drive[MAX_DRIVES];
extern BOOL enable_HDDs, enable_VHDs, use_fake_units, enable_vmdk, usb_debug;
extern BOOL list_non_usb_removable_drives, its_a_me_mario, right_to_left_mode;
extern char* image_path;

/* ------------------------------------------------------------------------- */
/* Small sysfs helpers                                                       */
/* ------------------------------------------------------------------------- */

/* Read a sysfs attribute, trimming trailing whitespace. Returns FALSE if absent. */
static BOOL sysfs_read(const char* path, char* buf, size_t size)
{
	FILE* f;
	size_t len;
	if (size == 0)
		return FALSE;
	buf[0] = 0;
	f = fopen(path, "r");
	if (f == NULL)
		return FALSE;
	len = fread(buf, 1, size - 1, f);
	fclose(f);
	buf[len] = 0;
	while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == ' ' || buf[len - 1] == '\t' || buf[len - 1] == '\r'))
		buf[--len] = 0;
	return TRUE;
}

static BOOL sysfs_readf(char* buf, size_t size, const char* fmt, ...) __attribute__((format(printf, 3, 4)));
static BOOL sysfs_readf(char* buf, size_t size, const char* fmt, ...)
{
	char path[PATH_MAX];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(path, sizeof(path), fmt, ap);
	va_end(ap);
	return sysfs_read(path, buf, size);
}

static BOOL path_exists_f(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static BOOL path_exists_f(const char* fmt, ...)
{
	char path[PATH_MAX];
	struct stat st;
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(path, sizeof(path), fmt, ap);
	va_end(ap);
	return (stat(path, &st) == 0);
}

/* Trim leading and trailing spaces in place */
static char* trim_spaces(char* s)
{
	size_t len;
	while (*s == ' ' || *s == '\t')
		s++;
	len = strlen(s);
	while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t'))
		s[--len] = 0;
	return s;
}

/* Parse a "major:minor" sysfs 'dev' attribute */
static BOOL read_devt(const char* kname, const char* part, dev_t* devt)
{
	char buf[32];
	unsigned int maj, min;
	BOOL r = (part == NULL) ? sysfs_readf(buf, sizeof(buf), "/sys/block/%s/dev", kname) :
		sysfs_readf(buf, sizeof(buf), "/sys/block/%s/%s/dev", kname, part);
	if (!r || sscanf(buf, "%u:%u", &maj, &min) != 2)
		return FALSE;
	*devt = makedev(maj, min);
	return TRUE;
}

/*
 * Read a property from the udev database for a block device (e.g. ID_FS_LABEL_ENC).
 * The database is world readable, and populated once udev has probed the device.
 */
static BOOL udev_property(dev_t devt, const char* key, char* value, size_t size)
{
	char path[64], line[1024];
	size_t klen = strlen(key);
	FILE* f;
	BOOL r = FALSE;

	snprintf(path, sizeof(path), "/run/udev/data/b%u:%u", major(devt), minor(devt));
	f = fopen(path, "r");
	if (f == NULL)
		return FALSE;
	while (fgets(line, sizeof(line), f) != NULL) {
		if (line[0] != 'E' || line[1] != ':' || strncmp(&line[2], key, klen) != 0 || line[2 + klen] != '=')
			continue;
		line[strcspn(line, "\r\n")] = 0;
		snprintf(value, size, "%s", &line[3 + klen]);
		r = TRUE;
		break;
	}
	fclose(f);
	return r;
}

/* Decode the \xHH escapes that udev uses in its *_ENC properties */
static void udev_decode(char* s)
{
	char *r = s, *w = s;
	while (*r != 0) {
		if (r[0] == '\\' && r[1] == 'x' && IS_HEXASCII(r[2]) && IS_HEXASCII(r[3])) {
			*w++ = (char)((FROM_HEXASCII(r[2]) << 4) | FROM_HEXASCII(r[3]));
			r += 4;
		} else {
			*w++ = *r++;
		}
	}
	*w = 0;
}

/* ------------------------------------------------------------------------- */
/* Drive index table (see linux_drive.h)                                     */
/* ------------------------------------------------------------------------- */
typedef struct {
	BOOL used;
	dev_t devt;
	char kname[32];
	char path[48];
} disk_slot_t;

static disk_slot_t disk_slot[MAX_DRIVES];
static int next_slot = 0;
static pthread_mutex_t slot_lock = PTHREAD_MUTEX_INITIALIZER;

/*
 * Whole disks we care about. Virtual devices that can never be a target
 * (RAM disks, device mapper, RAID, optical drives) don't get a slot.
 */
static BOOL is_candidate_disk(const char* kname)
{
	char buf[PATH_MAX];
	static const char* excluded_prefix[] = { "ram", "zram", "dm-", "md", "sr", "fd", "nbd", "mtdblock", "drbd", "rbd", "zd" };
	size_t i;

	if (kname[0] == '.')
		return FALSE;
	for (i = 0; i < ARRAYSIZE(excluded_prefix); i++)
		if (strncmp(kname, excluded_prefix[i], strlen(excluded_prefix[i])) == 0)
			return FALSE;
	/* eMMC boot and RPMB partitions are exposed as separate block devices */
	if (strncmp(kname, "mmcblk", 6) == 0 && (strstr(kname, "boot") != NULL || strstr(kname, "rpmb") != NULL))
		return FALSE;
	if (strncmp(kname, "loop", 4) == 0) {
		char ro[8] = "";
		size_t len;
		/* Only writable loop devices that are backed by a file */
		if (!sysfs_readf(buf, sizeof(buf), "/sys/block/%s/loop/backing_file", kname) || buf[0] == 0)
			return FALSE;
		/* Snaps (and other read-only images) are not meant to be written to */
		len = strlen(buf);
		if (strstr(buf, "/snapd/") != NULL || (len > 5 && _stricmp(&buf[len - 5], ".snap") == 0))
			return FALSE;
		if (sysfs_readf(ro, sizeof(ro), "/sys/block/%s/ro", kname) && ro[0] == '1')
			return FALSE;
	}
	return TRUE;
}

/* Allocate a slot for a disk (caller holds slot_lock). Returns -1 if none left. */
static int alloc_slot(const char* kname, dev_t devt)
{
	int i, j;
	for (i = 0; i < MAX_DRIVES; i++)
		if (disk_slot[i].used && disk_slot[i].devt == devt && strcmp(disk_slot[i].kname, kname) == 0)
			return i;
	/*
	 * Round-robin allocation, so that the slot of a disk that was just removed
	 * doesn't get reused right away for another disk: a thread that would still
	 * be holding the old drive index must not end up accessing a different disk.
	 */
	for (j = 0; j < MAX_DRIVES; j++) {
		i = (next_slot + j) % MAX_DRIVES;
		if (!disk_slot[i].used) {
			disk_slot[i].used = TRUE;
			disk_slot[i].devt = devt;
			static_strcpy(disk_slot[i].kname, kname);
			static_sprintf(disk_slot[i].path, "/dev/%s", kname);
			next_slot = (i + 1) % MAX_DRIVES;
			return i;
		}
	}
	return -1;
}

/* Rescan /sys/block, assigning slots to new disks and releasing those of removed ones */
static void refresh_slots(void)
{
	DIR* dir;
	struct dirent* de;
	BOOL seen[MAX_DRIVES] = { 0 };
	dev_t devt;
	int i;

	dir = opendir("/sys/block");
	if (dir == NULL)
		return;
	pthread_mutex_lock(&slot_lock);
	while ((de = readdir(dir)) != NULL) {
		if (!is_candidate_disk(de->d_name) || !read_devt(de->d_name, NULL, &devt))
			continue;
		i = alloc_slot(de->d_name, devt);
		if (i >= 0)
			seen[i] = TRUE;
		else
			uprintf("WARNING: Found more than %d disks - ignoring '%s'", MAX_DRIVES, de->d_name);
	}
	for (i = 0; i < MAX_DRIVES; i++)
		if (disk_slot[i].used && !seen[i])
			memset(&disk_slot[i], 0, sizeof(disk_slot[i]));
	pthread_mutex_unlock(&slot_lock);
	closedir(dir);
}

static int index_to_slot(DWORD DriveIndex)
{
	if (DriveIndex < DRIVE_INDEX_MIN || DriveIndex >= DRIVE_INDEX_MIN + MAX_DRIVES)
		return -1;
	return (int)(DriveIndex - DRIVE_INDEX_MIN);
}

const char* linux_get_disk_path(DWORD DriveIndex)
{
	int i = index_to_slot(DriveIndex);
	const char* r = NULL;
	if (i < 0)
		return NULL;
	pthread_mutex_lock(&slot_lock);
	if (disk_slot[i].used)
		r = disk_slot[i].path;
	pthread_mutex_unlock(&slot_lock);
	return r;
}

const char* linux_get_disk_kname(DWORD DriveIndex)
{
	int i = index_to_slot(DriveIndex);
	const char* r = NULL;
	if (i < 0)
		return NULL;
	pthread_mutex_lock(&slot_lock);
	if (disk_slot[i].used)
		r = disk_slot[i].kname;
	pthread_mutex_unlock(&slot_lock);
	return r;
}

/* Resolve a block device kernel name (disk or partition) to the kernel name of its disk */
static BOOL get_parent_disk_kname(const char* name, char* disk, size_t size)
{
	char link[PATH_MAX], path[PATH_MAX], *p;
	snprintf(path, sizeof(path), "/sys/class/block/%s", name);
	if (realpath(path, link) == NULL)
		return FALSE;
	snprintf(path, sizeof(path), "%s/partition", link);
	if (access(path, F_OK) == 0) {
		/* A partition: the parent directory is the disk */
		p = strrchr(link, '/');
		if (p == NULL)
			return FALSE;
		*p = 0;
	}
	p = strrchr(link, '/');
	if (p == NULL)
		return FALSE;
	snprintf(disk, size, "%s", p + 1);
	return TRUE;
}

DWORD linux_get_drive_index(const char* dev_or_kname)
{
	char kname[64], disk[64];
	const char* name;
	int i, pass;

	if (dev_or_kname == NULL || dev_or_kname[0] == 0)
		return 0;
	name = (strncmp(dev_or_kname, "/dev/", 5) == 0) ? &dev_or_kname[5] : dev_or_kname;
	static_strcpy(kname, name);
	if (!get_parent_disk_kname(kname, disk, sizeof(disk)))
		static_strcpy(disk, kname);
	for (pass = 0; pass < 2; pass++) {
		pthread_mutex_lock(&slot_lock);
		for (i = 0; i < MAX_DRIVES; i++) {
			if (disk_slot[i].used && strcmp(disk_slot[i].kname, disk) == 0) {
				pthread_mutex_unlock(&slot_lock);
				return DRIVE_INDEX_MIN + (DWORD)i;
			}
		}
		pthread_mutex_unlock(&slot_lock);
		/* Not known yet: rescan once */
		if (pass == 0)
			refresh_slots();
	}
	return 0;
}

char* linux_get_partition_path(DWORD DriveIndex, int partition_number)
{
	const char* path = linux_get_disk_path(DriveIndex);
	char* r = NULL;
	size_t len;
	if (path == NULL || partition_number <= 0)
		return NULL;
	len = strlen(path);
	/* Disks whose name ends with a digit (nvme0n1, mmcblk0, loop0) use a 'p' separator */
	if (asprintf(&r, "%s%s%d", path, (len > 0 && isdigit((unsigned char)path[len - 1])) ? "p" : "",
		partition_number) < 0)
		return NULL;
	return r;
}

/* ------------------------------------------------------------------------- */
/* System disk detection                                                     */
/* ------------------------------------------------------------------------- */
#define MAX_PROTECTED 32
typedef struct {
	int count;
	char kname[MAX_PROTECTED][32];
	int reason[MAX_PROTECTED];	// 1 = disk Rufus runs from, 2 = system disk, 3 = selected image
} protected_disks_t;

static void protect_disk(protected_disks_t* pd, const char* kname, int reason)
{
	int i;
	for (i = 0; i < pd->count; i++) {
		if (strcmp(pd->kname[i], kname) == 0) {
			/* Keep the "most important" reason (system > app > image) */
			if (reason == 2 || (reason == 1 && pd->reason[i] == 3))
				pd->reason[i] = reason;
			return;
		}
	}
	if (pd->count < MAX_PROTECTED) {
		static_strcpy(pd->kname[pd->count], kname);
		pd->reason[pd->count++] = reason;
	}
}

/* Mark the disk(s) backing a block device, following device mapper/RAID/LVM slaves */
static void protect_devt(protected_disks_t* pd, dev_t devt, int reason, int depth)
{
	char path[PATH_MAX], link[PATH_MAX], disk[64], *name;
	DIR* dir;
	struct dirent* de;
	struct stat st;

	if (depth > 8 || major(devt) == 0)
		return;
	snprintf(path, sizeof(path), "/sys/dev/block/%u:%u", major(devt), minor(devt));
	if (realpath(path, link) == NULL)
		return;
	name = strrchr(link, '/');
	if (name == NULL)
		return;
	name++;
	/* Holders (dm-crypt, LVM, RAID...) list their underlying devices as slaves */
	snprintf(path, sizeof(path), "%s/slaves", link);
	dir = opendir(path);
	if (dir != NULL) {
		int n = 0;
		while ((de = readdir(dir)) != NULL) {
			char dev_attr[PATH_MAX + 64], buf[32];
			unsigned int maj, min;
			if (de->d_name[0] == '.')
				continue;
			snprintf(dev_attr, sizeof(dev_attr), "%s/%s/dev", path, de->d_name);
			if (sysfs_read(dev_attr, buf, sizeof(buf)) && sscanf(buf, "%u:%u", &maj, &min) == 2) {
				protect_devt(pd, makedev(maj, min), reason, depth + 1);
				n++;
			}
		}
		closedir(dir);
		if (n > 0)
			return;
	}
	/* Loop devices: also protect whatever holds their backing file */
	snprintf(path, sizeof(path), "%s/loop/backing_file", link);
	if (sysfs_read(path, disk, sizeof(disk)) && disk[0] == '/' && stat(disk, &st) == 0)
		protect_devt(pd, st.st_dev, reason, depth + 1);
	if (get_parent_disk_kname(name, disk, sizeof(disk)))
		protect_disk(pd, disk, reason);
}

/* Resolve the device behind a file system's st_dev, using mountinfo for anonymous (btrfs, overlay...) devices */
static void protect_fs_dev(protected_disks_t* pd, dev_t fs_dev, int reason)
{
	FILE* f;
	char line[4096], source[PATH_MAX], *sep;
	unsigned int maj, min;
	struct stat st;

	if (major(fs_dev) != 0) {
		protect_devt(pd, fs_dev, reason, 0);
		return;
	}
	f = fopen("/proc/self/mountinfo", "r");
	if (f == NULL)
		return;
	while (fgets(line, sizeof(line), f) != NULL) {
		if (sscanf(line, "%*d %*d %u:%u", &maj, &min) != 2 || makedev(maj, min) != fs_dev)
			continue;
		sep = strstr(line, " - ");
		if (sep != NULL && sscanf(sep + 3, "%*s %4095s", source) == 1 &&
			strncmp(source, "/dev/", 5) == 0 && stat(source, &st) == 0 && S_ISBLK(st.st_mode))
			protect_devt(pd, st.st_rdev, reason, 0);
	}
	fclose(f);
}

/* Unescape the octal sequences (\040) used in mountinfo */
static void unescape_mount(char* s)
{
	char *r = s, *w = s;
	while (*r != 0) {
		if (r[0] == '\\' && r[1] >= '0' && r[1] <= '7' && r[2] >= '0' && r[2] <= '7' && r[3] >= '0' && r[3] <= '7') {
			*w++ = (char)(((r[1] - '0') << 6) | ((r[2] - '0') << 3) | (r[3] - '0'));
			r += 4;
		} else {
			*w++ = *r++;
		}
	}
	*w = 0;
}

/*
 * Build the list of disks that must never be listed: the ones holding the
 * system (/, /boot, /usr, ...), swap, the Rufus executable, and the image
 * that is currently selected.
 */
static void get_protected_disks(protected_disks_t* pd)
{
	static const char* system_mounts[] = { "/", "/boot", "/boot/efi", "/efi", "/usr", "/home", "/var" };
	FILE* f;
	char line[4096], mnt[PATH_MAX], source[PATH_MAX], *sep;
	unsigned int maj, min;
	struct stat st;
	size_t i;

	memset(pd, 0, sizeof(*pd));

	f = fopen("/proc/self/mountinfo", "r");
	if (f != NULL) {
		while (fgets(line, sizeof(line), f) != NULL) {
			if (sscanf(line, "%*d %*d %u:%u %*s %4095s", &maj, &min, mnt) != 3)
				continue;
			unescape_mount(mnt);
			for (i = 0; i < ARRAYSIZE(system_mounts); i++)
				if (strcmp(mnt, system_mounts[i]) == 0)
					break;
			if (i >= ARRAYSIZE(system_mounts))
				continue;
			sep = strstr(line, " - ");
			if (sep != NULL && sscanf(sep + 3, "%*s %4095s", source) == 1 &&
				strncmp(source, "/dev/", 5) == 0 && stat(source, &st) == 0 && S_ISBLK(st.st_mode))
				protect_devt(pd, st.st_rdev, 2, 0);
			else
				protect_devt(pd, makedev(maj, min), 2, 0);
		}
		fclose(f);
	}

	/* Swap partitions and swap files */
	f = fopen("/proc/swaps", "r");
	if (f != NULL) {
		while (fgets(line, sizeof(line), f) != NULL) {
			if (sscanf(line, "%4095s", source) != 1 || source[0] != '/')
				continue;
			unescape_mount(source);
			if (stat(source, &st) != 0)
				continue;
			if (S_ISBLK(st.st_mode))
				protect_devt(pd, st.st_rdev, 2, 0);
			else
				protect_fs_dev(pd, st.st_dev, 2);
		}
		fclose(f);
	}

	/* The disk we are running from */
	if (stat("/proc/self/exe", &st) == 0)
		protect_fs_dev(pd, st.st_dev, 1);

	/* The disk holding the selected image */
	if (image_path != NULL && stat(image_path, &st) == 0)
		protect_fs_dev(pd, S_ISBLK(st.st_mode) ? st.st_rdev : st.st_dev, 3);
}

/* ------------------------------------------------------------------------- */
/* USB properties                                                            */
/* ------------------------------------------------------------------------- */
static uint32_t version_to_speed(uint16_t usb_version) {
	switch (usb_version) {
	case 0x0100: return USB_SPEED_LOW_1_5M;
	case 0x0110: return USB_SPEED_FULL_12M;
	case 0x0200:
	case 0x0201:
	case 0x0210: return USB_SPEED_HIGH_480M;
	case 0x0300: return USB_SPEED_SUPER_5G;
	case 0x0310: return USB_SPEED_SUPER_10G;
	case 0x0320: return USB_SPEED_SUPER_20G;
	case 0x0400: return USB_SPEED_SUPER_40G;
	case 0x0410: return USB_SPEED_SUPER_80G;
	default: return USB_SPEED_UNKNOWN;
	}
}

/*
 * Find the USB device and interface a disk hangs from. 'dev_path' is the resolved
 * sysfs path of the disk's device. Fills usb_dev (sysfs path of the USB device) and
 * returns the name of the driver bound to the USB interface ("usb-storage", "uas"...).
 */
static BOOL find_usb_parent(const char* dev_path, char* usb_dev, size_t usb_dev_size, char* driver, size_t driver_size)
{
	char path[PATH_MAX], tmp[PATH_MAX + 32], link[PATH_MAX], *p;
	BOOL found_if = FALSE;

	snprintf(path, sizeof(path), "%s", dev_path);
	driver[0] = 0;
	while ((p = strrchr(path, '/')) != NULL && p != path) {
		snprintf(tmp, sizeof(tmp), "%s/bInterfaceClass", path);
		if (!found_if && access(tmp, F_OK) == 0) {
			ssize_t r;
			found_if = TRUE;
			snprintf(tmp, sizeof(tmp), "%s/driver", path);
			r = readlink(tmp, link, sizeof(link) - 1);
			if (r > 0) {
				link[r] = 0;
				snprintf(driver, driver_size, "%s", (strrchr(link, '/') != NULL) ? strrchr(link, '/') + 1 : link);
			}
		}
		snprintf(tmp, sizeof(tmp), "%s/idVendor", path);
		if (access(tmp, F_OK) == 0) {
			snprintf(usb_dev, usb_dev_size, "%s", path);
			return TRUE;
		}
		*p = 0;
	}
	return FALSE;
}

/*
 * Get the VID, PID, port and current/max device speed from the USB device's
 * sysfs attributes. This is what dev.c's GetUSBProperties() gets from the hub.
 */
static BOOL GetUSBProperties(const char* usb_dev, usb_device_props* props)
{
	char buf[64];
	const char* p;
	unsigned int maj, min;
	uint32_t speed, lanes;
	BOOL r = FALSE;

	if (usb_dev == NULL || props == NULL)
		return FALSE;

	/* The port is the last element of the device path (e.g. "2-1.4" -> port 4) */
	p = strrchr(usb_dev, '/');
	p = (p == NULL) ? usb_dev : p + 1;
	props->port = 0;
	if (strrchr(p, '.') != NULL)
		props->port = (uint32_t)strtoul(strrchr(p, '.') + 1, NULL, 10);
	else if (strrchr(p, '-') != NULL)
		props->port = (uint32_t)strtoul(strrchr(p, '-') + 1, NULL, 10);

	if (sysfs_readf(buf, sizeof(buf), "%s/idVendor", usb_dev))
		props->vid = (int32_t)strtoul(buf, NULL, 16);
	if (sysfs_readf(buf, sizeof(buf), "%s/idProduct", usb_dev))
		props->pid = (int32_t)strtoul(buf, NULL, 16);
	if (props->vid != -1 && props->pid != -1 && (props->vid != 0 || props->pid != 0))
		r = TRUE;

	/* bcdUSB from the device descriptor, e.g. " 3.20" */
	if (sysfs_readf(buf, sizeof(buf), "%s/version", usb_dev) && sscanf(trim_spaces(buf), "%x.%x", &maj, &min) == 2)
		props->max_speed = version_to_speed((uint16_t)((maj << 8) | min));

	/* Negotiated speed, in Mbps, e.g. "1.5", "480", "5000", "10000", "20000" */
	if (sysfs_readf(buf, sizeof(buf), "%s/speed", usb_dev)) {
		BOOL low_speed = (strncmp(buf, "1.5", 3) == 0);
		speed = (uint32_t)strtoul(buf, NULL, 10);
		lanes = 1;
		/* USB 3.2 Gen 1x2 and Gen 2x2 operate on 2 lanes */
		if (sysfs_readf(buf, sizeof(buf), "%s/rx_lanes", usb_dev))
			lanes = max(lanes, (uint32_t)strtoul(buf, NULL, 10));
		if (sysfs_readf(buf, sizeof(buf), "%s/tx_lanes", usb_dev))
			lanes = max(lanes, (uint32_t)strtoul(buf, NULL, 10));
		if (low_speed)
			props->current_speed = USB_SPEED_LOW_1_5M;
		else if ((uint64_t)speed * lanes >= 80000)
			props->current_speed = USB_SPEED_SUPER_80G;
		else if ((uint64_t)speed * lanes >= 40000)
			props->current_speed = USB_SPEED_SUPER_40G;
		else if ((uint64_t)speed * lanes >= 20000)
			props->current_speed = USB_SPEED_SUPER_20G;
		else if ((uint64_t)speed * lanes >= 10000)
			props->current_speed = USB_SPEED_SUPER_10G;
		else if (speed >= 5000)
			props->current_speed = USB_SPEED_SUPER_5G;
		else if (speed >= 480)
			props->current_speed = USB_SPEED_HIGH_480M;
		else if (speed >= 12)
			props->current_speed = USB_SPEED_FULL_12M;
		else if (speed >= 1)
			props->current_speed = USB_SPEED_LOW_1_5M;
		/* A device operating above its descriptor's version is at least that fast */
		if (props->current_speed > props->max_speed)
			props->max_speed = props->current_speed;
	}
	return r;
}

/*
 * Build a Windows like hardware ID ("USBSTOR\DiskVendor__Product_________") from
 * the SCSI vendor and model, which is what dev.c matches VHD and card names against.
 */
static void build_hardware_id(char* buf, size_t size, const char* enumerator, const char* vendor, const char* model)
{
	char v[9], m[17];
	size_t i, vlen = strlen(vendor), mlen = strlen(model);
	for (i = 0; i < 8; i++)
		v[i] = (i >= vlen || vendor[i] == ' ') ? '_' : vendor[i];
	v[8] = 0;
	for (i = 0; i < 16; i++)
		m[i] = (i >= mlen || model[i] == ' ') ? '_' : model[i];
	m[16] = 0;
	snprintf(buf, size, "%s\\Disk%s%s", enumerator, v, m);
}

static __inline BOOL IsVHD(const char* buffer)
{
	int i;
	// List of the Hardware IDs of the VHD devices we know
	const char* vhd_name[] = {
		"Arsenal_________Virtual_",
		"KernSafeVirtual_________",
		"Msft____Virtual_Disk____",
		"BHYVE__________SATA_DISK",
		"VMware__VMware_Virtual_S"	// Enabled through a cheat mode, as this lists primary disks on VMWare instances
	};

	if (buffer == NULL || buffer[0] == '\0')
		return FALSE;
	for (i = 0; i < (int)(ARRAYSIZE(vhd_name) - (enable_vmdk ? 0 : 1)); i++)
		if (safe_strstr(buffer, vhd_name[i]) != NULL)
			return TRUE;
	return FALSE;
}

/* ------------------------------------------------------------------------- */
/* Public API                                                                */
/* ------------------------------------------------------------------------- */

/*
 * Cycle the USB port of the selected device: on Linux, this is achieved by
 * deauthorizing and reauthorizing the USB device, which results in the same
 * disconnect/reconnect sequence as a port cycle.
 */
BOOL CyclePort(int index)
{
	static uint64_t LastReset = 0;
	char path[PATH_MAX];
	FILE* f;
	BOOL r = FALSE;

	if_assert_fails(index < MAX_DRIVES)
		return -1;
	// Wait at least 10 secs between resets
	if (GetTickCount64() < LastReset + 10000ULL) {
		uprintf("You must wait at least 10 seconds before trying to reset a device");
		return FALSE;
	}

	if (index < 0 || rufus_drive[index].hub == NULL) {
		uprintf("The device you are trying to reset does not appear to be a USB device...");
		return FALSE;
	}

	LastReset = GetTickCount64();

	static_sprintf(path, "%s/authorized", rufus_drive[index].hub);
	uprintf("Cycling port %d (reset) on %s", rufus_drive[index].port, rufus_drive[index].hub);
	f = fopen(path, "w");
	if (f == NULL) {
		uprintf("Could not open %s: %s", path, WindowsErrorString());
		goto out;
	}
	if (fputs("0", f) < 0 || fflush(f) != 0) {
		uprintf("  Failed to cycle port: %s", WindowsErrorString());
		fclose(f);
		goto out;
	}
	fclose(f);
	Sleep(500);
	f = fopen(path, "w");
	if (f == NULL || fputs("1", f) < 0 || fflush(f) != 0) {
		uprintf("  Failed to cycle port: %s", WindowsErrorString());
		if (f != NULL)
			fclose(f);
		goto out;
	}
	fclose(f);
	uprintf("Please wait for the device to re-appear...");
	r = TRUE;

out:
	return r;
}

/*
 * Forces a refresh of the device. On Windows, this disables and re-enables the
 * device through SetupAPI. On Linux, we ask the SCSI layer to rescan the device,
 * which re-reads its capacity and properties without removing it.
 * Returns the Windows error code from the operation.
 */
int CycleDevice(int index)
{
	char path[PATH_MAX];
	FILE* f;
	int ret = ERROR_DEV_NOT_EXIST;

	if_assert_fails(index < MAX_DRIVES)
		return ERROR_INVALID_DRIVE;
	if ((index < 0) || (safe_strlen(rufus_drive[index].id) < 8))
		return ERROR_INVALID_PARAMETER;

	static_sprintf(path, "%s/rescan", rufus_drive[index].id);
	if (access(path, F_OK) != 0) {
		uprintf("Could not find a device to cycle!");
		return ret;
	}
	f = fopen(path, "w");
	if (f == NULL) {
		uprintf("Could not cycle device (E1): %s", WindowsErrorString());
		return ERROR_OPERATION_ABORTED;
	}
	if (fputs("1", f) < 0 || fflush(f) != 0) {
		uprintf("Could not cycle device (E2): %s", WindowsErrorString());
		ret = ERROR_GEN_FAILURE;
	} else {
		ret = ERROR_SUCCESS;
	}
	fclose(f);
	/* Give udev a chance to process the resulting events */
	Sleep(250);
	return ret;
}

BOOL GetOpticalMedia(IMG_SAVE* img_save)
{
	static char str[MAX_PATH];
	static char label[33];
	char name[256], vendor[64], model[64];
	int k, fd;
	uint8_t* buffer;
	uint64_t size;
	DIR* dir;
	struct dirent* de;

	dir = opendir("/sys/block");
	if (dir == NULL) {
		uprintf("Could not enumerate block devices: %s", WindowsErrorString());
		return FALSE;
	}
	while ((de = readdir(dir)) != NULL) {
		if (strncmp(de->d_name, "sr", 2) != 0)
			continue;
		vendor[0] = 0;
		model[0] = 0;
		sysfs_readf(vendor, sizeof(vendor), "/sys/block/%s/device/vendor", de->d_name);
		sysfs_readf(model, sizeof(model), "/sys/block/%s/device/model", de->d_name);
		static_sprintf(name, "%s %s", trim_spaces(vendor), trim_spaces(model));
		if (trim_spaces(name)[0] == 0)
			static_strcpy(name, "Generic Optical Drive");
		uprintf("Found '%s' optical device", trim_spaces(name));

		// Get the size of the inserted media (if any)
		static_sprintf(str, "/dev/%s", de->d_name);
		fd = open(str, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		if (fd < 0)
			continue;
		if (ioctl(fd, BLKGETSIZE64, &size) != 0 || size <= 4096) {
			// Rewritable media usually has a one sector
			close(fd);
			continue;
		}
		// Read the label directly from the Primary Volume Descriptor
		buffer = malloc(2048);
		if (buffer != NULL && pread(fd, buffer, 2048, 0x8000) == 2048) {
			memcpy(label, &buffer[0x28], sizeof(label) - 1);
			label[sizeof(label) - 1] = 0;
			for (k = (int)strlen(label) - 1; (k >= 0) && (label[k] == 0x20); k--)
				label[k] = 0;
			img_save->Label = label;
		}
		free(buffer);
		close(fd);
		img_save->DevicePath = str;
		img_save->DeviceSize = (LONGLONG)size;
		closedir(dir);
		return TRUE;
	}
	closedir(dir);
	return FALSE;
}

void ClearDrives(void)
{
	int i;
	for (i = 0; i < MAX_DRIVES && rufus_drive[i].size != 0; i++) {
		free(rufus_drive[i].id);
		free(rufus_drive[i].name);
		free(rufus_drive[i].display_name);
		free(rufus_drive[i].label);
		free(rufus_drive[i].hub);
	}
	memset(rufus_drive, 0, sizeof(rufus_drive));
}

/* Count the volumes (partitions or whole disk) of a disk that hold a file system */
static int count_volumes(const char* kname, char* first_label, size_t label_size)
{
	DIR* dir;
	struct dirent* de;
	char path[PATH_MAX], value[256];
	dev_t devt;
	int n = 0;

	if (first_label != NULL && label_size > 0)
		first_label[0] = 0;
	/* Super floppy: a file system on the whole disk */
	if (read_devt(kname, NULL, &devt) && udev_property(devt, "ID_FS_USAGE", value, sizeof(value)) &&
		strcmp(value, "filesystem") == 0) {
		if (first_label != NULL && udev_property(devt, "ID_FS_LABEL_ENC", value, sizeof(value))) {
			udev_decode(value);
			snprintf(first_label, label_size, "%s", value);
		}
		return 1;
	}
	static_sprintf(path, "/sys/block/%s", kname);
	dir = opendir(path);
	if (dir == NULL)
		return 0;
	while ((de = readdir(dir)) != NULL) {
		if (strncmp(de->d_name, kname, strlen(kname)) != 0 || !path_exists_f("/sys/block/%s/%s/partition", kname, de->d_name))
			continue;
		if (!read_devt(kname, de->d_name, &devt))
			continue;
		if (!udev_property(devt, "ID_FS_USAGE", value, sizeof(value)) || strcmp(value, "filesystem") != 0)
			continue;
		if (n++ == 0 && first_label != NULL && udev_property(devt, "ID_FS_LABEL_ENC", value, sizeof(value))) {
			udev_decode(value);
			snprintf(first_label, label_size, "%s", value);
		}
	}
	closedir(dir);
	return n;
}

/*
 * Refresh the list of USB devices
 */
BOOL GetDevices(DWORD devnum)
{
	const char* usb_version[USB_SPEED_MAX] = {
		"USB", "USB 1.0", "USB 1.1", "USB 2.1", "USB 3.0", "USB 3.1", "USB 3.2", "USB 4.0", "USB 4.1"
	};	// See, USB Forum, it *REALLY* isn't that hard!!!
	const char* usb_speed[USB_SPEED_MAX] = {
		"???", "1.5 Mbps", "12 Mbps", "480 Mbps", "5 Gbps", "10 Gbps", "20 Gbps", "40 Gbps", "80 Gbps"
	};
	// Oh, and we also have card devices (e.g. 'SCSI\DiskO2Micro_SD_...') under the SCSI enumerator...
	const char* scsi_card_name[] = {
		"_SD_", "_SDHC_", "_SDXC_", "_MMC_", "_MS_", "_MSPro_", "_xDPicture_", "_O2Media_"
	};
	const char* windows_sandbox_vhd_label = "PortableBaseLayer";
	char setting_name[32];
	char scsi_card_name_copy[16];
	BOOL r = FALSE, found = FALSE;
	DWORD i, j, drive_index;
	int s, u, v, score, num_drives = 0, nb_volumes;
	char drive_letters[27], display_msg[128], fs_label[128];
	char *label, *display_name, buffer[4 * KB], str[MAX_PATH], hardware_id[256], device_id[PATH_MAX];
	char vendor[64], model[64], usb_dev[PATH_MAX], driver[64], tmp[PATH_MAX], *method_str, *hub_path;
	uint32_t ignore_vid_pid[MAX_IGNORE_USB];
	uint64_t drive_size = 0;
	usb_device_props props;
	protected_disks_t protected_disks;
	disk_slot_t slots[MAX_DRIVES];

	IGNORE_RETVAL(ComboBox_ResetContent(hDeviceList));
	ClearDrives();

	// Build the list of USB devices we may want to ignore
	for (s = 0; s < ARRAYSIZE(ignore_vid_pid); s++) {
		static_sprintf(setting_name, "IgnoreUsb%02d", s + 1);
		ignore_vid_pid[s] = ReadSetting32(setting_name);
	}

	refresh_slots();
	get_protected_disks(&protected_disks);
	pthread_mutex_lock(&slot_lock);
	memcpy(slots, disk_slot, sizeof(slots));
	pthread_mutex_unlock(&slot_lock);

	// Now enumerate all our disk storage devices
	for (i = 0; num_drives < MAX_DRIVES && i < MAX_DRIVES; i++) {
		const char* kname = slots[i].kname;
		if (!slots[i].used)
			continue;
		memset(buffer, 0, sizeof(buffer));
		memset(&props, 0, sizeof(props));
		props.vid = -1; props.pid = -1;
		method_str = "";
		hub_path = NULL;
		drive_index = DRIVE_INDEX_MIN + i;
		vendor[0] = 0;
		model[0] = 0;
		usb_dev[0] = 0;
		hardware_id[0] = 0;

		static_sprintf(tmp, "/sys/block/%s/device", kname);
		if (realpath(tmp, device_id) == NULL) {
			static_sprintf(tmp, "/sys/block/%s", kname);
			if (realpath(tmp, device_id) == NULL)
				static_strcpy(device_id, "<N/A>");
		}
		sysfs_readf(vendor, sizeof(vendor), "/sys/block/%s/device/vendor", kname);
		sysfs_readf(model, sizeof(model), "/sys/block/%s/device/model", kname);
		sysfs_readf(str, sizeof(str), "/sys/block/%s/removable", kname);
		props.is_Removable = (str[0] == '1');

		if (strncmp(kname, "loop", 4) == 0) {
			// Loop devices are the Linux equivalent of mounted VHDs
			char backing[PATH_MAX] = "", ro[8] = "";
			sysfs_readf(backing, sizeof(backing), "/sys/block/%s/loop/backing_file", kname);
			sysfs_readf(ro, sizeof(ro), "/sys/block/%s/ro", kname);
			uuprintf("Processing 'LOOP' device:");
			// Snaps (and other read-only images) are not meant to be written to
			if (ro[0] == '1' || strstr(backing, "/snapd/") != NULL || (strlen(backing) > 5 &&
				_stricmp(&backing[strlen(backing) - 5], ".snap") == 0)) {
				uuprintf("  Unsupported or disabled by policy");
				continue;
			}
			props.is_SCSI = TRUE;
			props.is_VHD = TRUE;
			static_sprintf(buffer, "Virtual Disk (%s)", PathFindFileNameU(backing));
			static_sprintf(hardware_id, "LOOP\\%s", backing);
		} else if (strncmp(kname, "mmcblk", 6) == 0) {
			// MMC/SD devices are the equivalent of the "SD" enumerator
			char type[16] = "", name[64] = "";
			sysfs_readf(type, sizeof(type), "/sys/block/%s/device/type", kname);
			sysfs_readf(name, sizeof(name), "/sys/block/%s/device/name", kname);
			uuprintf("Processing 'SD' device:");
			props.is_SCSI = TRUE;
			if (_stricmp(type, "MMC") == 0) {
				// Soldered eMMC: this is internal storage, not a card
				props.is_Removable = FALSE;
			} else {
				props.is_CARD = TRUE;
				props.is_Removable = TRUE;
			}
			static_sprintf(buffer, "%s%s%s Card", name, (name[0] != 0) ? " " : "", (type[0] != 0) ? type : "SD");
			static_sprintf(hardware_id, "SD\\%s_%s", (type[0] != 0) ? type : "SD", name);
		} else if (find_usb_parent(device_id, usb_dev, sizeof(usb_dev), driver, sizeof(driver))) {
			uuprintf("Processing '%s' device:", (strcmp(driver, "uas") == 0) ? "UASPSTOR" : "USBSTOR");
			props.is_USB = TRUE;
			props.is_UASP = (strcmp(driver, "uas") == 0);
			build_hardware_id(hardware_id, sizeof(hardware_id), "USBSTOR", trim_spaces(vendor), trim_spaces(model));
			// Friendly name, as Windows would build it
			static_sprintf(buffer, "%s%s%s %s", trim_spaces(vendor), (vendor[0] != 0) ? " " : "",
				trim_spaces(model), props.is_UASP ? "SCSI Disk Device" : "USB Device");
		} else {
			// Generic storage (SATA, NVMe, virtio, SCSI...): the "SCSI" enumerator
			uuprintf("Processing 'SCSI' device:");
			props.is_SCSI = TRUE;
			if (strncmp(kname, "nvme", 4) == 0 || strncmp(kname, "vd", 2) == 0 || strncmp(kname, "xvd", 3) == 0) {
				char nvme_model[128] = "";
				sysfs_readf(nvme_model, sizeof(nvme_model), "/sys/block/%s/device/model", kname);
				static_sprintf(buffer, "%s", (nvme_model[0] != 0) ? trim_spaces(nvme_model) : kname);
			} else {
				static_sprintf(buffer, "%s%s%s SCSI Disk Device", trim_spaces(vendor), (vendor[0] != 0) ? " " : "",
					trim_spaces(model));
			}
			build_hardware_id(hardware_id, sizeof(hardware_id), "SCSI", trim_spaces(vendor), trim_spaces(model));
		}

		// We can't use the friendly name to find if a drive is a VHD, as friendly name string gets translated
		// according to your locale, so we poke the Hardware ID
		if (!props.is_VHD)
			props.is_VHD = IsVHD(hardware_id);
		if (!props.is_VHD)
		// Additional detection for SCSI card readers
		if ((!props.is_CARD) && (!props.is_USB) && (safe_strnicmp(hardware_id, "SCSI\\Disk", 9) == 0)) {
			for (j = 0; j < ARRAYSIZE(scsi_card_name); j++) {
				static_strcpy(scsi_card_name_copy, scsi_card_name[j]);
				if (safe_strstr(hardware_id, scsi_card_name_copy) != NULL) {
					props.is_CARD = TRUE;
					break;
				}
				// Also test for "_SD&" instead of "_SD_" and so on to allow for devices like
				// "SCSI\DiskRicoh_Storage_SD&REV_3.0" to be detected.
				if_assert_fails(strlen(scsi_card_name_copy) > 1)
					continue;
				scsi_card_name_copy[strlen(scsi_card_name_copy) - 1] = '&';
				if (safe_strstr(hardware_id, scsi_card_name_copy) != NULL) {
					props.is_CARD = TRUE;
					break;
				}
			}
		}
		uuprintf("  Hardware ID: '%s'", hardware_id);

		if (buffer[0] == 0 || trim_spaces(buffer)[0] == 0) {
			// We can afford a failure on this call - just replace the name with "USB Storage Device (Generic)"
			static_strcpy(buffer, lmprintf(MSG_045));
		} else if (props.is_USB && !props.is_VHD) {
			uuprintf("  Matched with: %s", usb_dev);
			if (GetUSBProperties(usb_dev, &props)) {
				method_str = "";
				hub_path = usb_dev;
			}
		}
		// Windows has the bad habit of appending "SCSI Disk Device" to the description
		// of UAS devices, which of course screws up detection of device that actually
		// describe themselves as SCSI-like disks, so replace that with "UAS Device".
		if (props.is_UASP) {
			const char scsi_disk_device_str[] = "SCSI Disk Device";
			const char uas_device_str[] = "UAS Device";
			char* marker = strstr(buffer, scsi_disk_device_str);
			if (marker != NULL && marker[sizeof(scsi_disk_device_str) - 1] == 0)
				memcpy(marker, uas_device_str, sizeof(uas_device_str));
		}
		if (props.is_VHD) {
			uprintf("Found VHD device '%s'", buffer);
		} else if ((props.is_CARD) && ((!props.is_USB) || ((props.vid == 0) && (props.pid == 0)))) {
			uprintf("Found card reader device '%s'", buffer);
		} else if ((!props.is_USB) && (!props.is_UASP) && (props.is_Removable)) {
			if (!list_non_usb_removable_drives) {
				uprintf("Found non-USB removable device '%s' => Eliminated", buffer);
				uuprintf("If you *REALLY* need, you can enable listing of this device with <Ctrl><Alt><F>");
				continue;
			}
			uprintf("Found non-USB removable device '%s'", buffer);
		} else {
			if (props.vid == -1 || props.pid == -1) {
				if (!props.is_USB) {
					// If we have a non removable SCSI drive and couldn't get a VID:PID,
					// we are most likely dealing with a system drive => eliminate it!
					uuprintf("Found non-USB non-removable device '%s' => Eliminated", buffer);
					continue;
				}
				static_strcpy(str, "????:????");	// Couldn't figure VID:PID
			} else {
				static_sprintf(str, "%04X:%04X", props.vid, props.pid);
				// I *REALLY* don't want to erase the devices below by accident.
				if (its_a_me_mario) {
					if ((props.vid == 0x0525) && (props.pid == 0x622b))
						continue;
					if ((props.vid == 0x0781) && (props.pid == 0x75a0))
						continue;
					if ((props.vid == 0x10d6) && (props.pid == 0x1101))
						continue;
				}
				// Also ignore USB devices that have been specifically flagged by the user
				for (s = 0; s < ARRAYSIZE(ignore_vid_pid); s++) {
					if (ignore_vid_pid[s] != 0 && (props.vid == (ignore_vid_pid[s] >> 16)) &&
						(props.pid == (ignore_vid_pid[s] & 0xffff))) {
						uprintf("Ignoring '%s' (%s), per user settings", buffer, str);
						break;
					}
				}
				if (s < ARRAYSIZE(ignore_vid_pid))
					continue;
			}
			if (props.current_speed >= USB_SPEED_MAX)
				props.current_speed = 0;
			uprintf("Found %s%s%s device '%s' (%s) %s", props.is_UASP ? "UAS (" : "",
				usb_version[props.current_speed], props.is_UASP ? ")" : "", buffer, str, method_str);
			if (props.max_speed > props.current_speed)
				uprintf("NOTE: This a %s (%s) device operating at %s speed (%s)...", usb_version[props.max_speed],
					usb_speed[props.max_speed], usb_version[props.current_speed], usb_speed[props.current_speed]);
		}

		if (!IsMediaPresent(drive_index)) {
			uprintf("Device eliminated because it appears to contain no media");
			continue;
		}
		drive_size = GetDriveSize(drive_index);
		if (drive_size < MIN_DRIVE_SIZE) {
			uprintf("Device eliminated because it is smaller than %s", SizeToHumanReadable(MIN_DRIVE_SIZE, FALSE, FALSE));
			continue;
		}

		if (!GetDriveLabel(drive_index, drive_letters, &label, FALSE))
			continue;
		// On Linux, the volumes of a disk are its partitions that hold a file system
		// (Windows would have assigned them a drive letter)
		nb_volumes = count_volumes(kname, fs_label, sizeof(fs_label));
		if ((label == NULL || safe_strcmp(label, STR_NO_LABEL) == 0) && fs_label[0] != 0)
			label = fs_label;
		if (label == NULL)
			label = STR_NO_LABEL;

		if ((props.is_SCSI) && (!props.is_UASP) && (!props.is_VHD)) {
			if (!props.is_Removable) {
				// Non removables should have been eliminated above, but since we
				// are potentially dealing with system drives, better safe than sorry
				continue;
			}
		}
		if ((!enable_HDDs) && (!props.is_VHD) && (!props.is_CARD) &&
			((score = IsHDD(drive_index, (uint16_t)props.vid, (uint16_t)props.pid, buffer)) > 0)) {
			uprintf("Device eliminated because it was detected as a Hard Drive or SSD (score %d > 0)", score);
			if (!list_non_usb_removable_drives)
				uprintf("If this device is not a Hard Drive or SSD, please e-mail the author of this application");
			uprintf("NOTE: You can enable the listing of Hard Drives under 'advanced drive properties'");
			continue;
		} else if ((!enable_HDDs) && (props.is_CARD) && (drive_size > MAX_DEFAULT_LIST_CARD_SIZE)) {
			uprintf("Device eliminated because it was detected as a card larger than %s",
				SizeToHumanReadable(MAX_DEFAULT_LIST_CARD_SIZE, FALSE, FALSE));
			uprintf("To use such a card, check 'List USB Hard Drives' under 'advanced drive properties'");
			continue;
		} else if (props.is_VHD && IsMsDevDrive(drive_index)) {
			uprintf("Device eliminated because it was detected as a Microsoft Dev Drive");
			continue;
		} else if (IsFilteredDrive(drive_index)) {
			continue;
		}
		// Windows 10 19H1 mounts a 'PortableBaseLayer' for its Windows Sandbox feature => unlist those
		if (safe_strcmp(label, windows_sandbox_vhd_label) == 0) {
			uprintf("Device eliminated because it is a Windows Sandbox VHD");
			continue;
		}
		// Bitdefender now uses a special 32 MB VHD
		if (props.is_VHD && safe_strnicmp(label, "Bitdefender", 11) == 0 && GetDriveSize(drive_index) <= 32 * MB) {
			uprintf("Device eliminated because it is a Bitdefender VHD");
			continue;
		}
		if (props.is_VHD && (!enable_VHDs)) {
			uprintf("Device eliminated because listing of VHDs is disabled (Alt-G)");
			continue;
		}

		// Make sure that we don't list any drive that should not be listed
		for (s = 0; s < protected_disks.count; s++)
			if (strcmp(protected_disks.kname[s], kname) == 0)
				break;
		if (s < protected_disks.count) {
			uprintf("Removing %s from the list: This is the %s!", kname,
				(protected_disks.reason[s] == 1) ? "disk from which " APPLICATION_NAME " is running" :
				((protected_disks.reason[s] == 2) ? "system disk" : "disk that holds the selected image"));
			continue;
		}

		// We have multiple volumes assigned to the same device (multiple partitions)
		// If that is the case, use "Multiple Volumes" instead of the label
		static_strcpy(display_msg, (nb_volumes > 1) ? lmprintf(MSG_047) : label);
		// Instead of the drive letters, append the kernel name of the disk
		if (right_to_left_mode)
			static_strcat(display_msg, RIGHT_TO_LEFT_MARK);
		safe_sprintf(&display_msg[strlen(display_msg)], sizeof(display_msg) - strlen(display_msg) - 1, " (%s)", kname);
		safe_sprintf(&display_msg[strlen(display_msg)], sizeof(display_msg) - strlen(display_msg) - 1,
			"%s [%s]", (right_to_left_mode) ? RIGHT_TO_LEFT_MARK : "",
			SizeToHumanReadable(drive_size, FALSE, use_fake_units));
		display_name = display_msg;

		rufus_drive[num_drives].index = drive_index;
		rufus_drive[num_drives].id = safe_strdup(device_id);
		rufus_drive[num_drives].name = safe_strdup(buffer);
		rufus_drive[num_drives].display_name = safe_strdup(display_name);
		rufus_drive[num_drives].label = safe_strdup(label);
		rufus_drive[num_drives].size = drive_size;
		if_assert_fails(rufus_drive[num_drives].size != 0)
			break;
		if (hub_path != NULL) {
			rufus_drive[num_drives].hub = safe_strdup(hub_path);
			rufus_drive[num_drives].port = props.port;
		}
		num_drives++;
		if (num_drives >= MAX_DRIVES)
			uprintf("WARNING: Found more than %d drives - ignoring remaining ones...", MAX_DRIVES);
	}

	// Reorder the drives by increasing size, using the "selection sort" algorithm
	for (u = 0; u < num_drives - 1; u++) {
		uint64_t min_drive_size = rufus_drive[u].size;
		int min_index = u;
		for (v = u + 1; v < num_drives; v++) {
			if (rufus_drive[v].size < min_drive_size) {
				min_drive_size = rufus_drive[v].size;
				min_index = v;
			}
		}
		if (min_index != u) {
			RUFUS_DRIVE tmp_drive;
			memcpy(&tmp_drive, &rufus_drive[u], sizeof(RUFUS_DRIVE));
			memcpy(&rufus_drive[u], &rufus_drive[min_index], sizeof(RUFUS_DRIVE));
			memcpy(&rufus_drive[min_index], &tmp_drive, sizeof(RUFUS_DRIVE));
		}
	}

	// Now populate the drive combo box
	// NB: The combo box must have the UNSORTED attribute for indexes to remain the ones we assign
	for (u = 0; u < num_drives; u++)
		IGNORE_RETVAL(ComboBox_SetItemData(hDeviceList, ComboBox_AddStringU(hDeviceList, rufus_drive[u].display_name), rufus_drive[u].index));

	if (devnum >= DRIVE_INDEX_MIN) {
		for (i = 0; i < (DWORD)ComboBox_GetCount(hDeviceList); i++) {
			if ((DWORD)ComboBox_GetItemData(hDeviceList, i) == devnum) {
				found = TRUE;
				break;
			}
		}
	}
	if (!found)
		i = 0;
	IGNORE_RETVAL(ComboBox_SetCurSel(hDeviceList, i));
	SendMessage(hMainDialog, WM_COMMAND, (CBN_SELCHANGE<<16) | IDC_DEVICE, 0);
	r = TRUE;

	// Set 'Start' as the selected button, so that tab selection works
	SendMessage(hMainDialog, WM_NEXTDLGCTL, (WPARAM)GetDlgItem(hMainDialog, IDC_START), TRUE);
	return r;
}

/* ------------------------------------------------------------------------- */
/* Hotplug monitor                                                           */
/* ------------------------------------------------------------------------- */
#define UEVENT_BUFFER_SIZE	(64 * KB)
#define HOTPLUG_DEBOUNCE	1000	// Same delay as the TID_REFRESH_TIMER of the Windows version

static HANDLE monitor_thread = NULL;
static volatile BOOL monitor_active = FALSE;
static int monitor_pipe[2] = { -1, -1 };

/* libudev's netlink message header (udev multicast group) */
typedef struct {
	char prefix[8];			// "libudev"
	uint32_t magic;			// htobe32(0xfeedcafe)
	uint32_t header_size;
	uint32_t properties_off;
	uint32_t properties_len;
	uint32_t filter_subsystem_hash;
	uint32_t filter_devtype_hash;
	uint32_t filter_tag_bloom_hi;
	uint32_t filter_tag_bloom_lo;
} udev_netlink_header;

/* Returns TRUE if a uevent is about a block disk or partition being added, removed or changed */
static BOOL is_block_event(const char* buf, size_t len)
{
	const char *p, *end = buf + len, *subsystem = NULL, *devtype = NULL, *action = NULL;

	if (len >= sizeof(udev_netlink_header) && memcmp(buf, "libudev", 8) == 0) {
		const udev_netlink_header* hdr = (const udev_netlink_header*)buf;
		if (hdr->properties_off >= len)
			return FALSE;
		p = buf + hdr->properties_off;
	} else {
		// Kernel event: "action@devpath" followed by KEY=VALUE pairs
		p = memchr(buf, 0, len);
		if (p == NULL)
			return FALSE;
		p++;
	}
	for (; p < end && *p != 0; p += strnlen(p, end - p) + 1) {
		if (strncmp(p, "SUBSYSTEM=", 10) == 0)
			subsystem = p + 10;
		else if (strncmp(p, "DEVTYPE=", 8) == 0)
			devtype = p + 8;
		else if (strncmp(p, "ACTION=", 7) == 0)
			action = p + 7;
	}
	if (subsystem == NULL || strcmp(subsystem, "block") != 0)
		return FALSE;
	if (devtype == NULL || (strcmp(devtype, "disk") != 0 && strcmp(devtype, "partition") != 0))
		return FALSE;
	if (action == NULL)
		return FALSE;
	return (strcmp(action, "add") == 0 || strcmp(action, "remove") == 0 || strcmp(action, "change") == 0);
}

static DWORD WINAPI DeviceMonitorThread(void* param)
{
	struct sockaddr_nl addr = { 0 };
	struct pollfd pfd[2];
	char* buf;
	ssize_t len;
	int fd, timeout;
	uint64_t deadline = 0;
	(void)param;

	buf = malloc(UEVENT_BUFFER_SIZE);
	if (buf == NULL)
		ExitThread(1);
	fd = socket(AF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, NETLINK_KOBJECT_UEVENT);
	if (fd < 0) {
		uprintf("Could not create hotplug monitor socket: %s", WindowsErrorString());
		free(buf);
		ExitThread(1);
	}
	addr.nl_family = AF_NETLINK;
	addr.nl_pid = 0;
	// Kernel (1) and udev (2) multicast groups: udev events come in once the device
	// database has been updated, kernel ones ensure we get notified even without udev
	addr.nl_groups = 1 | 2;
	if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
		addr.nl_groups = 1;
		if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
			uprintf("Could not bind hotplug monitor socket: %s", WindowsErrorString());
			close(fd);
			free(buf);
			ExitThread(1);
		}
	}

	pfd[0].fd = fd;
	pfd[0].events = POLLIN;
	pfd[1].fd = monitor_pipe[0];
	pfd[1].events = POLLIN;
	while (monitor_active) {
		if (deadline != 0) {
			uint64_t now = GetTickCount64();
			if (now >= deadline) {
				deadline = 0;
				PostMessage(hMainDialog, UM_MEDIA_CHANGE, 0, 0);
				continue;
			}
			timeout = (int)(deadline - now);
		} else {
			timeout = -1;
		}
		if (poll(pfd, 2, timeout) < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (pfd[1].revents != 0)
			break;
		if (!(pfd[0].revents & POLLIN))
			continue;
		while ((len = recv(fd, buf, UEVENT_BUFFER_SIZE - 1, 0)) > 0) {
			buf[len] = 0;
			// If it's been more than a second since the last event, arm a refresh "timer"
			if (is_block_event(buf, (size_t)len) && deadline == 0)
				deadline = GetTickCount64() + HOTPLUG_DEBOUNCE;
		}
	}
	close(fd);
	free(buf);
	ExitThread(0);
}

BOOL linux_start_device_monitor(void)
{
	if (monitor_thread != NULL)
		return TRUE;
	if (pipe2(monitor_pipe, O_CLOEXEC) != 0) {
		uprintf("Could not create hotplug monitor pipe: %s", WindowsErrorString());
		return FALSE;
	}
	monitor_active = TRUE;
	monitor_thread = CreateThread(NULL, 0, DeviceMonitorThread, NULL, 0, NULL);
	if (monitor_thread == NULL) {
		uprintf("Could not start hotplug monitor: %s", WindowsErrorString());
		monitor_active = FALSE;
		close(monitor_pipe[0]);
		close(monitor_pipe[1]);
		monitor_pipe[0] = monitor_pipe[1] = -1;
		return FALSE;
	}
	return TRUE;
}

void linux_stop_device_monitor(void)
{
	if (monitor_thread == NULL)
		return;
	monitor_active = FALSE;
	if (monitor_pipe[1] >= 0)
		IGNORE_RETVAL(write(monitor_pipe[1], "q", 1));
	if (WaitForSingleObject(monitor_thread, 2000) != WAIT_OBJECT_0)
		TerminateThread(monitor_thread, 0);
	CloseHandle(monitor_thread);
	monitor_thread = NULL;
	close(monitor_pipe[0]);
	close(monitor_pipe[1]);
	monitor_pipe[0] = monitor_pipe[1] = -1;
}
