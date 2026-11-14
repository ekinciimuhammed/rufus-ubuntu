/*
 * Rufus for Linux: emulation of the Windows disk IOCTLs used by Rufus
 * Copyright © 2026 Rufus Linux port contributors
 *
 * Rufus' partitioning code (drive.c, format.c, format_fat32.c) talks to disks
 * through a handful of Windows IOCTLs. Rather than rewriting that logic, the
 * IOCTLs are implemented here on top of Linux block devices: partition tables
 * are parsed from, and written to, the device itself (MBR, EBR chains and GPT,
 * including the backup GPT and CRCs) and the kernel is then asked to re-read
 * them, exactly like the Windows disk class driver would.
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
#include <sys/mount.h>
#include <linux/fs.h>
#include <linux/blkpg.h>
#include <linux/hdreg.h>

#include "rufus.h"
#include "linux_drive.h"

/* Our virtual disk geometry: same as what Windows reports for USB media */
#define GEOMETRY_HEADS              255
#define GEOMETRY_SECTORS_PER_TRACK  63
#define GPT_ENTRY_SIZE              128
#define GPT_ENTRIES                 128
#define GPT_HEADER_SIZE             92
#define MAX_LAYOUT_ENTRIES          128

#pragma pack(push, 1)
typedef struct {
	uint8_t status;
	uint8_t chs_start[3];
	uint8_t type;
	uint8_t chs_end[3];
	uint32_t lba_start;
	uint32_t num_sectors;
} mbr_entry_t;

typedef struct {
	char signature[8];
	uint32_t revision;
	uint32_t header_size;
	uint32_t header_crc32;
	uint32_t reserved;
	uint64_t my_lba;
	uint64_t alternate_lba;
	uint64_t first_usable_lba;
	uint64_t last_usable_lba;
	GUID disk_guid;
	uint64_t partition_entry_lba;
	uint32_t num_partition_entries;
	uint32_t partition_entry_size;
	uint32_t partition_entry_array_crc32;
} gpt_header_t;

typedef struct {
	GUID type_guid;
	GUID unique_guid;
	uint64_t first_lba;
	uint64_t last_lba;
	uint64_t attributes;
	uint16_t name[36];
} gpt_entry_t;
#pragma pack(pop)

_Static_assert(sizeof(mbr_entry_t) == 16, "bad MBR entry size");
_Static_assert(sizeof(gpt_entry_t) == GPT_ENTRY_SIZE, "bad GPT entry size");

/* A parsed partition table, independent of Windows' structures */
typedef struct {
	int style;					/* PARTITION_STYLE_xxx */
	uint32_t mbr_signature;
	GUID disk_guid;
	uint64_t first_usable_lba, last_usable_lba;
	uint32_t max_entries;
	int count;
	struct {
		uint64_t start_lba;
		uint64_t num_sectors;
		uint32_t number;		/* 1-based, 0 for unused MBR slots */
		uint8_t mbr_type;
		BOOL boot;
		GUID type_guid;
		GUID part_guid;
		uint64_t attributes;
		uint16_t name[36];
	} part[MAX_LAYOUT_ENTRIES];
} layout_t;

/* ------------------------------------------------------------------------- */
/* Block device helpers                                                      */
/* ------------------------------------------------------------------------- */
typedef struct {
	int fd;
	BOOL is_partition;
	uint32_t sector_size;
	uint64_t size;				/* size of this block device, in bytes */
	uint64_t part_start;		/* partition start, in bytes (0 for a disk) */
	uint32_t part_number;
	char disk_kname[64];		/* kernel name of the whole disk */
	char kname[64];
	dev_t disk_dev;
} blkinfo_t;

static BOOL read_sysfs_u64(const char* path, uint64_t* val)
{
	char buf[64];
	FILE* f = fopenU(path, "r");
	BOOL r = FALSE;
	if (f == NULL)
		return FALSE;
	if (fgets(buf, sizeof(buf), f) != NULL) {
		*val = strtoull(buf, NULL, 10);
		r = TRUE;
	}
	fclose(f);
	return r;
}

static BOOL get_blkinfo(HANDLE h, blkinfo_t* bi)
{
	struct stat st;
	char path[256], link[256], *p;
	ssize_t r;
	uint64_t v;
	int ss = 512;

	memset(bi, 0, sizeof(*bi));
	bi->fd = HandleToFd(h);
	if (bi->fd < 0 || fstat(bi->fd, &st) != 0) {
		SetLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	if (!S_ISBLK(st.st_mode)) {
		/* Regular file (e.g. a disk image): behave like a whole disk */
		bi->sector_size = 512;
		bi->size = (uint64_t)st.st_size;
		return TRUE;
	}
	if (ioctl(bi->fd, BLKSSZGET, &ss) == 0 && ss >= 512)
		bi->sector_size = (uint32_t)ss;
	else
		bi->sector_size = 512;
	if (ioctl(bi->fd, BLKGETSIZE64, &bi->size) != 0)
		bi->size = 0;
	snprintf(path, sizeof(path), "/sys/dev/block/%u:%u", major(st.st_rdev), minor(st.st_rdev));
	r = readlink(path, link, sizeof(link) - 1);
	if (r <= 0)
		return TRUE;
	link[r] = 0;
	p = strrchr(link, '/');
	snprintf(bi->kname, sizeof(bi->kname), "%s", (p == NULL) ? link : p + 1);
	snprintf(path, sizeof(path), "/sys/dev/block/%u:%u/partition", major(st.st_rdev), minor(st.st_rdev));
	if (read_sysfs_u64(path, &v)) {
		bi->is_partition = TRUE;
		bi->part_number = (uint32_t)v;
		snprintf(path, sizeof(path), "/sys/dev/block/%u:%u/start", major(st.st_rdev), minor(st.st_rdev));
		if (read_sysfs_u64(path, &v))
			bi->part_start = v * 512;
		/* The parent directory is the whole disk */
		if (p != NULL) {
			*p = 0;
			p = strrchr(link, '/');
			snprintf(bi->disk_kname, sizeof(bi->disk_kname), "%s", (p == NULL) ? link : p + 1);
		}
	} else {
		snprintf(bi->disk_kname, sizeof(bi->disk_kname), "%s", bi->kname);
		bi->disk_dev = st.st_rdev;
	}
	return TRUE;
}

static BOOL pread_full(int fd, void* buf, size_t len, uint64_t off)
{
	size_t done = 0;
	ssize_t r;
	while (done < len) {
		r = pread(fd, (uint8_t*)buf + done, len - done, (off_t)(off + done));
		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0) {
			SetLastError((r == 0) ? ERROR_HANDLE_EOF : ErrnoToWin32(errno));
			return FALSE;
		}
		done += r;
	}
	return TRUE;
}

