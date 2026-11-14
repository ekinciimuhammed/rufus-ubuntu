/*
 * Rufus for Linux: file system formatting
 * Copyright © 2026 Rufus Linux port contributors
 *
 * On Windows, Rufus formats volumes through fmifs.dll's FormatEx() (or VDS),
 * and ext2/ext3 through its embedded copy of libext2fs. On Linux, the same
 * operations are carried out with the standard mkfs utilities, with the same
 * parameters Rufus would use (cluster size, label, quick/full format, NTFS
 * compression and indexing, ext block/inode sizes and features...).
 *
 * Large FAT32 (> 32 GB) volumes still go through Rufus' own format_fat32.c,
 * exactly as on Windows.
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
#include <msapi_utf8_linux.h>
#include <sys/ioctl.h>
#include <linux/fs.h>

#include "rufus.h"
#include "drive.h"
#include "format.h"
#include "missing.h"
#include "resource.h"
#include "settings.h"
#include "localization.h"
#include "linux_drive.h"

extern const char* FileSystemLabel[FS_MAX];
extern uint32_t dur_mins, dur_secs;
extern BOOL enable_file_indexing;

#define ZERO_BUFFER_SIZE        (4 * MB)

/* Remove our own (and any other) mounts of a volume before formatting it */
static void release_volume(const char* dev)
{
	const char* p = strrchr(dev, '/');
	linux_unmount_device((p == NULL) ? dev : p + 1, TRUE);
}

static uint64_t get_device_size(const char* dev, uint32_t* sector_size)
{
	int fd = open(dev, O_RDONLY | O_CLOEXEC), ss = 512;
	uint64_t size = 0;
	if (fd < 0)
		return 0;
	if (ioctl(fd, BLKGETSIZE64, &size) != 0)
		size = 0;
	if (ioctl(fd, BLKSSZGET, &ss) != 0 || ss < 512)
		ss = 512;
	close(fd);
	if (sector_size != NULL)
		*sector_size = (uint32_t)ss;
	return size;
}

/* Starting sector of a partition (used as the "hidden sectors" value) */
static uint64_t get_partition_start(const char* dev)
{
	char path[256], buf[64];
	const char* kname = strrchr(dev, '/');
	FILE* f;
	uint64_t start = 0;
	snprintf(path, sizeof(path), "/sys/class/block/%s/start", (kname == NULL) ? dev : kname + 1);
	f = fopenU(path, "r");
	if (f != NULL) {
		if (fgets(buf, sizeof(buf), f) != NULL)
			start = strtoull(buf, NULL, 10);
		fclose(f);
	}
	return start;
}

/*
 * The equivalent of a Windows "full" (non quick) format: every sector of the
 * volume gets written, which also exposes bad media.
 */
static BOOL zero_volume(const char* dev, uint64_t size)
{
	uint8_t* buf = NULL;
	uint64_t done = 0;
	ssize_t w;
	size_t chunk;
	int fd;
	BOOL r = FALSE;

	fd = open(dev, O_WRONLY | O_CLOEXEC);
	if (fd < 0) {
		uprintf("Could not open %s: %s", dev, strerror(errno));
		return FALSE;
	}
	if (posix_memalign((void**)&buf, 4096, ZERO_BUFFER_SIZE) != 0)
		goto out;
	memset(buf, 0, ZERO_BUFFER_SIZE);
	UpdateProgressWithInfoInit(NULL, TRUE);
	while (done < size) {
		if (IS_ERROR(ErrorStatus))
			goto out;
		chunk = (size_t)min((uint64_t)ZERO_BUFFER_SIZE, size - done);
		w = pwrite(fd, buf, chunk, (off_t)done);
		if (w < 0 && errno == EINTR)
			continue;
		if (w <= 0) {
			uprintf("Write error at offset %" PRIu64 ": %s", done, strerror(errno));
			ErrorStatus = RUFUS_ERROR(ERROR_WRITE_FAULT);
			goto out;
		}
		done += (uint64_t)w;
		UpdateProgressWithInfo(OP_FORMAT, MSG_217, done, size);
	}
	if (fdatasync(fd) != 0) {
		ErrorStatus = RUFUS_ERROR(ERROR_WRITE_FAULT);
		goto out;
	}
	r = TRUE;
out:
	free(buf);
	close(fd);
	return r;
}

/* Run a mkfs utility, reporting its output to the log */
static BOOL run_mkfs(const char* const argv[])
{
	char cmd[1024] = "";
	int i, r;
	for (i = 0; argv[i] != NULL; i++) {
		static_strcat(cmd, argv[i]);
		static_strcat(cmd, " ");
	}
	uprintf("Running command: '%s'", cmd);
	r = linux_run(argv, TRUE);
	if (r != 0) {
		uprintf("%s failed (exit code %d)", argv[0], r);
		return FALSE;
	}
	return TRUE;
}

