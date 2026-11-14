/*
 * Rufus for Linux: virtual disk (VHD/VHDX) support
 * Copyright © 2026 Rufus Linux port contributors
 *
 * Linux versions of the virtdisk.dll based functions from src/vhd.c:
 * - Using VHD/VHDX files as a source for DD writing: fixed VHDs are used
 *   directly (their data is a raw disk image followed by a 512 byte footer),
 *   dynamic VHDs and VHDXs are attached read-only through qemu-nbd.
 * - Saving a drive to VHD (fixed, i.e. uncompressed and usable as a DD image,
 *   exactly like Windows' CREATE_VIRTUAL_DISK_FLAG_FULL_PHYSICAL_ALLOCATION)
 *   or to VHDX (dynamic, through qemu-img).
 * - FFU images rely on Windows' DISM, and are not available on Linux.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */
#include <windows.h>
#include <msapi_utf8_linux.h>
#include <endian.h>
#include <sys/ioctl.h>
#include <linux/fs.h>

#include "rufus.h"
#include "vhd.h"
#include "drive.h"
#include "missing.h"
#include "resource.h"
#include "localization.h"
#include "linux_drive.h"

extern BOOL linux_run_capture(const char* const argv[], char* out, size_t out_size);

#define VHD_FOOTER_COOKIE       "conectix"
#define VHD_DISK_TYPE_FIXED     2
#define VHD_DISK_TYPE_DYNAMIC   3
#define VHD_DISK_TYPE_DIFF      4
#define SAVE_BUFFER_SIZE        (32 * MB)

#pragma pack(push, 1)
typedef struct {
	char cookie[8];
	uint32_t features;
	uint32_t file_format_version;
	uint64_t data_offset;
	uint32_t timestamp;
	char creator_app[4];
	uint32_t creator_version;
	uint32_t creator_host_os;
	uint64_t original_size;
	uint64_t current_size;
	uint16_t cylinders;
	uint8_t heads;
	uint8_t sectors_per_track;
	uint32_t disk_type;
	uint32_t checksum;
	uint8_t unique_id[16];
	uint8_t saved_state;
	uint8_t reserved[427];
} vhd_footer_t;
#pragma pack(pop)
_Static_assert(sizeof(vhd_footer_t) == 512, "invalid VHD footer size");

static char physical_path[128] = "";
static char nbd_device[32] = "";

static uint32_t vhd_checksum(const vhd_footer_t* footer)
{
	const uint8_t* p = (const uint8_t*)footer;
	uint32_t sum = 0;
	size_t i;
	for (i = 0; i < sizeof(*footer); i++)
		if (i < offsetof(vhd_footer_t, checksum) || i >= offsetof(vhd_footer_t, checksum) + 4)
			sum += p[i];
	return ~sum;
}

static BOOL read_vhd_footer(const char* path, vhd_footer_t* footer, uint64_t* file_size)
{
	FILE* f = fopenU(path, "rb");
	BOOL r = FALSE;
	if (f == NULL)
		return FALSE;
	if (fseeko(f, 0, SEEK_END) == 0) {
		*file_size = (uint64_t)ftello(f);
		if (*file_size >= sizeof(*footer) && fseeko(f, -(off_t)sizeof(*footer), SEEK_END) == 0 &&
			fread(footer, sizeof(*footer), 1, f) == 1 && memcmp(footer->cookie, VHD_FOOTER_COOKIE, 8) == 0)
			r = TRUE;
	}
	fclose(f);
	return r;
}

/* Find a qemu-nbd binary and a free nbd device */
static BOOL attach_nbd(const char* path, const char* format, char* dev, size_t dev_size)
{
	char sysfs[64], pid[32];
	int i;
	linux_run((const char* []) { "modprobe", "nbd", "max_part=16", NULL }, FALSE);
	for (i = 0; i < 16; i++) {
		snprintf(sysfs, sizeof(sysfs), "/sys/block/nbd%d/pid", i);
		/* A connected nbd device has a pid attribute */
		if (access(sysfs, F_OK) == 0)
			continue;
		snprintf(dev, dev_size, "/dev/nbd%d", i);
		if (access(dev, F_OK) != 0)
			continue;
		if (linux_run((const char* []) { "qemu-nbd", "--read-only", "--format", format, "--connect", dev, path, NULL }, TRUE) == 0) {
			/*
			 * Wait for the device to be ready: the connection is established (and the pid
			 * attribute created) before the kernel has been given the size of the device.
			 */
			for (int j = 0; j < 100; j++) {
				uint64_t size = 0;
				int fd = open(dev, O_RDONLY | O_CLOEXEC);
				if (fd >= 0) {
					if (ioctl(fd, BLKGETSIZE64, &size) != 0)
						size = 0;
					close(fd);
				}
				if (size != 0)
					return TRUE;
				Sleep(100);
			}
			uprintf("Timeout while waiting for %s to be ready", dev);
			linux_run((const char* []) { "qemu-nbd", "--disconnect", dev, NULL }, FALSE);
			return FALSE;
		}
		(void)pid;
		return FALSE;
	}
	return FALSE;
}

