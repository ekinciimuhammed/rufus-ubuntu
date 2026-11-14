/*
 * Rufus for Linux: file mapping (CreateFileMapping/MapViewOfFile) on top of mmap()
 * Copyright © 2026 Rufus Linux port contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */
#include <windows.h>
#include <pthread.h>
#include <sys/mman.h>

typedef struct {
	uint32_t magic;
	int fd;
	uint64_t size;
} file_mapping;

#define MAPPING_MAGIC 0x50414D46	/* 'FMAP' */
#define MAX_VIEWS 64

static struct { void* addr; size_t len; } views[MAX_VIEWS];
static pthread_mutex_t views_lock = PTHREAD_MUTEX_INITIALIZER;

HANDLE CreateFileMappingA(HANDLE hFile, LPSECURITY_ATTRIBUTES lpAttributes, DWORD flProtect,
	DWORD dwMaximumSizeHigh, DWORD dwMaximumSizeLow, LPCSTR lpName)
{
	file_mapping* m;
	struct stat st;
	int fd = HandleToFd(hFile);
	(void)lpAttributes; (void)flProtect; (void)lpName;
	if (fd < 0 || fstat(fd, &st) != 0) {
		SetLastError(ERROR_INVALID_HANDLE);
		return NULL;
	}
	m = calloc(1, sizeof(*m));
	if (m == NULL)
		return NULL;
	m->magic = MAPPING_MAGIC;
	m->fd = dup(fd);
	m->size = ((uint64_t)dwMaximumSizeHigh << 32) | dwMaximumSizeLow;
	if (m->size == 0)
		m->size = (uint64_t)st.st_size;
	return (HANDLE)m;
}

LPVOID MapViewOfFile(HANDLE hFileMappingObject, DWORD dwDesiredAccess, DWORD dwFileOffsetHigh,
	DWORD dwFileOffsetLow, SIZE_T dwNumberOfBytesToMap)
{
	file_mapping* m = (file_mapping*)hFileMappingObject;
	uint64_t offset = ((uint64_t)dwFileOffsetHigh << 32) | dwFileOffsetLow;
	size_t len;
	void* p;
	int i;
	(void)dwDesiredAccess;
	if (m == NULL || m->magic != MAPPING_MAGIC || offset > m->size) {
		SetLastError(ERROR_INVALID_PARAMETER);
		return NULL;
	}
	len = (dwNumberOfBytesToMap == 0) ? (size_t)(m->size - offset) : dwNumberOfBytesToMap;
	if (len == 0) {
		SetLastError(ERROR_INVALID_PARAMETER);
		return NULL;
	}
	p = mmap(NULL, len, PROT_READ, MAP_PRIVATE, m->fd, (off_t)offset);
	if (p == MAP_FAILED) {
		SetLastError(ErrnoToWin32(errno));
		return NULL;
	}
	pthread_mutex_lock(&views_lock);
	for (i = 0; i < MAX_VIEWS; i++) {
		if (views[i].addr == NULL) {
			views[i].addr = p;
			views[i].len = len;
			break;
		}
	}
	pthread_mutex_unlock(&views_lock);
	return p;
}

BOOL UnmapViewOfFile(LPCVOID lpBaseAddress)
{
	int i;
	BOOL r = FALSE;
	pthread_mutex_lock(&views_lock);
	for (i = 0; i < MAX_VIEWS; i++) {
		if (views[i].addr == lpBaseAddress) {
			munmap(views[i].addr, views[i].len);
			views[i].addr = NULL;
			r = TRUE;
			break;
		}
	}
	pthread_mutex_unlock(&views_lock);
	return r;
}