static BOOL pwrite_full(int fd, const void* buf, size_t len, uint64_t off)
{
	size_t done = 0;
	ssize_t r;
	while (done < len) {
		r = pwrite(fd, (const uint8_t*)buf + done, len - done, (off_t)(off + done));
		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0) {
			SetLastError((r == 0) ? ERROR_WRITE_FAULT : ErrnoToWin32(errno));
			return FALSE;
		}
		done += r;
	}
	return TRUE;
}

/* Standard (IEEE 802.3) CRC-32, as used by GPT */
static uint32_t crc32_ieee(const void* data, size_t len)
{
	static uint32_t table[256];
	static int init = 0;
	const uint8_t* p = (const uint8_t*)data;
	uint32_t crc = 0xFFFFFFFF;
	size_t i;
	if (!init) {
		uint32_t c, n, k;
		for (n = 0; n < 256; n++) {
			c = n;
			for (k = 0; k < 8; k++)
				c = (c & 1) ? (0xEDB88320 ^ (c >> 1)) : (c >> 1);
			table[n] = c;
		}
		init = 1;
	}
	for (i = 0; i < len; i++)
		crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
	return crc ^ 0xFFFFFFFF;
}

/* ------------------------------------------------------------------------- */
/* Partition table parsing                                                   */
/* ------------------------------------------------------------------------- */
static BOOL looks_like_vbr(const uint8_t* s)
{
	/* A volume boot record (no partition table) starts with a jump and has a BPB */
	if (!((s[0] == 0xEB && s[2] == 0x90) || s[0] == 0xE9))
		return FALSE;
	if (memcmp(&s[3], "NTFS    ", 8) == 0 || memcmp(&s[3], "EXFAT   ", 8) == 0)
		return TRUE;
	/* FAT: valid bytes per sector and sectors per cluster */
	{
		uint16_t bps = (uint16_t)(s[11] | (s[12] << 8));
		uint8_t spc = s[13];
		return (bps >= 512 && bps <= 4096 && IS_POWER_OF_2(bps) && spc != 0 && IS_POWER_OF_2(spc));
	}
}

static uint8_t vbr_to_mbr_type(const uint8_t* s)
{
	if (memcmp(&s[3], "NTFS    ", 8) == 0 || memcmp(&s[3], "EXFAT   ", 8) == 0)
		return 0x07;
	if (memcmp(&s[0x52], "FAT32", 5) == 0)
		return 0x0c;
	return 0x06;
}

static BOOL parse_gpt(int fd, uint32_t ss, uint64_t disk_size, layout_t* lo)
{
	uint8_t* sector = calloc(1, ss);
	gpt_header_t* hdr = (gpt_header_t*)sector;
	uint8_t* entries = NULL;
	size_t entries_len;
	uint32_t i;
	BOOL r = FALSE;

	if (sector == NULL || !pread_full(fd, sector, ss, ss))
		goto out;
	if (memcmp(hdr->signature, "EFI PART", 8) != 0 || hdr->partition_entry_size < sizeof(gpt_entry_t) ||
		hdr->num_partition_entries == 0 || hdr->num_partition_entries > 4096) {
		/* Try the backup header */
		if (disk_size < 2 * ss || !pread_full(fd, sector, ss, disk_size - ss) ||
			memcmp(hdr->signature, "EFI PART", 8) != 0 || hdr->partition_entry_size < sizeof(gpt_entry_t) ||
			hdr->num_partition_entries == 0 || hdr->num_partition_entries > 4096)
			goto out;
	}
	lo->style = PARTITION_STYLE_GPT;
	lo->disk_guid = hdr->disk_guid;
	lo->first_usable_lba = hdr->first_usable_lba;
	lo->last_usable_lba = hdr->last_usable_lba;
	lo->max_entries = hdr->num_partition_entries;
	entries_len = (size_t)hdr->num_partition_entries * hdr->partition_entry_size;
	entries = malloc(entries_len);
	if (entries == NULL || !pread_full(fd, entries, entries_len, hdr->partition_entry_lba * ss))
		goto out;
	for (i = 0; i < hdr->num_partition_entries && lo->count < MAX_LAYOUT_ENTRIES; i++) {
		gpt_entry_t* e = (gpt_entry_t*)&entries[i * hdr->partition_entry_size];
		if (CompareGUID(&e->type_guid, &GUID_NULL))
			continue;
		lo->part[lo->count].start_lba = e->first_lba;
		lo->part[lo->count].num_sectors = (e->last_lba >= e->first_lba) ? e->last_lba - e->first_lba + 1 : 0;
		lo->part[lo->count].number = i + 1;
		lo->part[lo->count].type_guid = e->type_guid;
		lo->part[lo->count].part_guid = e->unique_guid;
		lo->part[lo->count].attributes = e->attributes;
		memcpy(lo->part[lo->count].name, e->name, sizeof(e->name));
		lo->count++;
	}
	r = TRUE;
out:
	free(entries);
	free(sector);
	return r;
}