/*
 * Return the path of a raw (block) view of a VHD/VHDX image, as well as its
 * virtual disk size.
 */
char* VhdMountImageAndGetSize(const char* path, uint64_t* disk_size)
{
	vhd_footer_t footer;
	uint64_t file_size = 0, size = 0;
	char* lpath = linux_path(path);
	const char* ext;
	int fd;

	if (lpath == NULL)
		return NULL;
	VhdUnmountImage();
	ext = strrchr(lpath, '.');
	if (ext != NULL && strcasecmp(ext, ".vhd") == 0 && read_vhd_footer(lpath, &footer, &file_size)) {
		if (be32toh(footer.disk_type) == VHD_DISK_TYPE_FIXED) {
			/* A fixed VHD is a raw image with a footer */
			static_strcpy(physical_path, lpath);
			if (disk_size != NULL)
				*disk_size = min(be64toh(footer.current_size), file_size - sizeof(footer));
			free(lpath);
			return physical_path;
		}
	}
	if (!attach_nbd(lpath, (ext != NULL && strcasecmp(ext, ".vhdx") == 0) ? "vhdx" : "vpc", nbd_device, sizeof(nbd_device))) {
		uprintf("Could not mount '%s': dynamic VHD and VHDX images require qemu-nbd (package 'qemu-utils')", lpath);
		nbd_device[0] = 0;
		free(lpath);
		SetLastError(ERROR_NOT_SUPPORTED);
		return NULL;
	}
	free(lpath);
	static_strcpy(physical_path, nbd_device);
	fd = open(physical_path, O_RDONLY | O_CLOEXEC);
	if (fd >= 0) {
		if (ioctl(fd, BLKGETSIZE64, &size) != 0)
			size = 0;
		close(fd);
	}
	if (disk_size != NULL)
		*disk_size = size;
	uprintf("Mounted virtual disk as %s", physical_path);
	return physical_path;
}

void VhdUnmountImage(void)
{
	if (nbd_device[0] != 0) {
		linux_run((const char* []) { "qemu-nbd", "--disconnect", nbd_device, NULL }, FALSE);
		nbd_device[0] = 0;
	}
	physical_path[0] = 0;
}

/* CHS geometry as defined in the VHD specifications */
static void vhd_geometry(uint64_t size, vhd_footer_t* footer)
{
	uint64_t total = size / 512, cyl_times_heads;
	uint32_t heads, spt;
	if (total > 65535ULL * 16 * 255)
		total = 65535ULL * 16 * 255;
	if (total >= 65535ULL * 16 * 63) {
		spt = 255;
		heads = 16;
		cyl_times_heads = total / spt;
	} else {
		spt = 17;
		cyl_times_heads = total / spt;
		heads = (uint32_t)((cyl_times_heads + 1023) / 1024);
		if (heads < 4)
			heads = 4;
		if (cyl_times_heads >= (heads * 1024ULL) || heads > 16) {
			spt = 31;
			heads = 16;
			cyl_times_heads = total / spt;
		}
		if (cyl_times_heads >= (heads * 1024ULL)) {
			spt = 63;
			heads = 16;
			cyl_times_heads = total / spt;
		}
	}
	footer->cylinders = htobe16((uint16_t)(cyl_times_heads / heads));
	footer->heads = (uint8_t)heads;
	footer->sectors_per_track = (uint8_t)spt;
}