/*
 * Linux replacement for format.c's FormatNative(): same parameters, messages and
 * semantics (default cluster size below 512 bytes, quick vs full format, retries).
 */
BOOL FormatNativeLinux(DWORD DriveIndex, uint64_t PartitionOffset, DWORD ClusterSize, LPCSTR FSName, LPCSTR Label, DWORD Flags)
{
	BOOL r = FALSE;
	char *VolumeName = NULL, cluster_str[32], sectors_str[32], hidden_str[32], label_str[64];
	uint32_t sector_size = 512;
	uint64_t size;
	size_t i;
	const char* argv[24];
	int n = 0;

	if ((strcmp(FSName, FileSystemLabel[FS_EXFAT]) == 0) && !((dur_mins == 0) && (dur_secs == 0))) {
		PrintInfo(0, MSG_220, FSName, dur_mins, dur_secs);
	} else {
		PrintInfo(0, MSG_222, FSName);
	}
	uprintf("Formatting to %s (using mkfs)", FSName);

	VolumeName = GetLogicalName(DriveIndex, PartitionOffset, TRUE, TRUE);
	if (VolumeName == NULL) {
		uprintf("Could not read volume name");
		ErrorStatus = RUFUS_ERROR(ERROR_GEN_FAILURE);
		goto out;
	}
	release_volume(VolumeName);
	size = get_device_size(VolumeName, &sector_size);
	if (size == 0) {
		uprintf("Could not get the size of %s", VolumeName);
		ErrorStatus = RUFUS_ERROR(ERROR_INVALID_VOLUME_SIZE);
		goto out;
	}

	if (ClusterSize < 0x200) {
		ClusterSize = 0;
		uprintf("Using default cluster size");
	} else {
		uprintf("Using cluster size: %d bytes", ClusterSize);
	}
	uprintf("%s format was selected", (Flags & FP_QUICK) ? "Quick" : "Slow");

	/* A slow format writes the whole volume, like Windows' FormatEx() does */
	if (!(Flags & FP_QUICK)) {
		if (!zero_volume(VolumeName, size))
			goto out;
		UpdateProgress(OP_FORMAT, 100.0f);
	}
	CHECK_FOR_USER_CANCEL;

	static_sprintf(label_str, "%s", (Label == NULL) ? "" : Label);
	if (strcmp(FSName, FileSystemLabel[FS_FAT16]) == 0 || strcmp(FSName, FileSystemLabel[FS_FAT32]) == 0) {
		BOOL is_fat32 = (strcmp(FSName, FileSystemLabel[FS_FAT32]) == 0);
		argv[n++] = "mkfs.fat";
		argv[n++] = "-F";
		argv[n++] = is_fat32 ? "32" : "16";
		static_sprintf(sectors_str, "%u", sector_size);
		argv[n++] = "-S";
		argv[n++] = sectors_str;
		if (ClusterSize != 0) {
			static_sprintf(cluster_str, "%u", ClusterSize / sector_size);
			argv[n++] = "-s";
			argv[n++] = cluster_str;
		}
		/* Hidden sectors = partition start, as Windows sets them */
		static_sprintf(hidden_str, "%" PRIu64, get_partition_start(VolumeName) * 512 / sector_size);
		argv[n++] = "-h";
		argv[n++] = hidden_str;
		argv[n++] = "-D";
		argv[n++] = "0x80";
		argv[n++] = "-M";
		argv[n++] = "0xF8";
		/* Formatting a whole (unpartitioned) device */
		argv[n++] = "-I";
		if (label_str[0] != 0) {
			argv[n++] = "-n";
			argv[n++] = label_str;
		}
		argv[n++] = VolumeName;
	} else if (strcmp(FSName, FileSystemLabel[FS_NTFS]) == 0) {
		argv[n++] = "mkfs.ntfs";
		argv[n++] = "-F";
		if (Flags & FP_QUICK)
			argv[n++] = "-Q";
		static_sprintf(sectors_str, "%u", sector_size);
		argv[n++] = "-s";
		argv[n++] = sectors_str;
		if (ClusterSize != 0) {
			static_sprintf(cluster_str, "%u", ClusterSize);
			argv[n++] = "-c";
			argv[n++] = cluster_str;
		}
		static_sprintf(hidden_str, "%" PRIu64, get_partition_start(VolumeName) * 512 / sector_size);
		argv[n++] = "-p";
		argv[n++] = hidden_str;
		/* Same geometry as what Windows reports */
		argv[n++] = "-H";
		argv[n++] = "255";
		argv[n++] = "-S";
		argv[n++] = "63";
		if (Flags & FP_COMPRESSION)
			argv[n++] = "-C";
		if (!enable_file_indexing)
			argv[n++] = "-I";
		if (label_str[0] != 0) {
			argv[n++] = "-L";
			argv[n++] = label_str;
		}
		argv[n++] = VolumeName;
	} else if (strcmp(FSName, FileSystemLabel[FS_EXFAT]) == 0) {
		argv[n++] = "mkfs.exfat";
		if (ClusterSize != 0) {
			static_sprintf(cluster_str, "%u", ClusterSize);
			argv[n++] = "-c";
			argv[n++] = cluster_str;
		}
		if (label_str[0] != 0) {
			argv[n++] = "-L";
			argv[n++] = label_str;
		}
		argv[n++] = VolumeName;
	} else if (strcmp(FSName, FileSystemLabel[FS_UDF]) == 0) {
		static char udf_rev[16], blocksize[32], lvid[160], vid[160];
		uint32_t udf_version = ReadSetting32(SETTING_USE_UDF_VERSION);
		/* Windows formats UDF 2.01 by default */
		if (udf_version < 0x102 || udf_version > 0x260)
			udf_version = 0x201;
		static_sprintf(udf_rev, "0x%04x", udf_version);
		static_sprintf(blocksize, "--blocksize=%u", sector_size);
		argv[n++] = "mkudffs";
		argv[n++] = "--media-type=hd";
		argv[n++] = blocksize;
		argv[n++] = "--udfrev";
		argv[n++] = udf_rev;
		if (label_str[0] != 0) {
			static_sprintf(lvid, "--lvid=%s", label_str);
			static_sprintf(vid, "--vid=%.30s", label_str);
			argv[n++] = lvid;
			argv[n++] = vid;
		}
		argv[n++] = VolumeName;
	} else {
		uprintf("%s is not supported on Linux", FSName);
		ErrorStatus = RUFUS_ERROR(APPERR(ERROR_INCOMPATIBLE_FS));
		goto out;
	}
	argv[n] = NULL;

	for (i = 0; i < WRITE_RETRIES; i++) {
		if (run_mkfs(argv))
			break;
		if (IS_ERROR(ErrorStatus))
			goto out;
		uprintf("Retrying...");
		Sleep(WRITE_TIMEOUT);
	}
	if (i >= WRITE_RETRIES) {
		ErrorStatus = RUFUS_ERROR(ERROR_WRITE_FAULT);
		goto out;
	}
	UpdateProgress(OP_CREATE_FS, 100.0f);
	if (Flags & FP_COMPRESSION)
		uprintf("Enabled NTFS compression");

	if (!IS_ERROR(ErrorStatus)) {
		uprintf("Format completed.");
		r = TRUE;
	}

out:
	if (!r && !IS_ERROR(ErrorStatus))
		ErrorStatus = RUFUS_ERROR(ERROR_GEN_FAILURE);
	safe_free(VolumeName);
	return r;
}