static BOOL read_layout(int fd, uint32_t ss, uint64_t disk_size, layout_t* lo)
{
	uint8_t* mbr = calloc(1, max(ss, 512u));
	mbr_entry_t* e;
	int i, n;
	BOOL has_protective = FALSE, valid = TRUE;

	memset(lo, 0, sizeof(*lo));
	if (mbr == NULL || !pread_full(fd, mbr, ss, 0)) {
		free(mbr);
		return FALSE;
	}
	if (mbr[0x1FE] != 0x55 || mbr[0x1FF] != 0xAA) {
		/* No boot marker: uninitialized disk (unless there's a GPT without protective MBR) */
		lo->style = PARTITION_STYLE_RAW;
		parse_gpt(fd, ss, disk_size, lo);
		free(mbr);
		return TRUE;
	}
	e = (mbr_entry_t*)&mbr[0x1BE];
	for (i = 0; i < 4; i++) {
		if (e[i].type == 0xEE)
			has_protective = TRUE;
		if (e[i].status != 0x00 && e[i].status != 0x80)
			valid = FALSE;
	}
	if (has_protective && parse_gpt(fd, ss, disk_size, lo)) {
		free(mbr);
		return TRUE;
	}
	memset(lo, 0, sizeof(*lo));
	lo->style = PARTITION_STYLE_MBR;
	lo->mbr_signature = *(uint32_t*)&mbr[0x1B8];

	if (!valid && looks_like_vbr(mbr)) {
		/* Super floppy: one "partition" spanning the whole disk, starting at offset 0 */
		lo->count = 4;
		lo->part[0].start_lba = 0;
		lo->part[0].num_sectors = disk_size / ss;
		lo->part[0].number = 1;
		lo->part[0].mbr_type = vbr_to_mbr_type(mbr);
		free(mbr);
		return TRUE;
	}

	/* Primary partitions (4 slots, like Windows reports them) */
	lo->count = 4;
	for (i = 0, n = 0; i < 4; i++) {
		if (e[i].type == 0 || e[i].num_sectors == 0)
			continue;
		lo->part[i].start_lba = e[i].lba_start;
		lo->part[i].num_sectors = e[i].num_sectors;
		lo->part[i].number = ++n;
		lo->part[i].mbr_type = e[i].type;
		lo->part[i].boot = (e[i].status == 0x80);
	}
	/* Logical partitions, in groups of 4 slots per EBR */
	for (i = 0; i < 4; i++) {
		uint64_t ext_start, ebr_lba;
		int guard = 0;
		if (e[i].type != 0x05 && e[i].type != 0x0F && e[i].type != 0x85)
			continue;
		ext_start = e[i].lba_start;
		ebr_lba = ext_start;
		while (ebr_lba != 0 && guard++ < 64 && lo->count + 4 <= MAX_LAYOUT_ENTRIES) {
			uint8_t* ebr = calloc(1, ss);
			mbr_entry_t* le;
			if (ebr == NULL || !pread_full(fd, ebr, ss, ebr_lba * ss) || ebr[0x1FE] != 0x55 || ebr[0x1FF] != 0xAA) {
				free(ebr);
				break;
			}
			le = (mbr_entry_t*)&ebr[0x1BE];
			if (le[0].type != 0 && le[0].num_sectors != 0) {
				lo->part[lo->count].start_lba = ebr_lba + le[0].lba_start;
				lo->part[lo->count].num_sectors = le[0].num_sectors;
				lo->part[lo->count].number = ++n;
				lo->part[lo->count].mbr_type = le[0].type;
				lo->part[lo->count].boot = (le[0].status == 0x80);
			}
			lo->count += 4;
			ebr_lba = (le[1].type != 0) ? ext_start + le[1].lba_start : 0;
			free(ebr);
		}
		break;
	}
	free(mbr);
	return TRUE;
}

/* ------------------------------------------------------------------------- */
/* Partition table writing                                                   */
/* ------------------------------------------------------------------------- */
static void lba_to_chs(uint64_t lba, uint8_t chs[3])
{
	uint64_t c, h, s;
	if (lba >= (uint64_t)1024 * GEOMETRY_HEADS * GEOMETRY_SECTORS_PER_TRACK) {
		chs[0] = 0xFE; chs[1] = 0xFF; chs[2] = 0xFF;
		return;
	}
	c = lba / (GEOMETRY_HEADS * GEOMETRY_SECTORS_PER_TRACK);
	h = (lba / GEOMETRY_SECTORS_PER_TRACK) % GEOMETRY_HEADS;
	s = (lba % GEOMETRY_SECTORS_PER_TRACK) + 1;
	chs[0] = (uint8_t)h;
	chs[1] = (uint8_t)(s | ((c >> 2) & 0xC0));
	chs[2] = (uint8_t)(c & 0xFF);
}

