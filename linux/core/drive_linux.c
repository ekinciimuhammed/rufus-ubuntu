/*
 * Rufus for Linux: drive and volume handling
 * Copyright © 2026 Rufus Linux port contributors
 *
 * Linux implementation of the parts of src/drive.c that depend on Windows'
 * volume manager (volume GUIDs, drive letters, mount points, VDS). The rest of
 * drive.c (partition data, partition creation, ESP handling...) is shared with
 * Windows and runs on top of the IOCTL emulation from linux/compat.
 *
 * Volumes are identified by their block device node (e.g. /dev/sdb1) and, once
 * mounted by Rufus under RUFUS_RUN_DIR, by a virtual drive letter.
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
#include <pthread.h>
#include <sys/statvfs.h>
#include <msapi_utf8_linux.h>
#include <dirent.h>
#include <poll.h>
#include <spawn.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/wait.h>
#include <linux/fs.h>

#include "rufus.h"
#include "drive.h"
#include "missing.h"
#include "settings.h"
#include "localization.h"
#include "linux_drive.h"
#include "compat.h"

extern char** environ;

#define CheckDriveIndex(DriveIndex) do {                                            \
	if ((int)DriveIndex < 0) goto out;                                              \
	assert((DriveIndex >= DRIVE_INDEX_MIN) && (DriveIndex <= DRIVE_INDEX_MAX));     \
	if ((DriveIndex < DRIVE_INDEX_MIN) || (DriveIndex > DRIVE_INDEX_MAX)) goto out; \
	} while (0)

/* ------------------------------------------------------------------------- */
/* Running external programs                                                 */
/* ------------------------------------------------------------------------- */
int linux_run(const char* const argv[], BOOL log_output)
{
	posix_spawn_file_actions_t fa;
	int pipefd[2] = { -1, -1 }, status = 0, r;
	pid_t pid;
	char buf[4096], line[4096];
	size_t line_len = 0;
	ssize_t n;
	int i;

	if (argv == NULL || argv[0] == NULL)
		return -1;
	if (pipe2(pipefd, O_CLOEXEC) != 0)
		return -1;
	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_adddup2(&fa, pipefd[1], STDOUT_FILENO);
	posix_spawn_file_actions_adddup2(&fa, pipefd[1], STDERR_FILENO);
	posix_spawn_file_actions_addopen(&fa, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
	r = posix_spawnp(&pid, argv[0], &fa, NULL, (char* const*)argv, environ);
	posix_spawn_file_actions_destroy(&fa);
	close(pipefd[1]);
	if (r != 0) {
		close(pipefd[0]);
		if (log_output)
			uprintf("Could not run '%s': %s", argv[0], strerror(r));
		return -1;
	}
	while ((n = read(pipefd[0], buf, sizeof(buf))) > 0 || (n < 0 && errno == EINTR)) {
		for (i = 0; i < n; i++) {
			if (buf[i] == '\n' || buf[i] == '\r' || line_len >= sizeof(line) - 1) {
				line[line_len] = 0;
				if (log_output && line_len > 0)
					uprintf("  %s", line);
				line_len = 0;
			} else {
				line[line_len++] = buf[i];
			}
		}
	}
	if (log_output && line_len > 0) {
		line[line_len] = 0;
		uprintf("  %s", line);
	}
	close(pipefd[0]);
	while (waitpid(pid, &status, 0) < 0 && errno == EINTR);
	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* Run a program and capture its output */
BOOL linux_run_capture(const char* const argv[], char* out, size_t out_size)
{
	posix_spawn_file_actions_t fa;
	int pipefd[2], status = 0, r;
	pid_t pid;
	size_t len = 0;
	ssize_t n;

	if (out_size == 0 || pipe2(pipefd, O_CLOEXEC) != 0)
		return FALSE;
	out[0] = 0;
	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_adddup2(&fa, pipefd[1], STDOUT_FILENO);
	posix_spawn_file_actions_addopen(&fa, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
	posix_spawn_file_actions_addopen(&fa, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
	r = posix_spawnp(&pid, argv[0], &fa, NULL, (char* const*)argv, environ);
	posix_spawn_file_actions_destroy(&fa);
	close(pipefd[1]);
	if (r != 0) {
		close(pipefd[0]);
		return FALSE;
	}
	while (len < out_size - 1 && ((n = read(pipefd[0], &out[len], out_size - 1 - len)) > 0 || (n < 0 && errno == EINTR)))
		if (n > 0)
			len += n;
	out[len] = 0;
	close(pipefd[0]);
	while (waitpid(pid, &status, 0) < 0 && errno == EINTR);
	return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

/* ------------------------------------------------------------------------- */
/* Block device helpers                                                      */
/* ------------------------------------------------------------------------- */
typedef struct {
	char kname[64];
	int number;
	uint64_t start;		/* bytes */
	uint64_t size;		/* bytes */
} part_info_t;

static BOOL read_sysfs_str(const char* path, char* buf, size_t size)
{
	FILE* f = fopenU(path, "r");
	BOOL r = FALSE;
	if (f == NULL)
		return FALSE;
	if (fgets(buf, (int)size, f) != NULL) {
		buf[strcspn(buf, "\r\n")] = 0;
		r = TRUE;
	}
	fclose(f);
	return r;
}

static uint64_t read_sysfs_num(const char* path)
{
	char buf[64];
	return read_sysfs_str(path, buf, sizeof(buf)) ? strtoull(buf, NULL, 10) : 0;
}

/* List the partitions of a disk, sorted by partition number */
static int list_partitions(const char* disk_kname, part_info_t* parts, int max_parts)
{
	char path[256];
	DIR* d;
	struct dirent* de;
	int n = 0, i, j;
	size_t klen = strlen(disk_kname);

	snprintf(path, sizeof(path), "/sys/block/%s", disk_kname);
	d = opendir(path);
	if (d == NULL)
		return 0;
	while ((de = readdir(d)) != NULL && n < max_parts) {
		if (strncmp(de->d_name, disk_kname, klen) != 0)
			continue;
		snprintf(path, sizeof(path), "/sys/block/%s/%s/partition", disk_kname, de->d_name);
		if (access(path, F_OK) != 0)
			continue;
		snprintf(parts[n].kname, sizeof(parts[n].kname), "%s", de->d_name);
		parts[n].number = (int)read_sysfs_num(path);
		snprintf(path, sizeof(path), "/sys/block/%s/%s/start", disk_kname, de->d_name);
		parts[n].start = read_sysfs_num(path) * 512;
		snprintf(path, sizeof(path), "/sys/block/%s/%s/size", disk_kname, de->d_name);
		parts[n].size = read_sysfs_num(path) * 512;
		n++;
	}
	closedir(d);
	for (i = 0; i < n; i++) {
		for (j = i + 1; j < n; j++) {
			if (parts[j].number < parts[i].number) {
				part_info_t t = parts[i];
				parts[i] = parts[j];
				parts[j] = t;
			}
		}
	}
	return n;
}

/* Find the kernel name of the block device backing a path */
static BOOL path_to_kname(const char* path, char* kname, size_t size)
{
	struct stat st;
	char syspath[128], link[256], *p;
	ssize_t r;
	if (stat(path, &st) != 0)
		return FALSE;
	snprintf(syspath, sizeof(syspath), "/sys/dev/block/%u:%u",
		S_ISBLK(st.st_mode) ? major(st.st_rdev) : major(st.st_dev),
		S_ISBLK(st.st_mode) ? minor(st.st_rdev) : minor(st.st_dev));
	r = readlink(syspath, link, sizeof(link) - 1);
	if (r <= 0)
		return FALSE;
	link[r] = 0;
	p = strrchr(link, '/');
	snprintf(kname, size, "%s", (p == NULL) ? link : p + 1);
	return TRUE;
}

/* Kernel name of the whole disk a block device (disk or partition) belongs to */
static BOOL kname_to_disk(const char* kname, char* disk, size_t size)
{
	char path[256], link[512], *p;
	ssize_t r;
	snprintf(path, sizeof(path), "/sys/class/block/%s", kname);
	r = readlink(path, link, sizeof(link) - 1);
	if (r <= 0)
		return FALSE;
	link[r] = 0;
	snprintf(path, sizeof(path), "/sys/class/block/%s/partition", kname);
	if (access(path, F_OK) == 0) {
		p = strrchr(link, '/');
		if (p == NULL)
			return FALSE;
		*p = 0;
	}
	p = strrchr(link, '/');
	snprintf(disk, size, "%s", (p == NULL) ? link : p + 1);
	return TRUE;
}

/* ------------------------------------------------------------------------- */
/* Mounts                                                                    */
/* ------------------------------------------------------------------------- */
/* Decode the octal escapes used in /proc/self/mountinfo */
static void unescape_mount(char* s)
{
	char* o = s;
	while (*s) {
		if (s[0] == '\\' && s[1] >= '0' && s[1] <= '3' && s[2] >= '0' && s[2] <= '7' && s[3] >= '0' && s[3] <= '7') {
			*o++ = (char)(((s[1] - '0') << 6) | ((s[2] - '0') << 3) | (s[3] - '0'));
			s += 4;
		} else {
			*o++ = *s++;
		}
	}
	*o = 0;
}

typedef BOOL (*mount_cb)(const char* source_kname, const char* target, const char* fstype, void* ctx);

/* Enumerate the mounts of block devices, calling cb for each */
static void for_each_mount(mount_cb cb, void* ctx)
{
	FILE* f = fopenU("/proc/self/mountinfo", "r");
	char line[4096], target[MAX_PATH], fstype[64], *p;
	unsigned int maj, min;
	char kname[64], syspath[64], link[256];
	ssize_t r;

	if (f == NULL)
		return;
	while (fgets(line, sizeof(line), f) != NULL) {
		/* 36 35 98:0 /mnt1 /mnt2 rw,noatime master:1 - ext3 /dev/root rw,errors=continue */
		if (sscanf(line, "%*u %*u %u:%u %*s %4095s", &maj, &min, target) != 3)
			continue;
		p = strstr(line, " - ");
		if (p == NULL || sscanf(p + 3, "%63s", fstype) != 1)
			continue;
		unescape_mount(target);
		snprintf(syspath, sizeof(syspath), "/sys/dev/block/%u:%u", maj, min);
		r = readlink(syspath, link, sizeof(link) - 1);
		if (r <= 0)
			continue;	/* Not a block device (tmpfs, fuse, ...) */
		link[r] = 0;
		p = strrchr(link, '/');
		snprintf(kname, sizeof(kname), "%s", (p == NULL) ? link : p + 1);
		if (!cb(kname, target, fstype, ctx))
			break;
	}
	fclose(f);
}

/* ntfs-3g mounts appear as "fuseblk": find them through their source device */
static void for_each_fuse_mount(mount_cb cb, void* ctx)
{
	FILE* f = fopenU("/proc/self/mountinfo", "r");
	char line[4096], target[MAX_PATH], fstype[64], source[MAX_PATH], kname[64], *p;
	if (f == NULL)
		return;
	while (fgets(line, sizeof(line), f) != NULL) {
		if (sscanf(line, "%*u %*u %*u:%*u %*s %4095s", target) != 1)
			continue;
		p = strstr(line, " - ");
		if (p == NULL || sscanf(p + 3, "%63s %4095s", fstype, source) != 2)
			continue;
		if (strcmp(fstype, "fuseblk") != 0 || strncmp(source, "/dev/", 5) != 0)
			continue;
		unescape_mount(target);
		unescape_mount(source);
		if (!path_to_kname(source, kname, sizeof(kname)))
			continue;
		if (!cb(kname, target, fstype, ctx))
			break;
	}
	fclose(f);
}

typedef struct {
	const char* kname;		/* match this exact device... */
	const char* disk;		/* ...or any partition of this disk */
	StrArray* targets;
} mount_match_ctx;

static BOOL collect_mounts_cb(const char* source_kname, const char* target, const char* fstype, void* ctx)
{
	mount_match_ctx* m = (mount_match_ctx*)ctx;
	char disk[64];
	(void)fstype;
	if (m->kname != NULL && strcmp(source_kname, m->kname) == 0) {
		StrArrayAdd(m->targets, target, TRUE);
	} else if (m->disk != NULL && kname_to_disk(source_kname, disk, sizeof(disk)) && strcmp(disk, m->disk) == 0) {
		StrArrayAdd(m->targets, target, TRUE);
	}
	return TRUE;
}

static void get_mounts(const char* kname, const char* disk, StrArray* targets)
{
	mount_match_ctx m = { kname, disk, targets };
	for_each_mount(collect_mounts_cb, &m);
	for_each_fuse_mount(collect_mounts_cb, &m);
}

static BOOL unmount_path(const char* target, BOOL bSilent)
{
	int i;
	for (i = 0; i < 10; i++) {
		if (umount2(target, 0) == 0) {
			suprintf("Unmounted '%s'", target);
			return TRUE;
		}
		if (errno == EINVAL || errno == ENOENT)
			return TRUE;	/* Not mounted (anymore) */
		if (errno != EBUSY)
			break;
		sync();
		Sleep(250);
	}
	/* Last resort, like Windows' forced dismount: detach the file system */
	if (errno == EBUSY && umount2(target, MNT_DETACH) == 0) {
		uprintf("WARNING: '%s' was busy and had to be detached", target);
		return TRUE;
	}
	uprintf("Could not unmount '%s': %s", target, strerror(errno));
	return FALSE;
}

BOOL linux_unmount_device(const char* kname, BOOL bSilent)
{
	StrArray targets;
	uint32_t i;
	BOOL r = TRUE;
	char letter;
	StrArrayCreate(&targets, 8);
	get_mounts(kname, NULL, &targets);
	/* Unmount in reverse order, for nested mounts */
	for (i = targets.Index; i > 0; i--) {
		letter = linux_get_drive_letter_for_mount(targets.String[i - 1]);
		r = unmount_path(targets.String[i - 1], bSilent) && r;
		if (letter != 0)
			linux_unmap_drive_letter(letter);
	}
	StrArrayDestroy(&targets);
	return r;
}

/* Unmount every file system that lives on a disk (partitions included) */
static BOOL unmount_disk(const char* disk_kname, BOOL bSilent)
{
	StrArray targets;
	uint32_t i;
	BOOL r = TRUE;
	char letter;
	StrArrayCreate(&targets, 8);
	get_mounts(NULL, disk_kname, &targets);
	for (i = targets.Index; i > 0; i--) {
		letter = linux_get_drive_letter_for_mount(targets.String[i - 1]);
		r = unmount_path(targets.String[i - 1], bSilent) && r;
		if (letter != 0)
			linux_unmap_drive_letter(letter);
	}
	StrArrayDestroy(&targets);
	return r;
}

/* Probe a block device with blkid (low level probing, no cache) */
static BOOL blkid_probe(const char* dev, const char* tag, char* out, size_t out_size)
{
	const char* argv[] = { "blkid", "-p", "-s", tag, "-o", "value", dev, NULL };
	if (!linux_run_capture(argv, out, out_size))
		return FALSE;
	out[strcspn(out, "\r\n")] = 0;
	return (out[0] != 0);
}

/* Kernel file system driver to use for a block device */
static const char* get_mount_fstype(const char* dev)
{
	static char type[32];
	char version[32];
	if (!blkid_probe(dev, "TYPE", type, sizeof(type)))
		return NULL;
	if (strcmp(type, "vfat") == 0 || strcmp(type, "exfat") == 0 || strcmp(type, "udf") == 0 ||
		strcmp(type, "ext2") == 0 || strcmp(type, "ext3") == 0 || strcmp(type, "ext4") == 0 ||
		strcmp(type, "iso9660") == 0)
		return type;
	if (strcmp(type, "ntfs") == 0)
		return "ntfs3";
	if (strcmp(type, "msdos") == 0)
		return "vfat";
	(void)version;
	return type;
}

static BOOL is_mountpoint(const char* path)
{
	struct stat a, b;
	char parent[MAX_PATH];
	if (stat(path, &a) != 0)
		return FALSE;
	snprintf(parent, sizeof(parent), "%s/..", path);
	if (stat(parent, &b) != 0)
		return FALSE;
	return (a.st_dev != b.st_dev) || (a.st_ino == b.st_ino);
}

/* Mount a volume on our private mount point for a drive letter */
static BOOL mount_volume(const char* dev, char letter, BOOL bSilent)
{
	char target[64];
	const char* fstype;
	const char* opts = NULL;
	unsigned long flags = MS_NOSUID | MS_NODEV | MS_NOEXEC;
	int r;

	letter = (char)toupper((unsigned char)letter);
	snprintf(target, sizeof(target), "%s/%c", RUFUS_RUN_DIR, letter);
	if (mkdir(RUFUS_RUN_DIR, 0700) != 0 && errno != EEXIST) {
		uprintf("Could not create '%s': %s", RUFUS_RUN_DIR, strerror(errno));
		return FALSE;
	}
	if (mkdir(target, 0700) != 0 && errno != EEXIST) {
		uprintf("Could not create '%s': %s", target, strerror(errno));
		return FALSE;
	}
	if (is_mountpoint(target)) {
		/* Already in use: release it first */
		if (!unmount_path(target, bSilent))
			return FALSE;
	}
	fstype = get_mount_fstype(dev);
	if (fstype == NULL) {
		suprintf("Could not detect the file system of %s", dev);
		SetLastError(ERROR_UNRECOGNIZED_MEDIA);
		return FALSE;
	}
	if (strcmp(fstype, "vfat") == 0)
		/* Behave like Windows: case is preserved for 8.3 names, UTF-8 long names */
		opts = "shortname=mixed,utf8=1,errors=remount-ro";
	else if (strcmp(fstype, "exfat") == 0)
		opts = "iocharset=utf8";
	else if (strcmp(fstype, "ntfs3") == 0)
		opts = "iocharset=utf8";
	r = mount(dev, target, fstype, flags, opts);
	if (r != 0 && strcmp(fstype, "ntfs3") == 0) {
		/* Fall back to ntfs-3g if the kernel driver is not available */
		const char* argv[] = { "mount", "-t", "ntfs-3g", "-o", "nosuid,nodev,noexec", dev, target, NULL };
		r = (linux_run(argv, TRUE) == 0) ? 0 : -1;
		if (r != 0)
			errno = ENODEV;
	}
	if (r != 0) {
		uprintf("Could not mount %s on %s (%s): %s", dev, target, fstype, strerror(errno));
		SetLastError(ErrnoToWin32(errno));
		return FALSE;
	}
	if (strcmp(fstype, "vfat") == 0) {
		/*
		 * Windows' FAT driver scans the FAT when it mounts a volume, but Linux starts from
		 * the free cluster count of the FAT32 FSInfo sector, which some boot records (e.g.
		 * ms-sys' bootmgr one) set to 0. Getting the file system statistics makes Linux
		 * count the free clusters, so that the FSInfo sector is valid when we unmount.
		 */
		struct statvfs sv;
		if (statvfs(target, &sv) != 0)
			uprintf("Could not get the free space of %s: %s", target, strerror(errno));
	}
	linux_map_drive_letter(letter, target);
	suprintf("Mounted %s (%s) as %c: (%s)", dev, fstype, letter, target);
	return TRUE;
}

/*
 * Release the mounts that a previous instance of Rufus may have left under RUFUS_RUN_DIR
 * if it was killed. Only called once we hold the single instance lock.
 */
void linux_cleanup_stale_mounts(void)
{
	FILE* f = fopenU("/proc/self/mountinfo", "r");
	char line[4096], target[MAX_PATH];
	StrArray targets;
	size_t prefix_len = strlen(RUFUS_RUN_DIR "/");
	int i;

	if (f == NULL)
		return;
	StrArrayCreate(&targets, 8);
	while (fgets(line, sizeof(line), f) != NULL) {
		if (sscanf(line, "%*u %*u %*u:%*u %*s %4095s", target) != 1)
			continue;
		unescape_mount(target);
		if (strncmp(target, RUFUS_RUN_DIR "/", prefix_len) == 0)
			StrArrayAdd(&targets, target, TRUE);
	}
	fclose(f);
	/* Unmount in reverse order, so that stacked/nested mounts are released first */
	for (i = (int)targets.Index - 1; i >= 0; i--) {
		if (umount2(targets.String[i], 0) != 0 && umount2(targets.String[i], MNT_DETACH) != 0) {
			uprintf("Could not release stale mount '%s': %s", targets.String[i], strerror(errno));
			continue;
		}
		uprintf("Released stale mount '%s'", targets.String[i]);
		rmdir(targets.String[i]);
	}
	StrArrayDestroy(&targets);
}

/* Called once an operation has completed, so that the desktop can take over the drive again */
void linux_release_all_mounts(void)
{
	char letter;
	const char* mnt;
	for (letter = 'A'; letter <= 'Z'; letter++) {
		mnt = linux_get_drive_letter_mount(letter);
		if (mnt == NULL)
			continue;
		sync();
		unmount_path(mnt, TRUE);
		rmdir(mnt);
		linux_unmap_drive_letter(letter);
	}
	linux_release_image_mounts();
}

/* ------------------------------------------------------------------------- */
/* Automount inhibition                                                      */
/* ------------------------------------------------------------------------- */
BOOL linux_inhibit_automount(DWORD DriveIndex, BOOL inhibit)
{
	static char rule_path[128] = { 0 };
	static char inhibited_kname[64] = { 0 };
	const char* kname;
	FILE* f;

	if (inhibit) {
		kname = linux_get_disk_kname(DriveIndex);
		if (kname == NULL)
			return FALSE;
		mkdir("/run/udev/rules.d", 0755);
		snprintf(rule_path, sizeof(rule_path), "/run/udev/rules.d/90-rufus-%s.rules", kname);
		f = fopenU(rule_path, "w");
		if (f == NULL) {
			uprintf("Could not create '%s': %s", rule_path, strerror(errno));
			rule_path[0] = 0;
			return FALSE;
		}
		/* Prevent udisks (and therefore the desktop) from automounting this drive while we work on it */
		fprintf(f, "# Created by Rufus - removed when the current operation completes\n");
		fprintf(f, "SUBSYSTEM==\"block\", KERNEL==\"%s*\", ENV{UDISKS_IGNORE}=\"1\", ENV{UDISKS_AUTO}=\"0\"\n", kname);
		fclose(f);
		snprintf(inhibited_kname, sizeof(inhibited_kname), "%s", kname);
		linux_run((const char* []) { "udevadm", "control", "--reload", NULL }, FALSE);
		uprintf("Disabled automount for /dev/%s", kname);
		return TRUE;
	}
	if (rule_path[0] == 0)
		return TRUE;
	unlink(rule_path);
	rule_path[0] = 0;
	linux_run((const char* []) { "udevadm", "control", "--reload", NULL }, FALSE);
	if (inhibited_kname[0] != 0) {
		char dev[96];
		snprintf(dev, sizeof(dev), "/dev/%s", inhibited_kname);
		/* Let udisks and the desktop see the new partitions */
		linux_run((const char* []) { "udevadm", "trigger", "--action=change", "--subsystem-match=block", dev, NULL }, FALSE);
		uprintf("Restored automount for %s", dev);
		inhibited_kname[0] = 0;
	}
	return TRUE;
}

/* ------------------------------------------------------------------------- */
/* drive.h API                                                               */
/* ------------------------------------------------------------------------- */
BOOL SetAutoMount(BOOL enable)
{
	(void)enable;
	/* Automounting is handled per drive on Linux (see linux_inhibit_automount) */
	return TRUE;
}

BOOL GetAutoMount(BOOL* enabled)
{
	if (enabled == NULL)
		return FALSE;
	*enabled = TRUE;
	return TRUE;
}

/*
 * Exclusive locks held by this process. On Windows, a process that has locked a
 * physical drive can also lock its volumes. On Linux, an unpartitioned (super
 * floppy) drive's volume is the disk itself, and a second flock() on the same
 * device would fail, so we must recognize locks that we already hold.
 */
static struct { dev_t rdev; int fd; } held_lock[16];
static pthread_mutex_t held_lock_mutex = PTHREAD_MUTEX_INITIALIZER;

static BOOL is_lock_held_by_us(int fd)
{
	struct stat st, st2;
	BOOL r = FALSE;
	size_t i;

	if (fstat(fd, &st) != 0 || !S_ISBLK(st.st_mode))
		return FALSE;
	pthread_mutex_lock(&held_lock_mutex);
	for (i = 0; i < ARRAYSIZE(held_lock); i++) {
		if (held_lock[i].fd <= 0)
			continue;
		/* Prune the entries of handles that have since been closed */
		if (fstat(held_lock[i].fd, &st2) != 0 || st2.st_rdev != held_lock[i].rdev) {
			held_lock[i].fd = 0;
			continue;
		}
		if (held_lock[i].rdev == st.st_rdev)
			r = TRUE;
	}
	pthread_mutex_unlock(&held_lock_mutex);
	return r;
}

static void register_held_lock(int fd)
{
	struct stat st;
	size_t i;

	if (fstat(fd, &st) != 0 || !S_ISBLK(st.st_mode))
		return;
	pthread_mutex_lock(&held_lock_mutex);
	for (i = 0; i < ARRAYSIZE(held_lock); i++) {
		if (held_lock[i].fd <= 0) {
			held_lock[i].fd = fd;
			held_lock[i].rdev = st.st_rdev;
			break;
		}
	}
	pthread_mutex_unlock(&held_lock_mutex);
}

/*
 * Open a drive or volume with optional write and lock access.
 * Locking uses an exclusive BSD lock on the device, which udev honours, so that
 * the device isn't probed (and possibly automounted) while we modify it.
 */
HANDLE GetHandle(char* Path, BOOL bLockDrive, BOOL bWriteAccess, BOOL bWriteShare)
{
	int i, fd = -1;
	HANDLE hDrive = INVALID_HANDLE_VALUE;
	(void)bWriteShare;

	if (Path == NULL || Path[0] != '/')
		goto out;

	for (i = 0; i < DRIVE_ACCESS_RETRIES; i++) {
		fd = open(Path, (bWriteAccess ? O_RDWR : O_RDONLY) | O_CLOEXEC);
		if (fd >= 0)
			break;
		if (errno != EBUSY && errno != EAGAIN && errno != ENOENT && errno != ENXIO)
			break;
		if (i == 0)
			uprintf("Waiting for access on %s...", Path);
		Sleep(DRIVE_ACCESS_TIMEOUT / DRIVE_ACCESS_RETRIES);
	}
	if (fd < 0) {
		uprintf("Could not open %s: %s", Path, strerror(errno));
		SetLastError(ErrnoToWin32(errno));
		goto out;
	}
	if (bLockDrive && is_lock_held_by_us(fd)) {
		uuprintf("%s is already locked by this process", Path);
	} else if (bLockDrive) {
		for (i = 0; i < DRIVE_ACCESS_RETRIES; i++) {
			if (flock(fd, LOCK_EX | LOCK_NB) == 0)
				break;
			if (i == 0)
				uprintf("Waiting for exclusive access on %s...", Path);
			Sleep(DRIVE_ACCESS_TIMEOUT / DRIVE_ACCESS_RETRIES);
		}
		if (i >= DRIVE_ACCESS_RETRIES) {
			uprintf("Could not lock access to %s: %s", Path, strerror(errno));
			/* Report what's using the drive, like Windows does */
			GetProcessSearch(SEARCH_PROCESS_TIMEOUT, 0x07, FALSE);
			close(fd);
			SetLastError(ERROR_SHARING_VIOLATION);
			goto out;
		}
		register_held_lock(fd);
	}
	hDrive = FdToHandle(fd, Path);
out:
	return hDrive;
}

char* GetPhysicalName(DWORD DriveIndex)
{
	const char* path = NULL;
	CheckDriveIndex(DriveIndex);
	path = linux_get_disk_path(DriveIndex);
out:
	return safe_strdup(path);
}

/*
 * Return the block device of the partition starting at PartitionOffset, or of
 * the first partition if PartitionOffset is 0. Unpartitioned (super floppy)
 * drives are accessed through the whole disk.
 */
char* GetLogicalName(DWORD DriveIndex, uint64_t PartitionOffset, BOOL bKeepTrailingBackslash, BOOL bSilent)
{
	part_info_t parts[MAX_PARTITIONS * 2];
	const char* kname = NULL;
	char dev[96];
	int i, n;
	(void)bKeepTrailingBackslash;

	CheckDriveIndex(DriveIndex);
	kname = linux_get_disk_kname(DriveIndex);
	if (kname == NULL)
		goto out;
	n = list_partitions(kname, parts, ARRAYSIZE(parts));
	if (n == 0) {
		if (PartitionOffset != 0)
			goto out;
		snprintf(dev, sizeof(dev), "/dev/%s", kname);
		return safe_strdup(dev);
	}
	for (i = 0; i < n; i++) {
		if (PartitionOffset != 0 && parts[i].start != PartitionOffset)
			continue;
		snprintf(dev, sizeof(dev), "/dev/%s", parts[i].kname);
		/* Make sure the device node has been created */
		if (access(dev, F_OK) != 0) {
			suprintf("Device node %s is not available yet", dev);
			goto out;
		}
		if (!bSilent)
			uuprintf("Linux volume for this device: %s @%" PRIu64, dev, parts[i].start);
		return safe_strdup(dev);
	}
out:
	return NULL;
}

char* AltGetLogicalName(DWORD DriveIndex, uint64_t PartitionOffset, BOOL bKeepTrailingBackslash, BOOL bSilent)
{
	return GetLogicalName(DriveIndex, PartitionOffset, bKeepTrailingBackslash, bSilent);
}

char* GetExtPartitionName(DWORD DriveIndex, uint64_t PartitionOffset)
{
	return GetLogicalName(DriveIndex, PartitionOffset, FALSE, TRUE);
}

/* VDS equivalents */
BOOL IsVdsAvailable(BOOL bSilent)
{
	(void)bSilent;
	return TRUE;
}

BOOL ListVdsVolumes(BOOL bSilent)
{
	(void)bSilent;
	return TRUE;
}

BOOL VdsRescan(DWORD dwRescanType, DWORD dwSleepTime, BOOL bSilent)
{
	(void)dwRescanType; (void)bSilent;
	linux_run((const char* []) { "udevadm", "settle", "--timeout=10", NULL }, FALSE);
	Sleep(dwSleepTime);
	return TRUE;
}

BOOL RefreshLayout(DWORD DriveIndex)
{
	HANDLE hPhysical = GetPhysicalHandle(DriveIndex, FALSE, TRUE, TRUE);
	BOOL r;
	if (hPhysical == INVALID_HANDLE_VALUE)
		return FALSE;
	r = DeviceIoControl(hPhysical, IOCTL_DISK_UPDATE_PROPERTIES, NULL, 0, NULL, 0, NULL, NULL);
	CloseHandle(hPhysical);
	return r;
}

/* Delete all the partitions from a drive (the equivalent of VDS' partition deletion) */
BOOL DeletePartition(DWORD DriveIndex, ULONGLONG PartitionOffset, BOOL bSilent)
{
	HANDLE hPhysical;
	CREATE_DISK CreateDisk = { PARTITION_STYLE_RAW, { { 0 } } };
	const char* kname = linux_get_disk_kname(DriveIndex);
	BOOL r = FALSE;

	if (kname == NULL)
		return FALSE;
	if (PartitionOffset != 0) {
		suprintf("Deleting individual partitions is not supported");
		SetLastError(ERROR_NOT_SUPPORTED);
		return FALSE;
	}
	if (!unmount_disk(kname, bSilent))
		return FALSE;
	hPhysical = GetPhysicalHandle(DriveIndex, FALSE, TRUE, TRUE);
	if (hPhysical == INVALID_HANDLE_VALUE)
		return FALSE;
	suprintf("Deleting all partitions from /dev/%s", kname);
	r = DeviceIoControl(hPhysical, IOCTL_DISK_CREATE_DISK, &CreateDisk, sizeof(CreateDisk), NULL, 0, NULL, NULL) &&
		DeviceIoControl(hPhysical, IOCTL_DISK_UPDATE_PROPERTIES, NULL, 0, NULL, 0, NULL, NULL);
	CloseHandle(hPhysical);
	return r;
}

int GetDriveNumber(HANDLE hDrive, char* path)
{
	STORAGE_DEVICE_NUMBER sdn;
	DWORD size = 0;
	if (!DeviceIoControl(hDrive, IOCTL_STORAGE_GET_DEVICE_NUMBER, NULL, 0, &sdn, sizeof(sdn), &size, NULL)) {
		uuprintf("Could not get device number for device %s %s", path, WindowsErrorString());
		return -1;
	}
	return (int)sdn.DeviceNumber;
}

/* Drive letters are those that Rufus assigned to volumes from this drive */
BOOL GetDriveLetters(DWORD DriveIndex, char* drive_letters)
{
	const char* kname;
	char letter, mount_kname[64], disk[64];
	const char* mnt;
	int n = 0;
	BOOL r = FALSE;

	if (drive_letters != NULL)
		drive_letters[0] = 0;
	CheckDriveIndex(DriveIndex);
	kname = linux_get_disk_kname(DriveIndex);
	if (kname == NULL)
		goto out;
	for (letter = 'A'; letter <= 'Z'; letter++) {
		mnt = linux_get_drive_letter_mount(letter);
		if (mnt == NULL || !path_to_kname(mnt, mount_kname, sizeof(mount_kname)))
			continue;
		if (!kname_to_disk(mount_kname, disk, sizeof(disk)) || strcmp(disk, kname) != 0)
			continue;
		if (drive_letters != NULL)
			drive_letters[n++] = letter;
	}
	if (drive_letters != NULL)
		drive_letters[n] = 0;
	r = TRUE;
out:
	return r;
}

UINT GetDriveTypeFromIndex(DWORD DriveIndex)
{
	char path[128];
	const char* kname = linux_get_disk_kname(DriveIndex);
	if (kname == NULL)
		return DRIVE_UNKNOWN;
	snprintf(path, sizeof(path), "/sys/block/%s/removable", kname);
	return (read_sysfs_num(path) != 0) ? DRIVE_REMOVABLE : DRIVE_FIXED;
}

BOOL IsSourceImageLocatedOnTargetDrive(DWORD DriveIndex)
{
	char kname[64], disk[64];
	const char* target = linux_get_disk_kname(DriveIndex);
	if (boot_type != BT_IMAGE || image_path == NULL || target == NULL)
		return FALSE;
	if (!path_to_kname(image_path, kname, sizeof(kname)) || !kname_to_disk(kname, disk, sizeof(disk)))
		return FALSE;
	return (strcmp(disk, target) == 0);
}

/*
 * Release every volume of the drive (the Linux equivalent of removing the drive
 * letters, which makes Windows dismount the volumes) and return the drive letter
 * to use for our own mounting.
 */
char RemoveDriveLetters(DWORD DriveIndex, BOOL bReturnLast, BOOL bSilent)
{
	char drive_letters[27] = { 0 };
	const char* kname = linux_get_disk_kname(DriveIndex);
	int len;

	if (kname == NULL || !GetDriveLetters(DriveIndex, drive_letters)) {
		suprintf("Failed to get a drive letter");
		return 0;
	}
	if (IsSourceImageLocatedOnTargetDrive(DriveIndex)) {
		suprintf("ABORTED: Cannot use an image that is located on the target drive!");
		return 0;
	}
	len = (int)strlen(drive_letters);
	if (!unmount_disk(kname, bSilent))
		suprintf("WARNING: Some volumes from /dev/%s could not be unmounted", kname);
	if (len == 0) {
		suprintf("No drive letter was assigned...");
		return GetUnusedDriveLetter();
	}
	return drive_letters[bReturnLast ? (len - 1) : 0];
}

char GetUnusedDriveLetter(void)
{
	char letter;
	/* Like Windows, avoid A: and B: (and C:, which people associate with the system) */
	for (letter = 'D'; letter <= 'Z'; letter++)
		if (!IsDriveLetterInUse(letter))
			return letter;
	return 0;
}

BOOL IsDriveLetterInUse(const char drive_letter)
{
	return linux_is_drive_letter_pending(drive_letter) || (linux_get_drive_letter_mount(drive_letter) != NULL);
}

BOOL GetDriveLabel(DWORD DriveIndex, char* letters, char** label, BOOL bSilent)
{
	static char VolumeLabel[MAX_PATH + 1] = { 0 };
	char AutorunPath[MAX_PATH], *AutorunLabel = NULL, *dev;
	const char* mnt;

	*label = STR_NO_LABEL;
	if (!GetDriveLetters(DriveIndex, letters))
		return FALSE;
	if (letters[0] != 0) {
		/* We only care about an autorun.inf if we have a single volume */
		mnt = linux_get_drive_letter_mount(letters[0]);
		if (mnt != NULL) {
			snprintf(AutorunPath, sizeof(AutorunPath), "%s/autorun.inf", mnt);
			AutorunLabel = get_token_data_file("label", AutorunPath);
			if (AutorunLabel != NULL) {
				suprintf("Using 'autorun.inf' label for drive %c: '%s'", toupper(letters[0]), AutorunLabel);
				static_strcpy(VolumeLabel, AutorunLabel);
				safe_free(AutorunLabel);
				*label = VolumeLabel;
				return TRUE;
			}
		}
	}
	/* Label of the first partition (or of the whole disk for unpartitioned media) */
	dev = GetLogicalName(DriveIndex, 0, FALSE, TRUE);
	if (dev != NULL && blkid_probe(dev, "LABEL", VolumeLabel, sizeof(VolumeLabel)))
		*label = VolumeLabel;
	safe_free(dev);
	return TRUE;
}

static BOOL FlushDrive(char drive_letter)
{
	const char* mnt = linux_get_drive_letter_mount(drive_letter);
	int fd;
	BOOL r = FALSE;
	if (mnt == NULL)
		return FALSE;
	fd = open(mnt, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (fd >= 0) {
		r = (syncfs(fd) == 0);
		close(fd);
	}
	if (!r)
		uprintf("Failed to flush drive %c: %s", toupper(drive_letter), strerror(errno));
	return r;
}

BOOL UnmountVolume(HANDLE hDrive)
{
	char kname[64];
	const char* path = HandleToPath(hDrive);
	if (path == NULL || !path_to_kname(path, kname, sizeof(kname))) {
		uprintf("Could not unmount volume: invalid handle");
		return FALSE;
	}
	if (!linux_unmount_device(kname, FALSE)) {
		uprintf("Could not unmount volume: %s", WindowsErrorString());
		return FALSE;
	}
	return TRUE;
}

/* Mount the volume 'drive_guid' (a block device on Linux) as 'drive_name' ("X:\") */
BOOL MountVolume(char* drive_name, char* drive_guid)
{
	char kname[64];
	const char* mnt;
	char mnt_kname[64];

	if (drive_name == NULL || drive_guid == NULL || drive_name[0] == 0)
		return FALSE;
	/* Already mounted on the requested letter? */
	mnt = linux_get_drive_letter_mount(drive_name[0]);
	if (mnt != NULL && path_to_kname(drive_guid, kname, sizeof(kname)) &&
		path_to_kname(mnt, mnt_kname, sizeof(mnt_kname)) && strcmp(kname, mnt_kname) == 0) {
		uprintf("%s is already mounted as %c:", drive_guid, toupper(drive_name[0]));
		return TRUE;
	}
	/* Our private mount point must be the only active mount for this volume */
	if (path_to_kname(drive_guid, kname, sizeof(kname)))
		linux_unmount_device(kname, TRUE);
	return mount_volume(drive_guid, drive_name[0], FALSE);
}

/* Mount a partition on an unused letter, and return its name ("X:\") */
static BOOL pending_mount(char letter, const char* dev)
{
	return mount_volume(dev, letter, TRUE);
}

char* AltMountVolume(DWORD DriveIndex, uint64_t PartitionOffset, BOOL bSilent)
{
	char mounted_drive[] = "?:\\";
	char* volume_name;
	char kname[64];

	mounted_drive[0] = GetUnusedDriveLetter();
	if (mounted_drive[0] == 0) {
		suprintf("Could not find an unused drive letter");
		return NULL;
	}
	volume_name = AltGetLogicalName(DriveIndex, PartitionOffset, FALSE, bSilent);
	if (volume_name == NULL) {
		suprintf("Could not get a volume name for the partition @%" PRIu64, PartitionOffset);
		return NULL;
	}
	if (path_to_kname(volume_name, kname, sizeof(kname)))
		linux_unmount_device(kname, TRUE);
	if (!mount_volume(volume_name, mounted_drive[0], TRUE)) {
		/*
		 * Not having a file system yet is expected (we are about to format it). Windows
		 * assigns the drive letter to the RAW volume regardless, and it becomes usable
		 * once the volume has been formatted: we mount it when the drive letter is used.
		 */
		linux_set_pending_mount_callback(pending_mount);
		linux_reserve_drive_letter(mounted_drive[0], volume_name);
		suprintf("Assigned %c: to %s (to be mounted once formatted)", mounted_drive[0], volume_name);
	}
	free(volume_name);
	return safe_strdup(mounted_drive);
}

BOOL AltUnmountVolume(const char* drive_name, BOOL bSilent)
{
	const char* mnt;
	BOOL r;
	if (drive_name == NULL || drive_name[0] == 0)
		return FALSE;
	if (linux_is_drive_letter_pending(drive_name[0])) {
		/* Never mounted */
		linux_unmap_drive_letter(drive_name[0]);
		return TRUE;
	}
	mnt = linux_get_drive_letter_mount(drive_name[0]);
	if (mnt == NULL)
		return TRUE;
	r = unmount_path(mnt, bSilent);
	if (r) {
		rmdir(mnt);
		linux_unmap_drive_letter(drive_name[0]);
		suprintf("Successfully unmounted '%s'", drive_name);
	}
	return r;
}

/* Flush, unmount and remount a volume, to make sure everything has been written */
BOOL RemountVolume(char* drive_name, BOOL bSilent)
{
	const char* mnt;
	char kname[64], dev[96];

	if (drive_name == NULL || drive_name[0] == 0)
		return FALSE;
	mnt = linux_get_drive_letter_mount(drive_name[0]);
	if (mnt == NULL || !path_to_kname(mnt, kname, sizeof(kname))) {
		suprintf("Could not find the volume mounted as %c:", toupper(drive_name[0]));
		return FALSE;
	}
	snprintf(dev, sizeof(dev), "/dev/%s", kname);
	FlushDrive(drive_name[0]);
	if (!unmount_path(mnt, bSilent)) {
		ErrorStatus = RUFUS_ERROR(APPERR(ERROR_CANT_REMOUNT_VOLUME));
		return FALSE;
	}
	linux_unmap_drive_letter(drive_name[0]);
	if (!mount_volume(dev, drive_name[0], bSilent)) {
		uprintf("Could not remount %s as %c:", dev, toupper(drive_name[0]));
		ErrorStatus = RUFUS_ERROR(APPERR(ERROR_CANT_REMOUNT_VOLUME));
		return FALSE;
	}
	suprintf("Successfully remounted %s as %c:", dev, toupper(drive_name[0]));
	return TRUE;
}

BOOL IsMsDevDrive(DWORD DriveIndex)
{
	(void)DriveIndex;
	return FALSE;
}

/* ------------------------------------------------------------------------- */
/* Volume information                                                        */
/* ------------------------------------------------------------------------- */
/* Resolve a volume designation (device, mount point or drive letter) to a block device */
static BOOL volume_to_dev(const char* volume, char* dev, size_t size)
{
	char* path = linux_path(volume);
	struct stat st;
	char kname[64];
	BOOL r = FALSE;
	size_t len;

	if (path == NULL)
		return FALSE;
	len = strlen(path);
	while (len > 1 && path[len - 1] == '/')
		path[--len] = 0;
	if (stat(path, &st) != 0)
		goto out;
	if (S_ISBLK(st.st_mode)) {
		snprintf(dev, size, "%s", path);
		r = TRUE;
	} else if (path_to_kname(path, kname, sizeof(kname))) {
		snprintf(dev, size, "/dev/%s", kname);
		r = TRUE;
	}
out:
	free(path);
	return r;
}

BOOL GetVolumeInformationA(LPCSTR lpRootPathName, LPSTR lpVolumeNameBuffer, DWORD nVolumeNameSize,
	LPDWORD lpVolumeSerialNumber, LPDWORD lpMaximumComponentLength, LPDWORD lpFileSystemFlags,
	LPSTR lpFileSystemNameBuffer, DWORD nFileSystemNameSize)
{
	char dev[96], type[32], version[32], uuid[64];

	if (!volume_to_dev(lpRootPathName, dev, sizeof(dev)) || !blkid_probe(dev, "TYPE", type, sizeof(type))) {
		SetLastError(ERROR_UNRECOGNIZED_VOLUME);
		return FALSE;
	}
	if (lpFileSystemNameBuffer != NULL && nFileSystemNameSize > 0) {
		const char* name = type;
		if (strcmp(type, "vfat") == 0) {
			/* Windows reports "FAT32" or "FAT" (FAT12/FAT16) */
			name = (blkid_probe(dev, "VERSION", version, sizeof(version)) && strcmp(version, "FAT32") == 0) ? "FAT32" : "FAT";
		} else if (strcmp(type, "ntfs") == 0) {
			name = "NTFS";
		} else if (strcmp(type, "exfat") == 0) {
			name = "exFAT";
		} else if (strcmp(type, "udf") == 0) {
			name = "UDF";
		} else if (strcmp(type, "iso9660") == 0) {
			name = "CDFS";
		}
		snprintf(lpFileSystemNameBuffer, nFileSystemNameSize, "%s", name);
	}
	if (lpVolumeNameBuffer != NULL && nVolumeNameSize > 0) {
		if (!blkid_probe(dev, "LABEL", lpVolumeNameBuffer, nVolumeNameSize))
			lpVolumeNameBuffer[0] = 0;
	}
	if (lpVolumeSerialNumber != NULL) {
		*lpVolumeSerialNumber = 0;
		if (blkid_probe(dev, "UUID", uuid, sizeof(uuid))) {
			char hex[16] = { 0 };
			size_t i, j;
			for (i = 0, j = 0; uuid[i] != 0 && j < 8; i++)
				if (isxdigit((unsigned char)uuid[i]))
					hex[j++] = uuid[i];
			*lpVolumeSerialNumber = (DWORD)strtoul(hex, NULL, 16);
		}
	}
	if (lpMaximumComponentLength != NULL)
		*lpMaximumComponentLength = 255;
	if (lpFileSystemFlags != NULL)
		*lpFileSystemFlags = 0;
	return TRUE;
}

BOOL linux_get_volume_information(const char* root, char* label, DWORD label_size,
	DWORD* serial, DWORD* max_component, DWORD* fs_flags, char* fs_name, DWORD fs_name_size)
{
	return GetVolumeInformationA(root, label, label_size, serial, max_component, fs_flags, fs_name, fs_name_size);
}

/* Set the label of an (unmounted or mounted) volume */
BOOL SetVolumeLabelA(LPCSTR lpRootPathName, LPCSTR lpVolumeName)
{
	char dev[96], type[32];
	int r = -1;

	if (lpVolumeName == NULL)
		lpVolumeName = "";
	if (!volume_to_dev(lpRootPathName, dev, sizeof(dev)) || !blkid_probe(dev, "TYPE", type, sizeof(type))) {
		SetLastError(ERROR_UNRECOGNIZED_VOLUME);
		return FALSE;
	}
	if (strcmp(type, "vfat") == 0)
		r = linux_run((const char* []) { "fatlabel", dev, (lpVolumeName[0] == 0) ? "-r" : lpVolumeName, NULL }, TRUE);
	else if (strcmp(type, "exfat") == 0)
		r = linux_run((const char* []) { "exfatlabel", dev, lpVolumeName, NULL }, TRUE);
	else if (strcmp(type, "ntfs") == 0)
		r = linux_run((const char* []) { "ntfslabel", "--force", dev, lpVolumeName, NULL }, TRUE);
	else if (strncmp(type, "ext", 3) == 0)
		r = linux_run((const char* []) { "e2label", dev, lpVolumeName, NULL }, TRUE);
	if (r != 0) {
		SetLastError(ERROR_NOT_SUPPORTED);
		return FALSE;
	}
	return TRUE;
}