static BOOL save_fixed_vhd(IMG_SAVE* img_save)
{
	int in = -1, out = -1;
	uint8_t* buf = NULL;
	uint64_t done = 0, total = (uint64_t)img_save->DeviceSize;
	ssize_t r, w, wd;
	vhd_footer_t footer;
	GUID guid;
	BOOL ret = FALSE;
	char* dst = linux_path(img_save->ImagePath);

	in = open(img_save->DevicePath, O_RDONLY | O_CLOEXEC);
	if (in < 0) {
		uprintf("Could not open %s: %s", img_save->DevicePath, strerror(errno));
		SetLastError(ErrnoToWin32(errno));
		goto out;
	}
	out = open(dst, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (out < 0) {
		uprintf("Could not create virtual disk: %s", strerror(errno));
		SetLastError(ErrnoToWin32(errno));
		goto out;
	}
	if (posix_memalign((void**)&buf, 4096, SAVE_BUFFER_SIZE) != 0)
		goto out;
	while (done < total) {
		if (IS_ERROR(ErrorStatus) && (SCODE_CODE(ErrorStatus) == ERROR_CANCELLED))
			goto out;
		r = pread(in, buf, (size_t)min((uint64_t)SAVE_BUFFER_SIZE, total - done), (off_t)done);
		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0) {
			uprintf("Read error at offset %" PRIu64 ": %s", done, (r == 0) ? "Unexpected end of device" : strerror(errno));
			SetLastError(ERROR_READ_FAULT);
			goto out;
		}
		for (wd = 0; wd < r; wd += w) {
			w = write(out, buf + wd, (size_t)(r - wd));
			if (w < 0 && errno == EINTR) {
				w = 0;
				continue;
			}
			if (w <= 0) {
				uprintf("Write error: %s", strerror(errno));
				SetLastError((errno == ENOSPC) ? ERROR_DISK_FULL : ERROR_WRITE_FAULT);
				goto out;
			}
		}
		done += (uint64_t)r;
		UpdateProgressWithInfo(OP_FORMAT, MSG_261, done, total);
	}
	memset(&footer, 0, sizeof(footer));
	memcpy(footer.cookie, VHD_FOOTER_COOKIE, 8);
	footer.features = htobe32(0x00000002);
	footer.file_format_version = htobe32(0x00010000);
	footer.data_offset = htobe64(0xFFFFFFFFFFFFFFFFULL);
	footer.timestamp = htobe32((uint32_t)(time(NULL) - 946684800));	/* Seconds since 2000.01.01 */
	memcpy(footer.creator_app, "rufs", 4);
	footer.creator_version = htobe32(0x00040010);
	footer.creator_host_os = htobe32(0x5769326B);	/* "Wi2k", the only value Windows recognizes */
	footer.original_size = htobe64(total);
	footer.current_size = htobe64(total);
	vhd_geometry(total, &footer);
	footer.disk_type = htobe32(VHD_DISK_TYPE_FIXED);
	CoCreateGuid(&guid);
	memcpy(footer.unique_id, &guid, sizeof(footer.unique_id));
	footer.checksum = htobe32(vhd_checksum(&footer));
	if (write(out, &footer, sizeof(footer)) != sizeof(footer) || fsync(out) != 0) {
		uprintf("Could not write VHD footer: %s", strerror(errno));
		SetLastError(ERROR_WRITE_FAULT);
		goto out;
	}
	ret = TRUE;
out:
	if (in >= 0)
		close(in);
	if (out >= 0)
		close(out);
	if (!ret && dst != NULL)
		unlink(dst);
	free(buf);
	free(dst);
	return ret;
}

static BOOL save_vhdx(IMG_SAVE* img_save)
{
	char* dst = linux_path(img_save->ImagePath);
	char cmd[MAX_PATH * 2 + 128];
	DWORD r;
	BOOL ret;
	if (dst == NULL)
		return FALSE;
	unlink(dst);
	/* qemu-img convert -p reports its progress as "(xx.yy/100%)" */
	static_sprintf(cmd, "qemu-img convert -p -O vhdx -o subformat=dynamic '%s' '%s'", img_save->DevicePath, dst);
	r = RunCommandWithProgress(cmd, NULL, TRUE, MSG_261, ".*\\(([0-9\\.]+)/100%\\).*");
	ret = (r == 0);
	if (r == 127)
		uprintf("Saving to VHDX requires qemu-img (package 'qemu-utils')");
	if (!ret) {
		SetLastError((r == ERROR_CANCELLED) ? ERROR_CANCELLED : ERROR_WRITE_FAULT);
		unlink(dst);
	}
	free(dst);
	return ret;
}

DWORD WINAPI VhdSaveImageThread(void* param)
{
	IMG_SAVE* img_save = (IMG_SAVE*)param;
	DWORD r = ERROR_NOT_FOUND;

	if_assert_fails(img_save->Type == VIRTUAL_STORAGE_TYPE_DEVICE_VHD ||
		img_save->Type == VIRTUAL_STORAGE_TYPE_DEVICE_VHDX)
		return ERROR_INVALID_PARAMETER;

	UpdateProgressWithInfoInit(NULL, FALSE);
	if ((img_save->Type == VIRTUAL_STORAGE_TYPE_DEVICE_VHD) ? save_fixed_vhd(img_save) : save_vhdx(img_save)) {
		r = 0;
		UpdateProgressWithInfo(OP_FORMAT, MSG_261, SelectedDrive.DiskSize, SelectedDrive.DiskSize);
		uprintf("Saved '%s'", img_save->ImagePath);
	} else {
		r = GetLastError();
		if (IS_ERROR(ErrorStatus) && (SCODE_CODE(ErrorStatus) == ERROR_CANCELLED))
			r = ERROR_CANCELLED;
		else
			uprintf("Could not save virtual disk: %s", WindowsErrorString());
	}
	safe_free(img_save->DevicePath);
	safe_free(img_save->ImagePath);
	PostMessage(hMainDialog, UM_FORMAT_COMPLETED, (WPARAM)TRUE, 0);
	if (r != 0 && !IS_ERROR(ErrorStatus))
		ErrorStatus = RUFUS_ERROR(SCODE_CODE(r));
	ExitThread(r);
}

DWORD WINAPI FfuSaveImageThread(void* param)
{
	IMG_SAVE* img_save = (IMG_SAVE*)param;
	uprintf("FFU images require Windows' DISM and are not supported on Linux");
	safe_free(img_save->DevicePath);
	safe_free(img_save->ImagePath);
	ErrorStatus = RUFUS_ERROR(ERROR_NOT_SUPPORTED);
	PostMessage(hMainDialog, UM_FORMAT_COMPLETED, (WPARAM)TRUE, 0);
	ExitThread(ERROR_NOT_SUPPORTED);
}