static BOOL write_mbr_table(int fd, uint32_t ss, uint32_t signature, const layout_t* lo)
{
	uint8_t* mbr = calloc(1, ss);
	mbr_entry_t* e;
	int i;
	BOOL r = FALSE;

	if (mbr == NULL)
		return FALSE;
	/* Preserve the boot code, like Windows does */
	if (!pread_full(fd, mbr, ss, 0))
		memset(mbr, 0, ss);
	memset(&mbr[0x1B8], 0, 0x1FE - 0x1B8);
	*(uint32_t*)&mbr[0x1B8] = signature;
	e = (mbr_entry_t*)&mbr[0x1BE];
	for (i = 0; lo != NULL && i < 4 && i < lo->count; i++) {
		if (lo->part[i].mbr_type == 0 || lo->part[i].num_sectors == 0)
			continue;
		e[i].status = lo->part[i].boot ? 0x80 : 0x00;
		e[i].type = lo->part[i].mbr_type;
		e[i].lba_start = (uint32_t)lo->part[i].start_lba;
		e[i].num_sectors = (uint32_t)lo->part[i].num_sectors;
		lba_to_chs(lo->part[i].start_lba, e[i].chs_start);
		lba_to_chs(lo->part[i].start_lba + lo->part[i].num_sectors - 1, e[i].chs_end);
	}
	mbr[0x1FE] = 0x55;
	mbr[0x1FF] = 0xAA;
	r = pwrite_full(fd, mbr, ss, 0);
	free(mbr);
	return r;
}

