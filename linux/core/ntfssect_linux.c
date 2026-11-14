/*
 * Rufus for Linux: NTFS file sector mapping, for installing Syslinux on NTFS
 * Copyright © 2026 Rufus Linux port contributors
 *
 * Linux implementation of syslinux/win/ntfssect.c: on Windows the cluster
 * runs of a file come from FSCTL_GET_RETRIEVAL_POINTERS, on Linux from the
 * FIEMAP ioctl (supported by the ntfs3 kernel driver).
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */
#include <windows.h>
#include <sys/ioctl.h>
#include <linux/fs.h>
#include <linux/fiemap.h>

#include "rufus.h"
#include "ntfssect.h"

CHAR* NtfsSectLastErrorMessage = NULL;

/* Cluster size of the volume a file lives on, as recorded by NtfsSectGetVolumeInfo() */
static uint32_t cluster_size = 0;

DWORD M_NTFSSECT_API NtfsSectGetVolumeInfo(CHAR* VolumeName, S_NTFSSECT_VOLINFO* VolumeInfo)
{
	uint8_t boot[512];
	DWORD bytes_read = 0;
	LARGE_INTEGER zero = { .QuadPart = 0 };
	(void)VolumeName;

	if (VolumeInfo == NULL || VolumeInfo->Handle == NULL || VolumeInfo->Handle == INVALID_HANDLE_VALUE) {
		NtfsSectLastErrorMessage = "Invalid volume handle";
		return ERROR_INVALID_PARAMETER;
	}
	/* Read the cluster geometry from the NTFS boot sector */
	if (!SetFilePointerEx(VolumeInfo->Handle, zero, NULL, FILE_BEGIN) ||
		!ReadFile(VolumeInfo->Handle, boot, sizeof(boot), &bytes_read, NULL) || bytes_read != sizeof(boot)) {
		NtfsSectLastErrorMessage = "Could not read the NTFS boot sector";
		return ERROR_READ_FAULT;
	}
	if (memcmp(&boot[3], "NTFS    ", 8) != 0) {
		NtfsSectLastErrorMessage = "Not an NTFS volume";
		return ERROR_UNRECOGNIZED_VOLUME;
	}
	VolumeInfo->BytesPerSector = (DWORD)(boot[0x0B] | (boot[0x0C] << 8));
	VolumeInfo->SectorsPerCluster = (boot[0x0D] <= 0x80) ? boot[0x0D] : (1U << (256 - boot[0x0D])) / VolumeInfo->BytesPerSector;
	/* FIEMAP physical offsets are relative to the partition */
	VolumeInfo->PartitionLba.QuadPart = 0;
	cluster_size = VolumeInfo->BytesPerSector * VolumeInfo->SectorsPerCluster;
	if (cluster_size == 0) {
		NtfsSectLastErrorMessage = "Invalid NTFS geometry";
		return ERROR_INVALID_DATA;
	}
	return ERROR_SUCCESS;
}

DWORD M_NTFSSECT_API NtfsSectGetVolumeInfoFromFileName(CHAR* FileName, S_NTFSSECT_VOLINFO* VolumeInfo)
{
	return NtfsSectGetVolumeInfo(FileName, VolumeInfo);
}

DWORD M_NTFSSECT_API NtfsSectGetFileVcnExtent(HANDLE File, LARGE_INTEGER* Vcn, S_NTFSSECT_EXTENT* Extent)
{
	struct {
		struct fiemap fm;
		struct fiemap_extent fe[1];
	} req;
	int fd = HandleToFd(File);
	uint64_t offset;

	if (fd < 0 || Vcn == NULL || Extent == NULL || cluster_size == 0)
		return ERROR_INVALID_PARAMETER;
	offset = (uint64_t)Vcn->QuadPart * cluster_size;
	memset(&req, 0, sizeof(req));
	req.fm.fm_start = offset;
	req.fm.fm_length = FIEMAP_MAX_OFFSET - offset;
	req.fm.fm_flags = FIEMAP_FLAG_SYNC;
	req.fm.fm_extent_count = 1;
	if (ioctl(fd, FS_IOC_FIEMAP, &req.fm) != 0) {
		NtfsSectLastErrorMessage = "FIEMAP is not supported (the ntfs3 driver is required)";
		return ERROR_NOT_SUPPORTED;
	}
	if (req.fm.fm_mapped_extents == 0)
		return ERROR_HANDLE_EOF;
	if (req.fe[0].fe_flags & (FIEMAP_EXTENT_UNKNOWN | FIEMAP_EXTENT_DATA_INLINE | FIEMAP_EXTENT_ENCODED)) {
		NtfsSectLastErrorMessage = "File data is not stored in regular clusters";
		return ERROR_INVALID_DATA;
	}
	/* The extent may start before the requested VCN */
	Extent->FirstVcn.QuadPart = (LONGLONG)(req.fe[0].fe_logical / cluster_size);
	Extent->NextVcn.QuadPart = (LONGLONG)((req.fe[0].fe_logical + req.fe[0].fe_length + cluster_size - 1) / cluster_size);
	Extent->FirstLcn.QuadPart = (LONGLONG)(req.fe[0].fe_physical / cluster_size);
	if (Extent->FirstVcn.QuadPart < Vcn->QuadPart) {
		Extent->FirstLcn.QuadPart += Vcn->QuadPart - Extent->FirstVcn.QuadPart;
		Extent->FirstVcn.QuadPart = Vcn->QuadPart;
	}
	return ERROR_SUCCESS;
}

DWORD M_NTFSSECT_API NtfsSectLcnToLba(const S_NTFSSECT_VOLINFO* VolumeInfo, const LARGE_INTEGER* Lcn, LARGE_INTEGER* Lba)
{
	if (VolumeInfo == NULL || Lcn == NULL || Lba == NULL)
		return ERROR_INVALID_PARAMETER;
	Lba->QuadPart = VolumeInfo->PartitionLba.QuadPart + Lcn->QuadPart * VolumeInfo->SectorsPerCluster;
	return ERROR_SUCCESS;
}

DWORD M_NTFSSECT_API NtfsSectLoadXpFuncs(S_NTFSSECT_XPFUNCS* XpFuncs)
{
	(void)XpFuncs;
	return ERROR_SUCCESS;
}

VOID M_NTFSSECT_API NtfsSectUnloadXpFuncs(S_NTFSSECT_XPFUNCS* XpFuncs)
{
	(void)XpFuncs;
}