/*
 * Windows runs chkdsk on NTFS media created from ISOs, so that the drive is
 * properly recognized by WinPE. A freshly created ntfs3/ntfs-3g volume is clean,
 * so we just make sure the volume is not flagged as dirty.
 */
BOOL CheckDiskLinux(char DriveLetter)
{
	char drive_name[] = "?:\\", dev[96];
	const char* mnt;
	const char* p;
	FILE* f;
	char line[1024];
	BOOL found = FALSE;

	drive_name[0] = DriveLetter;
	PrintInfoDebug(0, MSG_223);
	mnt = linux_get_drive_letter_mount(DriveLetter);
	if (mnt == NULL)
		return FALSE;
	/* Find the device for the mount point */
	f = fopenU("/proc/self/mounts", "r");
	if (f == NULL)
		return FALSE;
	while (fgets(line, sizeof(line), f) != NULL) {
		char src[512], target[512];
		if (sscanf(line, "%511s %511s", src, target) == 2 && strcmp(target, mnt) == 0) {
			static_strcpy(dev, src);
			found = TRUE;
			break;
		}
	}
	fclose(f);
	if (!found)
		return FALSE;
	/*
	 * ntfsfix can't check a mounted volume, and the volume must stay mounted for the
	 * next steps. The Linux NTFS drivers mark the volume clean when it is unmounted,
	 * at the end of the operation, so flushing it is all that is needed here.
	 */
	sync();
	p = dev;
	uprintf("Flushed NTFS volume %s", p);
	uprintf("NTFS Fixup completed.");
	return TRUE;
}