static BOOL write_gpt_table(int fd, uint32_t ss, uint64_t disk_size, const layout_t* lo)
{
	const uint64_t total_lba = disk_size / ss;
	const size_t entries_len = (size_t)GPT_ENTRIES * GPT_ENTRY_SIZE;
	const uint64_t entries_lbas = (entries_len + ss - 1) / ss;
	uint8_t *entries = NULL, *sector = NULL;
	gpt_header_t* hdr;
	uint64_t first_usable, last_usable;
	int i;
	BOOL r = FALSE;

	if (total_lba < 2 * entries_lbas + 3) {
		SetLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	entries = calloc(1, entries_lbas * ss);
	sector = calloc(1, ss);
	if (entries == NULL || sector == NULL)
		goto out;
	first_usable = 2 + entries_lbas;
	last_usable = total_lba - 2 - entries_lbas;
	/* Honour the usable area requested by the caller when it is valid */
	if (lo->first_usable_lba >= first_usable && lo->first_usable_lba < last_usable)
		first_usable = lo->first_usable_lba;
	if (lo->last_usable_lba > first_usable && lo->last_usable_lba <= last_usable)
		last_usable = lo->last_usable_lba;

	for (i = 0; i < lo->count && i < GPT_ENTRIES; i++) {
		gpt_entry_t* e = (gpt_entry_t*)&entries[i * GPT_ENTRY_SIZE];
		if (lo->part[i].num_sectors == 0)
			continue;
		if (lo->part[i].start_lba < first_usable || lo->part[i].start_lba + lo->part[i].num_sectors - 1 > last_usable) {
			uprintf("GPT: partition %d (LBA %" PRIu64 "+%" PRIu64 ") is outside of the usable area",
				i + 1, lo->part[i].start_lba, lo->part[i].num_sectors);
			SetLastError(ERROR_INVALID_PARAMETER);
			goto out;
		}
		e->type_guid = lo->part[i].type_guid;
		e->unique_guid = lo->part[i].part_guid;
		if (CompareGUID(&e->unique_guid, &GUID_NULL))
			CoCreateGuid(&e->unique_guid);
		e->first_lba = lo->part[i].start_lba;
		e->last_lba = lo->part[i].start_lba + lo->part[i].num_sectors - 1;
		e->attributes = lo->part[i].attributes;
		memcpy(e->name, lo->part[i].name, sizeof(e->name));
	}

	/* Protective MBR (the boot code area is preserved, the rest is reset) */
	if (!pread_full(fd, sector, ss, 0))
		memset(sector, 0, ss);
	memset(&sector[0x1B8], 0, 0x1FE - 0x1B8);
	{
		mbr_entry_t* pe = (mbr_entry_t*)&sector[0x1BE];
		pe->status = 0x00;
		pe->chs_start[0] = 0x00; pe->chs_start[1] = 0x02; pe->chs_start[2] = 0x00;
		pe->type = 0xEE;
		pe->chs_end[0] = 0xFE; pe->chs_end[1] = 0xFF; pe->chs_end[2] = 0xFF;
		pe->lba_start = 1;
		pe->num_sectors = (total_lba - 1 > 0xFFFFFFFFULL) ? 0xFFFFFFFF : (uint32_t)(total_lba - 1);
	}
	sector[0x1FE] = 0x55;
	sector[0x1FF] = 0xAA;
	if (!pwrite_full(fd, sector, ss, 0))
		goto out;

	/* Partition entries: primary copy after the header, backup before the backup header */
	if (!pwrite_full(fd, entries, entries_lbas * ss, 2 * ss) ||
		!pwrite_full(fd, entries, entries_lbas * ss, (total_lba - 1 - entries_lbas) * ss))
		goto out;

	/* Primary header */
	memset(sector, 0, ss);
	hdr = (gpt_header_t*)sector;
	memcpy(hdr->signature, "EFI PART", 8);
	hdr->revision = 0x00010000;
	hdr->header_size = GPT_HEADER_SIZE;
	hdr->my_lba = 1;
	hdr->alternate_lba = total_lba - 1;
	hdr->first_usable_lba = first_usable;
	hdr->last_usable_lba = last_usable;
	hdr->disk_guid = lo->disk_guid;
	if (CompareGUID(&hdr->disk_guid, &GUID_NULL))
		CoCreateGuid(&hdr->disk_guid);
	hdr->partition_entry_lba = 2;
	hdr->num_partition_entries = GPT_ENTRIES;
	hdr->partition_entry_size = GPT_ENTRY_SIZE;
	hdr->partition_entry_array_crc32 = crc32_ieee(entries, entries_len);
	hdr->header_crc32 = 0;
	hdr->header_crc32 = crc32_ieee(hdr, GPT_HEADER_SIZE);
	if (!pwrite_full(fd, sector, ss, ss))
		goto out;

	/* Backup header */
	hdr->my_lba = total_lba - 1;
	hdr->alternate_lba = 1;
	hdr->partition_entry_lba = total_lba - 1 - entries_lbas;
	hdr->header_crc32 = 0;
	hdr->header_crc32 = crc32_ieee(hdr, GPT_HEADER_SIZE);
	if (!pwrite_full(fd, sector, ss, (total_lba - 1) * ss))
		goto out;
	r = TRUE;
out:
	free(entries);
	free(sector);
	return r;
}

/* Wipe the partition tables (primary and backup GPT, MBR partition entries) */
static BOOL wipe_tables(int fd, uint32_t ss, uint64_t disk_size)
{
	uint8_t* buf = calloc(34, ss);
	uint8_t* mbr = calloc(1, ss);
	BOOL r = FALSE;
	if (buf == NULL || mbr == NULL)
		goto out;
	if (!pread_full(fd, mbr, ss, 0))
		memset(mbr, 0, ss);
	/* Keep the boot code, clear the signature, entries and boot marker */
	memset(&mbr[0x1B8], 0, ss - 0x1B8);
	if (!pwrite_full(fd, mbr, ss, 0))
		goto out;
	/* Primary GPT header and entries */
	if (!pwrite_full(fd, buf, 33 * (size_t)ss, ss))
		goto out;
	/* Backup GPT */
	if (disk_size > 34ULL * ss && !pwrite_full(fd, buf, 33 * (size_t)ss, disk_size - 33ULL * ss))
		goto out;
	r = TRUE;
out:
	free(buf);
	free(mbr);
	return r;
}

/* Make the kernel's view of the partitions match the on-disk table */
static BOOL update_kernel_partitions(int fd, uint32_t ss, uint64_t disk_size, const char* disk_kname)
{
	layout_t lo;
	struct blkpg_partition bp;
	struct blkpg_ioctl_arg arg;
	int i, n, retries;

	fsync(fd);
	ioctl(fd, BLKFLSBUF, 0);
	for (retries = 0; retries < 10; retries++) {
		if (ioctl(fd, BLKRRPART, NULL) == 0) {
			linux_run((const char* []) { "udevadm", "settle", "--timeout=10", NULL }, FALSE);
			return TRUE;
		}
		if (errno != EBUSY)
			break;
		Sleep(100);
	}
	/* BLKRRPART fails when a partition is in use: synchronize through BLKPG instead (like partx -u) */
	uprintf("Notice: Partition table re-read failed (%s) - updating kernel partitions individually", strerror(errno));
	for (n = 1; n <= 128; n++) {
		memset(&bp, 0, sizeof(bp));
		bp.pno = n;
		arg.op = BLKPG_DEL_PARTITION;
		arg.flags = 0;
		arg.datalen = sizeof(bp);
		arg.data = &bp;
		ioctl(fd, BLKPG, &arg);
	}
	if (!read_layout(fd, ss, disk_size, &lo))
		return FALSE;
	for (i = 0; i < lo.count; i++) {
		if (lo.part[i].num_sectors == 0 || lo.part[i].number == 0)
			continue;
		memset(&bp, 0, sizeof(bp));
		bp.start = (long long)(lo.part[i].start_lba * ss);
		bp.length = (long long)(lo.part[i].num_sectors * ss);
		bp.pno = (int)lo.part[i].number;
		arg.op = BLKPG_ADD_PARTITION;
		arg.flags = 0;
		arg.datalen = sizeof(bp);
		arg.data = &bp;
		if (ioctl(fd, BLKPG, &arg) != 0)
			uprintf("Could not add kernel partition %s%d: %s", disk_kname, bp.pno, strerror(errno));
	}
	linux_run((const char* []) { "udevadm", "settle", "--timeout=10", NULL }, FALSE);
	return TRUE;
}

/* ------------------------------------------------------------------------- */
/* DeviceIoControl                                                           */
/* ------------------------------------------------------------------------- */
static void fill_geometry(const blkinfo_t* bi, DISK_GEOMETRY* g, const char* disk_kname)
{
	char path[128];
	uint64_t removable = 0;
	g->BytesPerSector = bi->sector_size;
	g->SectorsPerTrack = GEOMETRY_SECTORS_PER_TRACK;
	g->TracksPerCylinder = GEOMETRY_HEADS;
	snprintf(path, sizeof(path), "/sys/block/%s/removable", disk_kname);
	read_sysfs_u64(path, &removable);
	g->MediaType = removable ? RemovableMedia : FixedMedia;
}

BOOL DeviceIoControl(HANDLE hDevice, DWORD dwIoControlCode, LPVOID lpInBuffer, DWORD nInBufferSize,
	LPVOID lpOutBuffer, DWORD nOutBufferSize, LPDWORD lpBytesReturned, LPOVERLAPPED lpOverlapped)
{
	blkinfo_t bi;
	layout_t lo;
	DWORD returned = 0;
	BOOL r = FALSE;
	uint64_t disk_size;
	int i;

	(void)lpOverlapped;
	if (lpBytesReturned != NULL)
		*lpBytesReturned = 0;

	/* Volume (un)locking: exclusivity is handled through the drive layer on Linux */
	switch (dwIoControlCode) {
	case FSCTL_LOCK_VOLUME:
	case FSCTL_UNLOCK_VOLUME:
	case FSCTL_ALLOW_EXTENDED_DASD_IO:
		return TRUE;
	default:
		break;
	}

	if (!get_blkinfo(hDevice, &bi))
		return FALSE;
	disk_size = bi.size;

	switch (dwIoControlCode) {
	case IOCTL_DISK_GET_DRIVE_GEOMETRY:
	case IOCTL_DISK_GET_DRIVE_GEOMETRY_EX: {
		blkinfo_t di = bi;
		/* A volume reports the geometry of its disk */
		if (bi.is_partition) {
			char path[128];
			uint64_t sectors = 0;
			snprintf(path, sizeof(path), "/sys/block/%s/size", bi.disk_kname);
			if (read_sysfs_u64(path, &sectors))
				di.size = sectors * 512;
		}
		if (dwIoControlCode == IOCTL_DISK_GET_DRIVE_GEOMETRY) {
			PDISK_GEOMETRY g = (PDISK_GEOMETRY)lpOutBuffer;
			if (g == NULL || nOutBufferSize < sizeof(DISK_GEOMETRY)) {
				SetLastError(ERROR_INSUFFICIENT_BUFFER);
				return FALSE;
			}
			memset(g, 0, sizeof(*g));
			fill_geometry(&di, g, bi.disk_kname);
			g->Cylinders.QuadPart = di.size / ((uint64_t)GEOMETRY_HEADS * GEOMETRY_SECTORS_PER_TRACK * di.sector_size);
			returned = sizeof(DISK_GEOMETRY);
		} else {
			PDISK_GEOMETRY_EX g = (PDISK_GEOMETRY_EX)lpOutBuffer;
			if (g == NULL || nOutBufferSize < offsetof(DISK_GEOMETRY_EX, Data)) {
				SetLastError(ERROR_INSUFFICIENT_BUFFER);
				return FALSE;
			}
			memset(g, 0, offsetof(DISK_GEOMETRY_EX, Data));
			fill_geometry(&di, &g->Geometry, bi.disk_kname);
			g->Geometry.Cylinders.QuadPart = di.size / ((uint64_t)GEOMETRY_HEADS * GEOMETRY_SECTORS_PER_TRACK * di.sector_size);
			g->DiskSize.QuadPart = di.size;
			returned = offsetof(DISK_GEOMETRY_EX, Data);
		}
		/* No media (e.g. empty card reader) */
		if (di.size == 0) {
			SetLastError(ERROR_NOT_READY);
			return FALSE;
		}
		r = TRUE;
		break;
	}

	case IOCTL_DISK_GET_LENGTH_INFO: {
		PGET_LENGTH_INFORMATION li = (PGET_LENGTH_INFORMATION)lpOutBuffer;
		if (li == NULL || nOutBufferSize < sizeof(*li)) {
			SetLastError(ERROR_INSUFFICIENT_BUFFER);
			return FALSE;
		}
		li->Length.QuadPart = disk_size;
		returned = sizeof(*li);
		r = TRUE;
		break;
	}

	case IOCTL_STORAGE_CHECK_VERIFY:
		r = (disk_size != 0);
		if (!r)
			SetLastError(ERROR_NOT_READY);
		break;

	case IOCTL_DISK_GET_DRIVE_LAYOUT_EX: {
		PDRIVE_LAYOUT_INFORMATION_EX dl = (PDRIVE_LAYOUT_INFORMATION_EX)lpOutBuffer;
		size_t capacity;
		if (bi.is_partition) {
			SetLastError(ERROR_INVALID_FUNCTION);
			return FALSE;
		}
		if (dl == NULL || nOutBufferSize < offsetof(DRIVE_LAYOUT_INFORMATION_EX, PartitionEntry)) {
			SetLastError(ERROR_INSUFFICIENT_BUFFER);
			return FALSE;
		}
		if (!read_layout(bi.fd, bi.sector_size, disk_size, &lo))
			return FALSE;
		capacity = (nOutBufferSize - offsetof(DRIVE_LAYOUT_INFORMATION_EX, PartitionEntry)) / sizeof(PARTITION_INFORMATION_EX);
		if ((size_t)lo.count > capacity) {
			SetLastError(ERROR_INSUFFICIENT_BUFFER);
			return FALSE;
		}
		memset(dl, 0, offsetof(DRIVE_LAYOUT_INFORMATION_EX, PartitionEntry) + lo.count * sizeof(PARTITION_INFORMATION_EX));
		dl->PartitionStyle = (lo.style == PARTITION_STYLE_RAW) ? PARTITION_STYLE_RAW : lo.style;
		dl->PartitionCount = (lo.style == PARTITION_STYLE_RAW) ? 0 : lo.count;
		if (lo.style == PARTITION_STYLE_MBR) {
			dl->Mbr.Signature = lo.mbr_signature;
		} else if (lo.style == PARTITION_STYLE_GPT) {
			dl->Gpt.DiskId = lo.disk_guid;
			dl->Gpt.StartingUsableOffset.QuadPart = lo.first_usable_lba * bi.sector_size;
			dl->Gpt.UsableLength.QuadPart = (lo.last_usable_lba - lo.first_usable_lba + 1) * bi.sector_size;
			dl->Gpt.MaxPartitionCount = lo.max_entries;
		}
		for (i = 0; i < (int)dl->PartitionCount; i++) {
			PARTITION_INFORMATION_EX* pe = &dl->PartitionEntry[i];
			pe->PartitionStyle = lo.style;
			pe->StartingOffset.QuadPart = lo.part[i].start_lba * bi.sector_size;
			pe->PartitionLength.QuadPart = lo.part[i].num_sectors * bi.sector_size;
			pe->PartitionNumber = lo.part[i].number;
			if (lo.style == PARTITION_STYLE_MBR) {
				pe->Mbr.PartitionType = lo.part[i].mbr_type;
				pe->Mbr.BootIndicator = lo.part[i].boot;
				pe->Mbr.RecognizedPartition = (lo.part[i].mbr_type != 0);
				pe->Mbr.HiddenSectors = (DWORD)lo.part[i].start_lba;
			} else {
				int k;
				pe->Gpt.PartitionType = lo.part[i].type_guid;
				pe->Gpt.PartitionId = lo.part[i].part_guid;
				pe->Gpt.Attributes = lo.part[i].attributes;
				for (k = 0; k < 36; k++)
					pe->Gpt.Name[k] = (WCHAR)lo.part[i].name[k];
			}
		}
		returned = (DWORD)(offsetof(DRIVE_LAYOUT_INFORMATION_EX, PartitionEntry) + dl->PartitionCount * sizeof(PARTITION_INFORMATION_EX));
		r = TRUE;
		break;
	}

	case IOCTL_DISK_CREATE_DISK: {
		PCREATE_DISK cd = (PCREATE_DISK)lpInBuffer;
		if (bi.is_partition || cd == NULL || nInBufferSize < sizeof(CREATE_DISK)) {
			SetLastError(ERROR_INVALID_PARAMETER);
			return FALSE;
		}
		if (!wipe_tables(bi.fd, bi.sector_size, disk_size))
			return FALSE;
		memset(&lo, 0, sizeof(lo));
		switch (cd->PartitionStyle) {
		case PARTITION_STYLE_MBR:
			r = write_mbr_table(bi.fd, bi.sector_size, cd->Mbr.Signature, NULL);
			break;
		case PARTITION_STYLE_GPT:
			lo.style = PARTITION_STYLE_GPT;
			lo.disk_guid = cd->Gpt.DiskId;
			r = write_gpt_table(bi.fd, bi.sector_size, disk_size, &lo);
			break;
		default:	/* RAW: the disk is left without any partition table */
			r = TRUE;
			break;
		}
		fsync(bi.fd);
		break;
	}

	case IOCTL_DISK_SET_DRIVE_LAYOUT_EX: {
		PDRIVE_LAYOUT_INFORMATION_EX dl = (PDRIVE_LAYOUT_INFORMATION_EX)lpInBuffer;
		size_t max_count;
		if (bi.is_partition || dl == NULL || nInBufferSize < offsetof(DRIVE_LAYOUT_INFORMATION_EX, PartitionEntry)) {
			SetLastError(ERROR_INVALID_PARAMETER);
			return FALSE;
		}
		max_count = (nInBufferSize - offsetof(DRIVE_LAYOUT_INFORMATION_EX, PartitionEntry)) / sizeof(PARTITION_INFORMATION_EX);
		if (dl->PartitionCount > max_count || dl->PartitionCount > MAX_LAYOUT_ENTRIES) {
			SetLastError(ERROR_INVALID_PARAMETER);
			return FALSE;
		}
		memset(&lo, 0, sizeof(lo));
		lo.style = dl->PartitionStyle;
		lo.count = (int)dl->PartitionCount;
		for (i = 0; i < lo.count; i++) {
			const PARTITION_INFORMATION_EX* pe = &dl->PartitionEntry[i];
			int k;
			if ((pe->StartingOffset.QuadPart % bi.sector_size) || (pe->PartitionLength.QuadPart % bi.sector_size)) {
				uprintf("Partition %d is not aligned to the sector size", i + 1);
				SetLastError(ERROR_INVALID_PARAMETER);
				return FALSE;
			}
			lo.part[i].start_lba = pe->StartingOffset.QuadPart / bi.sector_size;
			lo.part[i].num_sectors = pe->PartitionLength.QuadPart / bi.sector_size;
			lo.part[i].number = pe->PartitionNumber;
			if (lo.style == PARTITION_STYLE_MBR) {
				lo.part[i].mbr_type = pe->Mbr.PartitionType;
				lo.part[i].boot = pe->Mbr.BootIndicator;
				if (lo.part[i].start_lba > 0xFFFFFFFFULL || lo.part[i].num_sectors > 0xFFFFFFFFULL) {
					uprintf("Partition %d does not fit in an MBR partition table", i + 1);
					SetLastError(ERROR_INVALID_PARAMETER);
					return FALSE;
				}
			} else {
				lo.part[i].type_guid = pe->Gpt.PartitionType;
				lo.part[i].part_guid = pe->Gpt.PartitionId;
				lo.part[i].attributes = pe->Gpt.Attributes;
				for (k = 0; k < 36; k++)
					lo.part[i].name[k] = (uint16_t)pe->Gpt.Name[k];
			}
		}
		if (lo.style == PARTITION_STYLE_MBR) {
			r = write_mbr_table(bi.fd, bi.sector_size, dl->Mbr.Signature, &lo);
		} else if (lo.style == PARTITION_STYLE_GPT) {
			lo.disk_guid = dl->Gpt.DiskId;
			lo.first_usable_lba = dl->Gpt.StartingUsableOffset.QuadPart / bi.sector_size;
			lo.last_usable_lba = (dl->Gpt.StartingUsableOffset.QuadPart + dl->Gpt.UsableLength.QuadPart) / bi.sector_size - 1;
			r = write_gpt_table(bi.fd, bi.sector_size, disk_size, &lo);
		} else {
			SetLastError(ERROR_INVALID_PARAMETER);
			return FALSE;
		}
		fsync(bi.fd);
		break;
	}

	case IOCTL_DISK_UPDATE_PROPERTIES:
		if (bi.is_partition) {
			r = TRUE;
			break;
		}
		r = update_kernel_partitions(bi.fd, bi.sector_size, disk_size, bi.disk_kname);
		if (!r)
			SetLastError(ERROR_GEN_FAILURE);
		break;

	case IOCTL_DISK_GET_PARTITION_INFO:
	case IOCTL_DISK_GET_PARTITION_INFO_EX: {
		uint8_t type = 0;
		BOOL boot = FALSE;
		int style = PARTITION_STYLE_MBR;
		GUID type_guid = GUID_NULL, part_guid = GUID_NULL;
		/* Find our partition in the parent disk's table, for the type information */
		if (bi.is_partition) {
			char dev[128];
			int dfd;
			snprintf(dev, sizeof(dev), "/dev/%s", bi.disk_kname);
			dfd = open(dev, O_RDONLY | O_CLOEXEC);
			if (dfd >= 0) {
				int ss = 512;
				uint64_t dsize = 0;
				ioctl(dfd, BLKSSZGET, &ss);
				ioctl(dfd, BLKGETSIZE64, &dsize);
				if (read_layout(dfd, (uint32_t)ss, dsize, &lo)) {
					style = lo.style;
					for (i = 0; i < lo.count; i++) {
						if (lo.part[i].start_lba * (uint64_t)ss == bi.part_start) {
							type = lo.part[i].mbr_type;
							boot = lo.part[i].boot;
							type_guid = lo.part[i].type_guid;
							part_guid = lo.part[i].part_guid;
							break;
						}
					}
				}
				close(dfd);
			}
		}
		if (dwIoControlCode == IOCTL_DISK_GET_PARTITION_INFO) {
			PPARTITION_INFORMATION pi = (PPARTITION_INFORMATION)lpOutBuffer;
			if (pi == NULL || nOutBufferSize < sizeof(*pi)) {
				SetLastError(ERROR_INSUFFICIENT_BUFFER);
				return FALSE;
			}
			/* Like Windows, this fails for GPT partitions (callers then use the _EX version) */
			if (style == PARTITION_STYLE_GPT) {
				SetLastError(ERROR_INVALID_FUNCTION);
				return FALSE;
			}
			memset(pi, 0, sizeof(*pi));
			pi->StartingOffset.QuadPart = bi.part_start;
			pi->PartitionLength.QuadPart = disk_size;
			pi->HiddenSectors = (DWORD)(bi.part_start / bi.sector_size);
			pi->PartitionNumber = bi.part_number;
			pi->PartitionType = type;
			pi->BootIndicator = boot;
			pi->RecognizedPartition = TRUE;
			returned = sizeof(*pi);
		} else {
			PPARTITION_INFORMATION_EX pe = (PPARTITION_INFORMATION_EX)lpOutBuffer;
			if (pe == NULL || nOutBufferSize < sizeof(*pe)) {
				SetLastError(ERROR_INSUFFICIENT_BUFFER);
				return FALSE;
			}
			memset(pe, 0, sizeof(*pe));
			pe->PartitionStyle = (style == PARTITION_STYLE_GPT) ? PARTITION_STYLE_GPT : PARTITION_STYLE_MBR;
			pe->StartingOffset.QuadPart = bi.part_start;
			pe->PartitionLength.QuadPart = disk_size;
			pe->PartitionNumber = bi.part_number;
			if (pe->PartitionStyle == PARTITION_STYLE_GPT) {
				pe->Gpt.PartitionType = type_guid;
				pe->Gpt.PartitionId = part_guid;
			} else {
				pe->Mbr.PartitionType = type;
				pe->Mbr.BootIndicator = boot;
				pe->Mbr.RecognizedPartition = TRUE;
				pe->Mbr.HiddenSectors = (DWORD)(bi.part_start / bi.sector_size);
			}
			returned = sizeof(*pe);
		}
		r = TRUE;
		break;
	}

	case IOCTL_STORAGE_GET_DEVICE_NUMBER: {
		PSTORAGE_DEVICE_NUMBER sdn = (PSTORAGE_DEVICE_NUMBER)lpOutBuffer;
		DWORD index = linux_get_drive_index(bi.disk_kname);
		if (sdn == NULL || nOutBufferSize < sizeof(*sdn)) {
			SetLastError(ERROR_INSUFFICIENT_BUFFER);
			return FALSE;
		}
		if (index < DRIVE_INDEX_MIN) {
			SetLastError(ERROR_FILE_NOT_FOUND);
			return FALSE;
		}
		sdn->DeviceType = FILE_DEVICE_DISK;
		sdn->DeviceNumber = index - DRIVE_INDEX_MIN;
		sdn->PartitionNumber = bi.part_number;
		returned = sizeof(*sdn);
		r = TRUE;
		break;
	}

	case FSCTL_DISMOUNT_VOLUME: {
		/* Unmount every mount of this block device */
		extern BOOL linux_unmount_device(const char* kname, BOOL bSilent);
		r = linux_unmount_device(bi.kname, TRUE);
		break;
	}

	default:
		SetLastError(ERROR_NOT_SUPPORTED);
		return FALSE;
	}

	if (lpBytesReturned != NULL)
		*lpBytesReturned = returned;
	return r;
}