/* Same defaults as Rufus' embedded ext2fs formatter (taken from mke2fs.conf) */
typedef struct {
	uint64_t max_size;
	uint32_t block_size;
	uint32_t inode_size;
	uint32_t inode_ratio;
} ext2fs_default_t;

/* e2fsprogs' ext2fs_default_journal_size() */
static int default_journal_size(uint64_t num_blocks)
{
	if (num_blocks < 2048)
		return -1;
	if (num_blocks < 32768)
		return 1024;
	if (num_blocks < 256 * 1024)
		return 4096;
	if (num_blocks < 512 * 1024)
		return 8192;
	if (num_blocks < 4096 * 1024)
		return 16384;
	if (num_blocks < 8192 * 1024)
		return 32768;
	if (num_blocks < 16384 * 1024)
		return 65536;
	if (num_blocks < 32768 * 1024)
		return 131072;
	return 262144;
}

BOOL FormatExtFs(DWORD DriveIndex, uint64_t PartitionOffset, DWORD BlockSize, LPCSTR FSName, LPCSTR Label, DWORD Flags)
{
	const float reserve_ratio = 0.05f;
	const ext2fs_default_t ext2fs_default[5] = {
		{ 3 * MB, 1024, 128, 3},	// "floppy"
		{ 512 * MB, 1024, 128, 2},	// "small"
		{ 4 * GB, 4096, 256, 2},	// "default"
		{ 16 * GB, 4096, 256, 3},	// "big"
		{ 1024 * TB, 4096, 256, 4}	// "huge"
	};
	BOOL ret = FALSE;
	char* volume_name = NULL;
	char tmp_dir[MAX_PATH] = "", conf_path[MAX_PATH];
	char block_str[16], inode_size_str[16], inodes_str[32], journal_str[64], extended[64], features[160];
	char blocks_str[32];
	const char* argv[32];
	uint64_t size, blocks, inodes;
	int i, n = 0, journal_size;

	volume_name = GetExtPartitionName(DriveIndex, PartitionOffset);
	if ((volume_name == NULL) || (strlen(FSName) != 4) || (strncmp(FSName, "ext", 3) != 0)) {
		ErrorStatus = RUFUS_ERROR(ERROR_INVALID_PARAMETER);
		goto out;
	}
	if ((strcmp(FSName, FileSystemLabel[FS_EXT2]) != 0) && (strcmp(FSName, FileSystemLabel[FS_EXT3]) != 0)) {
		if (strcmp(FSName, FileSystemLabel[FS_EXT4]) == 0)
			uprintf("ext4 file system is not supported, defaulting to ext3");
		else
			uprintf("Invalid ext file system version requested, defaulting to ext3");
		FSName = FileSystemLabel[FS_EXT3];
	}
	PrintInfoDebug(0, MSG_222, FSName);
	UpdateProgressWithInfoInit(NULL, TRUE);
	release_volume(volume_name);

	size = get_device_size(volume_name, NULL);
	if (size == 0) {
		ErrorStatus = RUFUS_ERROR(ERROR_READ_FAULT);
		uprintf("Could not read device size");
		goto out;
	}
	for (i = 0; i < (int)ARRAYSIZE(ext2fs_default); i++) {
		if (size < ext2fs_default[i].max_size)
			break;
	}
	assert(i < (int)ARRAYSIZE(ext2fs_default));
	if ((BlockSize == 0) || (BlockSize < 1024))
		BlockSize = ext2fs_default[i].block_size;
	assert(IS_POWER_OF_2(BlockSize));
	blocks = size / BlockSize;
	if (blocks >= 0x100000000ULL) {
		ErrorStatus = RUFUS_ERROR(ERROR_INVALID_VOLUME_SIZE);
		uprintf("Volume size is too large for ext2 or ext3");
		goto out;
	}
	inodes = blocks >> ext2fs_default[i].inode_ratio;
	if (inodes > UINT32_MAX)
		inodes = UINT32_MAX;
	uprintf("%d possible inodes out of %lld blocks (block size = %d)", (int)inodes, (long long)blocks, BlockSize);
	uprintf("%lld blocks (%0.1f%%) reserved for the super user", (long long)(reserve_ratio * blocks), reserve_ratio * 100.0f);

	/* persistence.conf for Debian Live persistence (mke2fs populates the root from a directory) */
	if (Flags & FP_CREATE_PERSISTENCE_CONF) {
		FILE* f;
		static_sprintf(tmp_dir, "%s/rufus-persistence-XXXXXX", temp_dir);
		if (mkdtemp(tmp_dir) == NULL) {
			uprintf("Could not create temporary directory: %s", strerror(errno));
			tmp_dir[0] = 0;
		} else {
			static_sprintf(conf_path, "%s/persistence.conf", tmp_dir);
			f = fopenU(conf_path, "wb");
			// You *do* want the LF at the end of the "/ union" line, else Debian Live bails out...
			if (f == NULL || fwrite("/ union\n", 1, 8, f) != 8)
				uprintf("Error: Could not create 'persistence.conf' file");
			else
				uprintf("Created 'persistence.conf' file");
			if (f != NULL) {
				fclose(f);
				chmod(conf_path, 0644);
			}
		}
	}

	static_sprintf(block_str, "%u", BlockSize);
	static_sprintf(inode_size_str, "%u", ext2fs_default[i].inode_size);
	static_sprintf(inodes_str, "%" PRIu64, inodes);
	static_sprintf(blocks_str, "%" PRIu64, blocks);
	static_sprintf(features, "none,dir_index,filetype,large_file,sparse_super,ext_attr%s",
		(FSName[3] != '2') ? ",has_journal" : "");
	static_sprintf(extended, "lazy_itable_init=0,lazy_journal_init=%d", (Flags & FP_QUICK) ? 1 : 0);
	argv[n++] = "mke2fs";
	argv[n++] = "-F";
	argv[n++] = "-q";
	argv[n++] = "-t";
	argv[n++] = FSName;
	argv[n++] = "-b";
	argv[n++] = block_str;
	argv[n++] = "-I";
	argv[n++] = inode_size_str;
	argv[n++] = "-N";
	argv[n++] = inodes_str;
	argv[n++] = "-m";
	argv[n++] = "5";
	argv[n++] = "-O";
	argv[n++] = features;
	argv[n++] = "-E";
	argv[n++] = extended;
	argv[n++] = "-e";
	argv[n++] = "continue";
	if (FSName[3] != '2') {
		journal_size = default_journal_size(blocks);
		if (journal_size > 0) {
			/* Rufus halves the default journal size (in blocks) */
			journal_size /= 2;
			static_sprintf(journal_str, "size=%u", (unsigned)max(1ULL, ((uint64_t)journal_size * BlockSize) / MB));
			uprintf("Creating %d journal blocks", journal_size);
			argv[n++] = "-J";
			argv[n++] = journal_str;
		}
	}
	if (Label != NULL && Label[0] != 0) {
		argv[n++] = "-L";
		argv[n++] = Label;
	}
	if (tmp_dir[0] != 0) {
		argv[n++] = "-d";
		argv[n++] = tmp_dir;
	}
	argv[n++] = volume_name;
	argv[n++] = blocks_str;
	argv[n] = NULL;

	if (!run_mkfs(argv)) {
		ErrorStatus = RUFUS_ERROR(ERROR_WRITE_FAULT);
		uprintf("Could not create %s volume", FSName);
		goto out;
	}
	/* Same tunables as Rufus sets: no maximum mount count, half MD4 directory hash */
	linux_run((const char* []) { "tune2fs", "-c", "-1", "-E", "hash_alg=half_md4", "-o", "user_xattr,acl", volume_name, NULL }, FALSE);
	UpdateProgressWithInfo(OP_FORMAT, MSG_217, 100, 100);
	ret = TRUE;

out:
	if (tmp_dir[0] != 0) {
		static_sprintf(conf_path, "%s/persistence.conf", tmp_dir);
		unlink(conf_path);
		rmdir(tmp_dir);
	}
	safe_free(volume_name);
	return ret;
}

/* Return the label of an ext file system (NULL if not ext or no label) */
const char* GetExtFsLabel(DWORD DriveIndex, uint64_t PartitionOffset)
{
	static char label[64];
	char type[32];
	char* volume_name = GetExtPartitionName(DriveIndex, PartitionOffset);
	const char* argv_type[] = { "blkid", "-p", "-s", "TYPE", "-o", "value", volume_name, NULL };
	const char* argv_label[] = { "blkid", "-p", "-s", "LABEL", "-o", "value", volume_name, NULL };
	const char* r = NULL;

	if (volume_name == NULL)
		return NULL;
	if (!linux_run_capture(argv_type, type, sizeof(type)) || strncmp(type, "ext", 3) != 0)
		goto out;
	if (linux_run_capture(argv_label, label, sizeof(label))) {
		label[strcspn(label, "\r\n")] = 0;
		if (label[0] != 0)
			r = label;
	}
out:
	free(volume_name);
	return r;
}
