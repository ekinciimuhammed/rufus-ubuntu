/*
 * Rufus for Linux: POSIX implementation of the Win32 subset used by Rufus
 * Copyright © 2026 Rufus Linux port contributors
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
#define RUFUS_COMPAT_INTERNAL
#include <windows.h>
#include <msapi_utf8_linux.h>
#include <pthread.h>
#include <dirent.h>
#include <fnmatch.h>
#include <libgen.h>
#include <pwd.h>
#include <sched.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <sys/sendfile.h>
#include <sys/syscall.h>
#include <linux/fs.h>
#include <utime.h>

#include "compat.h"

/* ------------------------------------------------------------------------- */
/* Last error                                                                */
/* ------------------------------------------------------------------------- */
static __thread DWORD last_error = 0;

DWORD GetLastError(void)
{
	return last_error;
}

void SetLastError(DWORD dwErrCode)
{
	last_error = dwErrCode;
}

DWORD ErrnoToWin32(int err)
{
	switch (err) {
	case 0: return ERROR_SUCCESS;
	case ENOENT: return ERROR_FILE_NOT_FOUND;
	case ENOTDIR: return ERROR_PATH_NOT_FOUND;
	case EMFILE: case ENFILE: return ERROR_TOO_MANY_OPEN_FILES;
	case EACCES: case EPERM: return ERROR_ACCESS_DENIED;
	case EBADF: return ERROR_INVALID_HANDLE;
	case ENOMEM: return ERROR_NOT_ENOUGH_MEMORY;
	case EROFS: return ERROR_WRITE_PROTECT;
	case ENODEV: case ENXIO: return ERROR_DEV_NOT_EXIST;
	case ENOMEDIUM: return ERROR_NO_MEDIA_IN_DRIVE;
	case EIO: return ERROR_IO_DEVICE;
	case ESPIPE: return ERROR_SEEK;
	case EBUSY: return ERROR_BUSY;
	case ETXTBSY: return ERROR_SHARING_VIOLATION;
	case EEXIST: return ERROR_ALREADY_EXISTS;
	case EINVAL: return ERROR_INVALID_PARAMETER;
	case ENOSPC: return ERROR_DISK_FULL;
	case EDQUOT: return ERROR_NOT_ENOUGH_QUOTA;
	case ENAMETOOLONG: return ERROR_FILENAME_EXCED_RANGE;
	case ENOTEMPTY: return ERROR_DIR_NOT_EMPTY;
	case EFBIG: return ERROR_FILE_TOO_LARGE;
	case EPIPE: return ERROR_BROKEN_PIPE;
	case ETIMEDOUT: return ERROR_TIMEOUT;
	case ECANCELED: return ERROR_CANCELLED;
	case ENOSYS: case EOPNOTSUPP: return ERROR_NOT_SUPPORTED;
	case EAGAIN: return ERROR_NOT_READY;
	case EINTR: return ERROR_OPERATION_ABORTED;
	case ELOOP: return ERROR_CANT_RESOLVE_FILENAME;
	case EXDEV: return ERROR_NOT_SAME_DEVICE_COMPAT;
	default: return ERROR_GEN_FAILURE;
	}
}

#define SET_ERRNO_ERROR() SetLastError(ErrnoToWin32(errno))

/* ------------------------------------------------------------------------- */
/* Handles                                                                   */
/* ------------------------------------------------------------------------- */
enum handle_type {
	HT_FILE = 0x46494C45,	// 'FILE'
	HT_THREAD,
	HT_EVENT,
	HT_MUTEX,
	HT_FIND,
	HT_PSEUDO,
};

struct _RUFUS_HANDLE {
	uint32_t type;
	/* File */
	int fd;
	char* path;
	BOOL append;
	BOOL overlapped;	/* Opened with FILE_FLAG_OVERLAPPED */
	/* Thread / Event / Mutex */
	pthread_t thread;
	pthread_mutex_t mutex;
	pthread_cond_t cond;
	BOOL signaled;
	BOOL manual_reset;
	BOOL detached;
	DWORD exit_code;
	LPTHREAD_START_ROUTINE start;
	LPVOID param;
	/* Find */
	DIR* dir;
	char* pattern;
	char* dirname;
};

static struct _RUFUS_HANDLE current_process = { .type = HT_PSEUDO, .fd = -1 };
static struct _RUFUS_HANDLE current_thread = { .type = HT_PSEUDO, .fd = -1 };

static BOOL is_valid(HANDLE h, uint32_t type)
{
	return (h != NULL) && (h != INVALID_HANDLE_VALUE) && (h->type == type);
}

static HANDLE new_handle(uint32_t type)
{
	HANDLE h = calloc(1, sizeof(struct _RUFUS_HANDLE));
	if (h == NULL) {
		SetLastError(ERROR_NOT_ENOUGH_MEMORY);
		return NULL;
	}
	h->type = type;
	h->fd = -1;
	pthread_mutex_init(&h->mutex, NULL);
	pthread_cond_init(&h->cond, NULL);
	return h;
}

static void free_handle(HANDLE h)
{
	pthread_mutex_destroy(&h->mutex);
	pthread_cond_destroy(&h->cond);
	free(h->path);
	free(h->pattern);
	free(h->dirname);
	h->type = 0;
	free(h);
}

/*
 * Virtual drive letters -> mount points.
 * Like Windows, a drive letter can also be assigned to a volume that doesn't have a
 * file system yet (e.g. before it is formatted): such a letter is "pending", and the
 * volume is mounted the first time a path on that drive letter is used.
 */
static char* drive_letter_mount[26];
static char* drive_letter_pending[26];
static BOOL (*pending_mount_cb)(char letter, const char* dev) = NULL;
static pthread_mutex_t drive_letter_lock = PTHREAD_MUTEX_INITIALIZER;

void linux_set_pending_mount_callback(BOOL (*cb)(char letter, const char* dev))
{
	pending_mount_cb = cb;
}

BOOL linux_reserve_drive_letter(char letter, const char* dev)
{
	int i = toupper((unsigned char)letter) - 'A';
	if (i < 0 || i >= 26 || dev == NULL)
		return FALSE;
	pthread_mutex_lock(&drive_letter_lock);
	free(drive_letter_pending[i]);
	drive_letter_pending[i] = strdup(dev);
	pthread_mutex_unlock(&drive_letter_lock);
	return TRUE;
}

BOOL linux_is_drive_letter_pending(char letter)
{
	int i = toupper((unsigned char)letter) - 'A';
	return (i >= 0 && i < 26 && drive_letter_pending[i] != NULL);
}

/* Mount the volume of a pending drive letter (must be called without the lock held) */
static void mount_pending_drive_letter(char letter)
{
	int i = toupper((unsigned char)letter) - 'A';
	char* dev = NULL;
	if (i < 0 || i >= 26 || pending_mount_cb == NULL)
		return;
	pthread_mutex_lock(&drive_letter_lock);
	if (drive_letter_mount[i] == NULL && drive_letter_pending[i] != NULL)
		dev = strdup(drive_letter_pending[i]);
	pthread_mutex_unlock(&drive_letter_lock);
	if (dev == NULL)
		return;
	if (pending_mount_cb((char)('A' + i), dev)) {
		pthread_mutex_lock(&drive_letter_lock);
		free(drive_letter_pending[i]);
		drive_letter_pending[i] = NULL;
		pthread_mutex_unlock(&drive_letter_lock);
	}
	free(dev);
}

BOOL linux_map_drive_letter(char letter, const char* mount_point)
{
	int i = toupper((unsigned char)letter) - 'A';
	if (i < 0 || i >= 26 || mount_point == NULL)
		return FALSE;
	pthread_mutex_lock(&drive_letter_lock);
	free(drive_letter_mount[i]);
	drive_letter_mount[i] = strdup(mount_point);
	pthread_mutex_unlock(&drive_letter_lock);
	return TRUE;
}

void linux_unmap_drive_letter(char letter)
{
	int i = toupper((unsigned char)letter) - 'A';
	if (i < 0 || i >= 26)
		return;
	pthread_mutex_lock(&drive_letter_lock);
	free(drive_letter_mount[i]);
	drive_letter_mount[i] = NULL;
	free(drive_letter_pending[i]);
	drive_letter_pending[i] = NULL;
	pthread_mutex_unlock(&drive_letter_lock);
}

static const char* get_drive_letter_mount(char letter)
{
	int i = toupper((unsigned char)letter) - 'A';
	return (i < 0 || i >= 26) ? NULL : drive_letter_mount[i];
}

const char* linux_get_drive_letter_mount(char letter)
{
	if (linux_is_drive_letter_pending(letter))
		mount_pending_drive_letter(letter);
	return get_drive_letter_mount(letter);
}

char linux_get_drive_letter_for_mount(const char* mount_point)
{
	int i;
	char r = 0;
	pthread_mutex_lock(&drive_letter_lock);
	for (i = 0; i < 26 && mount_point != NULL; i++) {
		if (drive_letter_mount[i] != NULL && strcmp(drive_letter_mount[i], mount_point) == 0) {
			r = (char)('A' + i);
			break;
		}
	}
	pthread_mutex_unlock(&drive_letter_lock);
	return r;
}

char* linux_path(const char* path)
{
	char* p;
	const char* mount;
	if (path == NULL)
		return NULL;
	/* "X:", "X:\..." or "X:/..." with X a mapped drive letter */
	if (isalpha((unsigned char)path[0]) && path[1] == ':' &&
		(path[2] == 0 || path[2] == '\\' || path[2] == '/')) {
		if (linux_is_drive_letter_pending(path[0]))
			mount_pending_drive_letter(path[0]);
		pthread_mutex_lock(&drive_letter_lock);
		mount = get_drive_letter_mount(path[0]);
		if (mount != NULL) {
			const char* rest = (path[2] == 0) ? "" : &path[3];
			if (asprintf(&p, "%s/%s", mount, rest) < 0)
				p = NULL;
			pthread_mutex_unlock(&drive_letter_lock);
			if (p != NULL)
				to_unix_path_inplace(p);
			return p;
		}
		pthread_mutex_unlock(&drive_letter_lock);
	}
	p = strdup(path);
	if (p != NULL)
		to_unix_path_inplace(p);
	return p;
}

const char* linux_path_tmp(const char* path)
{
	static __thread char* last = NULL;
	free(last);
	last = linux_path(path);
	return (last == NULL) ? "" : last;
}

char* linux_wim_path(const char* path)
{
	return linux_path(path);
}

HANDLE FdToHandle(int fd, const char* path)
{
	HANDLE h;
	if (fd < 0)
		return INVALID_HANDLE_VALUE;
	h = new_handle(HT_FILE);
	if (h == NULL)
		return INVALID_HANDLE_VALUE;
	h->fd = fd;
	h->path = (path == NULL) ? NULL : strdup(path);
	return h;
}

int HandleToFd(HANDLE h)
{
	return is_valid(h, HT_FILE) ? h->fd : -1;
}

const char* HandleToPath(HANDLE h)
{
	return is_valid(h, HT_FILE) ? h->path : NULL;
}

intptr_t _get_osfhandle(int fd)
{
	/* Only used for logging/compatibility: callers get a new handle on the fd */
	return (intptr_t)FdToHandle(dup(fd), NULL);
}

int _open_osfhandle(intptr_t osfhandle, int flags)
{
	(void)flags;
	/* The fd remains owned by the handle, which the caller closes */
	return HandleToFd((HANDLE)osfhandle);
}

HANDLE CreateFileA(LPCSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode,
	LPSECURITY_ATTRIBUTES lpSecurityAttributes, DWORD dwCreationDisposition,
	DWORD dwFlagsAndAttributes, HANDLE hTemplateFile)
{
	int flags = O_CLOEXEC, fd;
	char* path;
	struct stat st;
	BOOL existed;
	HANDLE h;

	(void)dwShareMode; (void)lpSecurityAttributes; (void)hTemplateFile;
	if (lpFileName == NULL) {
		SetLastError(ERROR_INVALID_PARAMETER);
		return INVALID_HANDLE_VALUE;
	}
	path = linux_path(lpFileName);

	if ((dwDesiredAccess & (GENERIC_READ | FILE_READ_DATA | GENERIC_ALL)) &&
		(dwDesiredAccess & (GENERIC_WRITE | FILE_WRITE_DATA | FILE_APPEND_DATA | GENERIC_ALL)))
		flags |= O_RDWR;
	else if (dwDesiredAccess & (GENERIC_WRITE | FILE_WRITE_DATA | FILE_APPEND_DATA))
		flags |= O_WRONLY;
	else
		flags |= O_RDONLY;
	if (dwFlagsAndAttributes & FILE_FLAG_WRITE_THROUGH)
		flags |= O_SYNC;
	/* NB: we don't map FILE_FLAG_NO_BUFFERING to O_DIRECT, as the alignment requirements
	 * aren't always honoured by callers. Block device writes are explicitly flushed. */

	existed = (stat(path, &st) == 0);
	switch (dwCreationDisposition) {
	case CREATE_NEW:
		flags |= O_CREAT | O_EXCL;
		break;
	case CREATE_ALWAYS:
		flags |= O_CREAT | O_TRUNC;
		break;
	case OPEN_ALWAYS:
		flags |= O_CREAT;
		break;
	case TRUNCATE_EXISTING:
		flags |= O_TRUNC;
		break;
	case OPEN_EXISTING:
	default:
		break;
	}
	/* Opening a directory (FILE_FLAG_BACKUP_SEMANTICS) */
	if (existed && S_ISDIR(st.st_mode)) {
		flags &= ~(O_WRONLY | O_RDWR | O_CREAT | O_TRUNC);
		flags |= O_RDONLY | O_DIRECTORY;
	}

	fd = open(path, flags, 0644);
	if (fd < 0) {
		DWORD err = ErrnoToWin32(errno);
		if (errno == EEXIST)
			err = ERROR_FILE_EXISTS;
		free(path);
		SetLastError(err);
		return INVALID_HANDLE_VALUE;
	}
	h = FdToHandle(fd, path);
	free(path);
	if (h == INVALID_HANDLE_VALUE) {
		close(fd);
		return INVALID_HANDLE_VALUE;
	}
	if (dwDesiredAccess == FILE_APPEND_DATA)
		h->append = TRUE;
	h->overlapped = ((dwFlagsAndAttributes & FILE_FLAG_OVERLAPPED) != 0);
	/* Windows sets ERROR_ALREADY_EXISTS on success for CREATE_ALWAYS/OPEN_ALWAYS */
	SetLastError(((dwCreationDisposition == CREATE_ALWAYS || dwCreationDisposition == OPEN_ALWAYS) && existed) ?
		ERROR_ALREADY_EXISTS : ERROR_SUCCESS);
	return h;
}

static BOOL close_thread_handle(HANDLE h)
{
	BOOL detach;
	pthread_mutex_lock(&h->mutex);
	/* If the thread is still running, it frees the handle itself on exit */
	detach = !h->signaled;
	h->detached = TRUE;
	pthread_mutex_unlock(&h->mutex);
	if (!detach) {
		pthread_join(h->thread, NULL);
		free_handle(h);
	}
	return TRUE;
}

BOOL CloseHandle(HANDLE h)
{
	if (h == NULL || h == INVALID_HANDLE_VALUE) {
		SetLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	/* File mapping objects (see mmap_linux.c) */
	if (h->type == 0x50414D46) {
		close(((int*)h)[1]);
		free(h);
		return TRUE;
	}
	switch (h->type) {
	case HT_FILE:
		if (h->fd >= 0 && close(h->fd) != 0) {
			SET_ERRNO_ERROR();
			free_handle(h);
			return FALSE;
		}
		free_handle(h);
		return TRUE;
	case HT_THREAD:
		return close_thread_handle(h);
	case HT_EVENT:
	case HT_MUTEX:
		free_handle(h);
		return TRUE;
	case HT_FIND:
		return FindClose(h);
	case HT_PSEUDO:
		return TRUE;
	default:
		SetLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}
}

static off_t overlapped_offset(LPOVERLAPPED o)
{
	return (off_t)(((uint64_t)o->OffsetHigh << 32) | o->Offset);
}

BOOL ReadFile(HANDLE hFile, LPVOID lpBuffer, DWORD nNumberOfBytesToRead, LPDWORD lpNumberOfBytesRead, LPOVERLAPPED lpOverlapped)
{
	size_t total = 0;
	ssize_t r;

	if (lpNumberOfBytesRead != NULL)
		*lpNumberOfBytesRead = 0;
	if (!is_valid(hFile, HT_FILE)) {
		SetLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	while (total < nNumberOfBytesToRead) {
		if (lpOverlapped != NULL)
			r = pread(hFile->fd, (uint8_t*)lpBuffer + total, nNumberOfBytesToRead - total, overlapped_offset(lpOverlapped) + total);
		else
			r = read(hFile->fd, (uint8_t*)lpBuffer + total, nNumberOfBytesToRead - total);
		if (r < 0) {
			if (errno == EINTR)
				continue;
			SET_ERRNO_ERROR();
			if (lpNumberOfBytesRead != NULL)
				*lpNumberOfBytesRead = (DWORD)total;
			return FALSE;
		}
		if (r == 0)
			break;
		total += r;
	}
	if (lpNumberOfBytesRead != NULL)
		*lpNumberOfBytesRead = (DWORD)total;
	/*
	 * Like Windows: on a handle that wasn't opened with FILE_FLAG_OVERLAPPED, an
	 * OVERLAPPED operation starts at the provided offset *and* updates the file pointer.
	 */
	if (lpOverlapped != NULL && !hFile->overlapped)
		lseek(hFile->fd, (off_t)(overlapped_offset(lpOverlapped) + total), SEEK_SET);
	if (lpOverlapped != NULL) {
		lpOverlapped->Internal = 0;
		lpOverlapped->InternalHigh = total;
		if (lpOverlapped->hEvent != NULL)
			SetEvent(lpOverlapped->hEvent);
	}
	SetLastError((total == 0 && nNumberOfBytesToRead != 0 && lpOverlapped != NULL) ? ERROR_HANDLE_EOF : ERROR_SUCCESS);
	return TRUE;
}

BOOL WriteFile(HANDLE hFile, LPCVOID lpBuffer, DWORD nNumberOfBytesToWrite, LPDWORD lpNumberOfBytesWritten, LPOVERLAPPED lpOverlapped)
{
	size_t total = 0;
	ssize_t r;

	if (lpNumberOfBytesWritten != NULL)
		*lpNumberOfBytesWritten = 0;
	if (!is_valid(hFile, HT_FILE)) {
		SetLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	if (hFile->append)
		lseek(hFile->fd, 0, SEEK_END);
	while (total < nNumberOfBytesToWrite) {
		if (lpOverlapped != NULL)
			r = pwrite(hFile->fd, (const uint8_t*)lpBuffer + total, nNumberOfBytesToWrite - total, overlapped_offset(lpOverlapped) + total);
		else
			r = write(hFile->fd, (const uint8_t*)lpBuffer + total, nNumberOfBytesToWrite - total);
		if (r < 0) {
			if (errno == EINTR)
				continue;
			SET_ERRNO_ERROR();
			if (lpNumberOfBytesWritten != NULL)
				*lpNumberOfBytesWritten = (DWORD)total;
			return FALSE;
		}
		if (r == 0) {
			SetLastError(ERROR_WRITE_FAULT);
			if (lpNumberOfBytesWritten != NULL)
				*lpNumberOfBytesWritten = (DWORD)total;
			return FALSE;
		}
		total += r;
	}
	if (lpNumberOfBytesWritten != NULL)
		*lpNumberOfBytesWritten = (DWORD)total;
	/* See ReadFile() */
	if (lpOverlapped != NULL && !hFile->overlapped)
		lseek(hFile->fd, (off_t)(overlapped_offset(lpOverlapped) + total), SEEK_SET);
	if (lpOverlapped != NULL) {
		lpOverlapped->Internal = 0;
		lpOverlapped->InternalHigh = total;
		if (lpOverlapped->hEvent != NULL)
			SetEvent(lpOverlapped->hEvent);
	}
	SetLastError(ERROR_SUCCESS);
	return TRUE;
}

BOOL GetOverlappedResult(HANDLE hFile, LPOVERLAPPED lpOverlapped, LPDWORD lpNumberOfBytesTransferred, BOOL bWait)
{
	(void)hFile; (void)bWait;
	/* Our overlapped I/O always completes synchronously */
	if (lpNumberOfBytesTransferred != NULL)
		*lpNumberOfBytesTransferred = (DWORD)lpOverlapped->InternalHigh;
	return TRUE;
}

BOOL GetOverlappedResultEx(HANDLE hFile, LPOVERLAPPED lpOverlapped, LPDWORD lpNumberOfBytesTransferred,
	DWORD dwMilliseconds, BOOL bAlertable)
{
	(void)dwMilliseconds; (void)bAlertable;
	return GetOverlappedResult(hFile, lpOverlapped, lpNumberOfBytesTransferred, TRUE);
}

BOOL HasOverlappedIoCompleted_(LPOVERLAPPED lpOverlapped)
{
	(void)lpOverlapped;
	return TRUE;
}

BOOL CancelIo(HANDLE hFile)
{
	(void)hFile;
	return TRUE;
}

BOOL CancelIoEx(HANDLE hFile, LPOVERLAPPED lpOverlapped)
{
	(void)hFile; (void)lpOverlapped;
	return TRUE;
}

BOOL SetFilePointerEx(HANDLE hFile, LARGE_INTEGER liDistanceToMove, PLARGE_INTEGER lpNewFilePointer, DWORD dwMoveMethod)
{
	off_t r;
	int whence = (dwMoveMethod == FILE_CURRENT) ? SEEK_CUR : ((dwMoveMethod == FILE_END) ? SEEK_END : SEEK_SET);
	if (!is_valid(hFile, HT_FILE)) {
		SetLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	r = lseek(hFile->fd, (off_t)liDistanceToMove.QuadPart, whence);
	if (r == (off_t)-1) {
		SetLastError((errno == EINVAL) ? ERROR_NEGATIVE_SEEK : ErrnoToWin32(errno));
		return FALSE;
	}
	if (lpNewFilePointer != NULL)
		lpNewFilePointer->QuadPart = r;
	SetLastError(ERROR_SUCCESS);
	return TRUE;
}

DWORD SetFilePointer(HANDLE hFile, LONG lDistanceToMove, PLONG lpDistanceToMoveHigh, DWORD dwMoveMethod)
{
	LARGE_INTEGER li, np;
	li.QuadPart = (lpDistanceToMoveHigh != NULL) ?
		(LONGLONG)(((uint64_t)(uint32_t)*lpDistanceToMoveHigh << 32) | (uint32_t)lDistanceToMove) : (LONGLONG)lDistanceToMove;
	if (!SetFilePointerEx(hFile, li, &np, dwMoveMethod))
		return INVALID_SET_FILE_POINTER;
	if (lpDistanceToMoveHigh != NULL)
		*lpDistanceToMoveHigh = np.HighPart;
	return np.LowPart;
}

BOOL SetEndOfFile(HANDLE hFile)
{
	off_t pos;
	if (!is_valid(hFile, HT_FILE)) {
		SetLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	pos = lseek(hFile->fd, 0, SEEK_CUR);
	if (pos == (off_t)-1 || ftruncate(hFile->fd, pos) != 0) {
		SET_ERRNO_ERROR();
		return FALSE;
	}
	return TRUE;
}

static int64_t fd_size(int fd)
{
	struct stat st;
	uint64_t size;
	if (fstat(fd, &st) != 0)
		return -1;
	if (S_ISBLK(st.st_mode)) {
		if (ioctl(fd, BLKGETSIZE64, &size) != 0)
			return -1;
		return (int64_t)size;
	}
	return (int64_t)st.st_size;
}

BOOL GetFileSizeEx(HANDLE hFile, PLARGE_INTEGER lpFileSize)
{
	int64_t size;
	if (!is_valid(hFile, HT_FILE)) {
		SetLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	size = fd_size(hFile->fd);
	if (size < 0) {
		SET_ERRNO_ERROR();
		return FALSE;
	}
	lpFileSize->QuadPart = size;
	return TRUE;
}

DWORD GetFileSize(HANDLE hFile, LPDWORD lpFileSizeHigh)
{
	LARGE_INTEGER li;
	if (!GetFileSizeEx(hFile, &li))
		return INVALID_FILE_SIZE;
	if (lpFileSizeHigh != NULL)
		*lpFileSizeHigh = (DWORD)li.HighPart;
	return li.LowPart;
}

BOOL FlushFileBuffers(HANDLE hFile)
{
	struct stat st;
	if (!is_valid(hFile, HT_FILE)) {
		SetLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	if (fsync(hFile->fd) != 0) {
		SET_ERRNO_ERROR();
		return FALSE;
	}
	if (fstat(hFile->fd, &st) == 0 && S_ISBLK(st.st_mode))
		ioctl(hFile->fd, BLKFLSBUF, 0);
	return TRUE;
}

/* FILETIME is 100ns intervals since 1601.01.01 */
#define EPOCH_DIFF_100NS 116444736000000000ULL

static void filetime_to_timespec(const FILETIME* ft, struct timespec* ts)
{
	uint64_t t = ((uint64_t)ft->dwHighDateTime << 32) | ft->dwLowDateTime;
	if (t < EPOCH_DIFF_100NS)
		t = EPOCH_DIFF_100NS;
	t -= EPOCH_DIFF_100NS;
	ts->tv_sec = (time_t)(t / 10000000ULL);
	ts->tv_nsec = (long)((t % 10000000ULL) * 100);
}

static void timespec_to_filetime(const struct timespec* ts, FILETIME* ft)
{
	uint64_t t = (uint64_t)ts->tv_sec * 10000000ULL + ts->tv_nsec / 100 + EPOCH_DIFF_100NS;
	ft->dwLowDateTime = (DWORD)t;
	ft->dwHighDateTime = (DWORD)(t >> 32);
}

BOOL SetFileTime(HANDLE hFile, const FILETIME* lpCreationTime, const FILETIME* lpLastAccessTime, const FILETIME* lpLastWriteTime)
{
	struct timespec ts[2] = { { 0, UTIME_OMIT }, { 0, UTIME_OMIT } };
	(void)lpCreationTime;
	if (!is_valid(hFile, HT_FILE)) {
		SetLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	if (lpLastAccessTime != NULL)
		filetime_to_timespec(lpLastAccessTime, &ts[0]);
	if (lpLastWriteTime != NULL)
		filetime_to_timespec(lpLastWriteTime, &ts[1]);
	if (futimens(hFile->fd, ts) != 0) {
		SET_ERRNO_ERROR();
		return FALSE;
	}
	return TRUE;
}

static DWORD mode_to_attributes(const char* path, mode_t mode)
{
	DWORD attr = 0;
	const char* base;
	if (S_ISDIR(mode))
		attr |= FILE_ATTRIBUTE_DIRECTORY;
	if (S_ISLNK(mode))
		attr |= FILE_ATTRIBUTE_REPARSE_POINT;
	if (!(mode & (S_IWUSR | S_IWGRP | S_IWOTH)))
		attr |= FILE_ATTRIBUTE_READONLY;
	if (path != NULL) {
		base = strrchr(path, '/');
		base = (base == NULL) ? path : base + 1;
		if (base[0] == '.' && base[1] != 0 && strcmp(base, "..") != 0)
			attr |= FILE_ATTRIBUTE_HIDDEN;
	}
	if (attr == 0)
		attr = FILE_ATTRIBUTE_NORMAL;
	else if (!(attr & FILE_ATTRIBUTE_DIRECTORY))
		attr |= FILE_ATTRIBUTE_ARCHIVE;
	return attr;
}

BOOL GetFileInformationByHandle(HANDLE hFile, LPBY_HANDLE_FILE_INFORMATION info)
{
	struct stat st;
	int64_t size;
	if (!is_valid(hFile, HT_FILE) || fstat(hFile->fd, &st) != 0) {
		SetLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	memset(info, 0, sizeof(*info));
	info->dwFileAttributes = mode_to_attributes(hFile->path, st.st_mode);
	timespec_to_filetime(&st.st_ctim, &info->ftCreationTime);
	timespec_to_filetime(&st.st_atim, &info->ftLastAccessTime);
	timespec_to_filetime(&st.st_mtim, &info->ftLastWriteTime);
	info->dwVolumeSerialNumber = (DWORD)st.st_dev;
	size = fd_size(hFile->fd);
	info->nFileSizeHigh = (DWORD)((uint64_t)size >> 32);
	info->nFileSizeLow = (DWORD)size;
	info->nNumberOfLinks = (DWORD)st.st_nlink;
	info->nFileIndexHigh = (DWORD)((uint64_t)st.st_ino >> 32);
	info->nFileIndexLow = (DWORD)st.st_ino;
	return TRUE;
}

DWORD GetFileType(HANDLE hFile)
{
	struct stat st;
	if (!is_valid(hFile, HT_FILE) || fstat(hFile->fd, &st) != 0)
		return FILE_TYPE_UNKNOWN;
	if (S_ISCHR(st.st_mode))
		return FILE_TYPE_CHAR;
	if (S_ISFIFO(st.st_mode) || S_ISSOCK(st.st_mode))
		return FILE_TYPE_PIPE;
	return FILE_TYPE_DISK;
}

/* DeviceIoControl() is implemented in ioctl_linux.c */

DWORD GetFileAttributesA(LPCSTR lpFileName)
{
	struct stat st;
	char* path = linux_path(lpFileName);
	DWORD r;
	if (path == NULL || stat(path, &st) != 0) {
		SET_ERRNO_ERROR();
		free(path);
		return INVALID_FILE_ATTRIBUTES;
	}
	r = mode_to_attributes(path, st.st_mode);
	free(path);
	return r;
}

BOOL SetFileAttributesA(LPCSTR lpFileName, DWORD dwFileAttributes)
{
	struct stat st;
	char* path = linux_path(lpFileName);
	BOOL r = FALSE;
	if (path == NULL || stat(path, &st) != 0) {
		SET_ERRNO_ERROR();
		goto out;
	}
	/* Only the read-only attribute has a POSIX equivalent (FAT attributes are set by the fs layer) */
	if (dwFileAttributes & FILE_ATTRIBUTE_READONLY)
		st.st_mode &= ~(S_IWUSR | S_IWGRP | S_IWOTH);
	else
		st.st_mode |= S_IWUSR;
	r = (chmod(path, st.st_mode & 07777) == 0) || (errno == EPERM);
	if (!r)
		SET_ERRNO_ERROR();
	/* Non-POSIX file systems (vfat, ntfs3) may legitimately refuse this */
	r = TRUE;
out:
	free(path);
	return r;
}

BOOL DeleteFileA(LPCSTR lpFileName)
{
	char* path = linux_path(lpFileName);
	BOOL r = (path != NULL) && (unlink(path) == 0);
	if (!r)
		SET_ERRNO_ERROR();
	free(path);
	return r;
}

static BOOL copy_fd(int in, int out)
{
	uint8_t buf[256 * 1024];
	ssize_t r, w, done;
	for (;;) {
		r = read(in, buf, sizeof(buf));
		if (r < 0 && errno == EINTR)
			continue;
		if (r < 0)
			return FALSE;
		if (r == 0)
			return TRUE;
		for (done = 0; done < r; done += w) {
			w = write(out, buf + done, r - done);
			if (w < 0 && errno == EINTR) {
				w = 0;
				continue;
			}
			if (w <= 0)
				return FALSE;
		}
	}
}

BOOL CopyFileA(LPCSTR lpExistingFileName, LPCSTR lpNewFileName, BOOL bFailIfExists)
{
	char *src = linux_path(lpExistingFileName), *dst = linux_path(lpNewFileName);
	int in = -1, out = -1;
	struct stat st;
	BOOL r = FALSE;

	if (src == NULL || dst == NULL) {
		SetLastError(ERROR_INVALID_PARAMETER);
		goto out;
	}
	in = open(src, O_RDONLY | O_CLOEXEC);
	if (in < 0 || fstat(in, &st) != 0) {
		SET_ERRNO_ERROR();
		goto out;
	}
	out = open(dst, O_WRONLY | O_CREAT | O_CLOEXEC | (bFailIfExists ? O_EXCL : O_TRUNC), st.st_mode & 0777);
	if (out < 0) {
		SetLastError((errno == EEXIST) ? ERROR_FILE_EXISTS : ErrnoToWin32(errno));
		goto out;
	}
	r = copy_fd(in, out);
	if (!r)
		SET_ERRNO_ERROR();
	else {
		struct timespec ts[2] = { st.st_atim, st.st_mtim };
		futimens(out, ts);
	}
out:
	if (in >= 0)
		close(in);
	if (out >= 0 && close(out) != 0 && r) {
		SET_ERRNO_ERROR();
		r = FALSE;
	}
	free(src);
	free(dst);
	return r;
}

BOOL MoveFileExA(LPCSTR lpExistingFileName, LPCSTR lpNewFileName, DWORD dwFlags)
{
	char *src = linux_path(lpExistingFileName), *dst = linux_path(lpNewFileName);
	struct stat st;
	BOOL r = FALSE;
	if (src == NULL || dst == NULL) {
		SetLastError(ERROR_INVALID_PARAMETER);
		goto out;
	}
	if (!(dwFlags & MOVEFILE_REPLACE_EXISTING) && lstat(dst, &st) == 0) {
		SetLastError(ERROR_ALREADY_EXISTS);
		goto out;
	}
	if (rename(src, dst) == 0) {
		r = TRUE;
	} else if (errno == EXDEV && (dwFlags & MOVEFILE_COPY_ALLOWED)) {
		r = CopyFileA(src, dst, FALSE) && (unlink(src) == 0);
	} else {
		SET_ERRNO_ERROR();
	}
out:
	free(src);
	free(dst);
	return r;
}

BOOL MoveFileA(LPCSTR lpExistingFileName, LPCSTR lpNewFileName)
{
	return MoveFileExA(lpExistingFileName, lpNewFileName, MOVEFILE_COPY_ALLOWED);
}

BOOL CreateDirectoryA(LPCSTR lpPathName, LPSECURITY_ATTRIBUTES lpSecurityAttributes)
{
	char* path = linux_path(lpPathName);
	BOOL r;
	(void)lpSecurityAttributes;
	r = (path != NULL) && (mkdir(path, 0755) == 0);
	if (!r)
		SET_ERRNO_ERROR();
	free(path);
	return r;
}

BOOL RemoveDirectoryA(LPCSTR lpPathName)
{
	char* path = linux_path(lpPathName);
	BOOL r = (path != NULL) && (rmdir(path) == 0);
	if (!r)
		SET_ERRNO_ERROR();
	free(path);
	return r;
}

BOOL PathFileExistsA(LPCSTR pszPath)
{
	struct stat st;
	char* path = linux_path(pszPath);
	BOOL r = (path != NULL) && (stat(path, &st) == 0);
	free(path);
	return r;
}

BOOLEAN CreateSymbolicLinkA(LPCSTR lpSymlinkFileName, LPCSTR lpTargetFileName, DWORD dwFlags)
{
	char *link = linux_path(lpSymlinkFileName), *target = linux_path(lpTargetFileName);
	BOOLEAN r;
	(void)dwFlags;
	r = (link != NULL) && (target != NULL) && (symlink(target, link) == 0);
	if (!r)
		SET_ERRNO_ERROR();
	free(link);
	free(target);
	return r;
}

BOOLEAN CreateSymbolicLinkU(const char* lpSymlinkFileName, const char* lpTargetFileName, DWORD dwFlags)
{
	return CreateSymbolicLinkA(lpSymlinkFileName, lpTargetFileName, dwFlags);
}

DWORD GetTempPathA(DWORD nBufferLength, LPSTR lpBuffer)
{
	const char* tmp = getenv("TMPDIR");
	int n;
	if (tmp == NULL || tmp[0] == 0)
		tmp = "/tmp";
	n = snprintf(lpBuffer, nBufferLength, "%s/", tmp);
	return (n < 0 || (DWORD)n >= nBufferLength) ? 0 : (DWORD)n;
}

UINT GetTempFileNameA(LPCSTR lpPathName, LPCSTR lpPrefixString, UINT uUnique, LPSTR lpTempFileName)
{
	char* dir = linux_path(lpPathName);
	size_t len;
	int fd;
	if (dir == NULL) {
		SetLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}
	len = strlen(dir);
	while (len > 1 && dir[len - 1] == '/')
		dir[--len] = 0;
	if (uUnique != 0) {
		sprintf(lpTempFileName, "%s/%.3s%04X.TMP", dir, (lpPrefixString == NULL) ? "" : lpPrefixString, uUnique & 0xFFFF);
		free(dir);
		return uUnique;
	}
	sprintf(lpTempFileName, "%s/%.3sXXXXXX", dir, (lpPrefixString == NULL) ? "" : lpPrefixString);
	free(dir);
	fd = mkstemp(lpTempFileName);
	if (fd < 0) {
		SET_ERRNO_ERROR();
		return 0;
	}
	close(fd);
	return 1;
}

DWORD GetTempDirNameU(char* lpPathName, char* lpPrefixString, UINT uUnique, char* lpTempDirName)
{
	DWORD ret = GetTempFileNameA(lpPathName, lpPrefixString, uUnique, lpTempDirName);
	if (ret != 0) {
		DeleteFileA(lpTempDirName);
		if (!CreateDirectoryA(lpTempDirName, NULL))
			ret = 0;
	}
	return ret;
}

DWORD GetCurrentDirectoryA(DWORD nBufferLength, LPSTR lpBuffer)
{
	if (getcwd(lpBuffer, nBufferLength) == NULL) {
		SET_ERRNO_ERROR();
		return 0;
	}
	return (DWORD)strlen(lpBuffer);
}

BOOL SetCurrentDirectoryA(LPCSTR lpPathName)
{
	return _chdirU(lpPathName) == 0;
}

BOOL GetDiskFreeSpaceExA(LPCSTR lpDirectoryName, PULARGE_INTEGER lpFreeBytesAvailableToCaller,
	PULARGE_INTEGER lpTotalNumberOfBytes, PULARGE_INTEGER lpTotalNumberOfFreeBytes)
{
	struct statvfs sv;
	char* path = linux_path(lpDirectoryName);
	BOOL r = (path != NULL) && (statvfs(path, &sv) == 0);
	free(path);
	if (!r) {
		SET_ERRNO_ERROR();
		return FALSE;
	}
	if (lpFreeBytesAvailableToCaller != NULL)
		lpFreeBytesAvailableToCaller->QuadPart = (ULONGLONG)sv.f_bavail * sv.f_frsize;
	if (lpTotalNumberOfBytes != NULL)
		lpTotalNumberOfBytes->QuadPart = (ULONGLONG)sv.f_blocks * sv.f_frsize;
	if (lpTotalNumberOfFreeBytes != NULL)
		lpTotalNumberOfFreeBytes->QuadPart = (ULONGLONG)sv.f_bfree * sv.f_frsize;
	return TRUE;
}

DWORD GetFullPathNameA(LPCSTR lpFileName, DWORD nBufferLength, LPSTR lpBuffer, LPSTR* lpFilePart)
{
	char* path = linux_path(lpFileName);
	char* r;
	char* slash;
	if (path == NULL)
		return 0;
	r = realpath(path, NULL);
	if (r == NULL) {
		/* Non existing file: build the absolute path manually */
		char cwd[MAX_PATH];
		if (path[0] == '/' || getcwd(cwd, sizeof(cwd)) == NULL)
			r = strdup(path);
		else if (asprintf(&r, "%s/%s", cwd, path) < 0)
			r = NULL;
	}
	free(path);
	if (r == NULL)
		return 0;
	if (strlen(r) + 1 > nBufferLength) {
		DWORD needed = (DWORD)strlen(r) + 1;
		free(r);
		return needed;
	}
	strcpy(lpBuffer, r);
	free(r);
	if (lpFilePart != NULL) {
		slash = strrchr(lpBuffer, '/');
		*lpFilePart = (slash == NULL) ? lpBuffer : slash + 1;
	}
	return (DWORD)strlen(lpBuffer);
}

DWORD GetModuleFileNameA(HMODULE hModule, LPSTR lpFilename, DWORD nSize)
{
	ssize_t r;
	(void)hModule;
	r = readlink("/proc/self/exe", lpFilename, nSize - 1);
	if (r < 0) {
		SET_ERRNO_ERROR();
		return 0;
	}
	lpFilename[r] = 0;
	return (DWORD)r;
}

char* GetAppDirectoryU(void)
{
	static char app_dir_buf[MAX_PATH];
	char* p;
	if (GetModuleFileNameA(NULL, app_dir_buf, sizeof(app_dir_buf)) == 0)
		return NULL;
	p = strrchr(app_dir_buf, '/');
	if (p != NULL)
		*p = 0;
	return app_dir_buf;
}

/* FindFirstFile: pattern matching on a single directory level */
static BOOL find_fill(HANDLE h, LPWIN32_FIND_DATAA data)
{
	struct dirent* de;
	struct stat st;
	char full[MAX_PATH];
	while ((de = readdir(h->dir)) != NULL) {
		if (fnmatch(h->pattern, de->d_name, FNM_CASEFOLD) != 0)
			continue;
		memset(data, 0, sizeof(*data));
		snprintf(full, sizeof(full), "%s/%s", h->dirname, de->d_name);
		if (lstat(full, &st) == 0) {
			int64_t size = (int64_t)st.st_size;
			data->dwFileAttributes = mode_to_attributes(full, st.st_mode);
			timespec_to_filetime(&st.st_ctim, &data->ftCreationTime);
			timespec_to_filetime(&st.st_atim, &data->ftLastAccessTime);
			timespec_to_filetime(&st.st_mtim, &data->ftLastWriteTime);
			data->nFileSizeHigh = (DWORD)((uint64_t)size >> 32);
			data->nFileSizeLow = (DWORD)size;
		}
		snprintf(data->cFileName, sizeof(data->cFileName), "%s", de->d_name);
		return TRUE;
	}
	SetLastError(ERROR_NO_MORE_FILES);
	return FALSE;
}

HANDLE FindFirstFileA(LPCSTR lpFileName, LPWIN32_FIND_DATAA lpFindFileData)
{
	char* path = linux_path(lpFileName);
	char* slash;
	HANDLE h;
	if (path == NULL)
		return INVALID_HANDLE_VALUE;
	h = new_handle(HT_FIND);
	if (h == NULL) {
		free(path);
		return INVALID_HANDLE_VALUE;
	}
	slash = strrchr(path, '/');
	if (slash == NULL) {
		h->dirname = strdup(".");
		h->pattern = strdup(path);
	} else {
		*slash = 0;
		h->dirname = strdup((path[0] == 0) ? "/" : path);
		h->pattern = strdup(slash + 1);
	}
	free(path);
	/* Windows' "*.*" matches everything */
	if (strcmp(h->pattern, "*.*") == 0) {
		free(h->pattern);
		h->pattern = strdup("*");
	}
	h->dir = opendir(h->dirname);
	if (h->dir == NULL) {
		SetLastError((errno == ENOENT) ? ERROR_PATH_NOT_FOUND : ErrnoToWin32(errno));
		free_handle(h);
		return INVALID_HANDLE_VALUE;
	}
	if (!find_fill(h, lpFindFileData)) {
		closedir(h->dir);
		free_handle(h);
		SetLastError(ERROR_FILE_NOT_FOUND);
		return INVALID_HANDLE_VALUE;
	}
	return h;
}

BOOL FindNextFileA(HANDLE hFindFile, LPWIN32_FIND_DATAA lpFindFileData)
{
	if (!is_valid(hFindFile, HT_FIND)) {
		SetLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	return find_fill(hFindFile, lpFindFileData);
}

BOOL FindClose(HANDLE hFindFile)
{
	if (!is_valid(hFindFile, HT_FIND)) {
		SetLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	if (hFindFile->dir != NULL)
		closedir(hFindFile->dir);
	free_handle(hFindFile);
	return TRUE;
}

/* ------------------------------------------------------------------------- */
/* msapi_utf8 CRT equivalents                                                */
/* ------------------------------------------------------------------------- */
FILE* fopenU(const char* filename, const char* mode)
{
	return rufus_fopen(filename, mode);
}

int _openU(const char* filename, int oflag, int pmode)
{
	char* path = linux_path(filename);
	int fd;
	if (path == NULL)
		return -1;
	fd = open(path, oflag | O_CLOEXEC, (pmode == 0) ? 0644 : (pmode | S_IRGRP | S_IROTH));
	free(path);
	return fd;
}

int _unlinkU(const char* filename)
{
	char* path = linux_path(filename);
	int r = (path == NULL) ? -1 : unlink(path);
	free(path);
	return r;
}

int _accessU(const char* filename, int mode)
{
	char* path = linux_path(filename);
	int r = (path == NULL) ? -1 : access(path, mode);
	free(path);
	return r;
}

int _chdirU(const char* dirname)
{
	char* path = linux_path(dirname);
	int r = (path == NULL) ? -1 : chdir(path);
	free(path);
	return r;
}

int _mkdirU(const char* dirname)
{
	char* path = linux_path(dirname);
	int r = (path == NULL) ? -1 : mkdir(path, 0755);
	free(path);
	return r;
}

/* Same semantics as the Windows version: create every directory in the path */
int _mkdirExU(const char* dirname)
{
	char* path = linux_path(dirname);
	size_t i, len;
	int ret = -1;
	if (path == NULL)
		return -1;
	len = strlen(path);
	while (len > 1 && path[len - 1] == '/')
		path[--len] = 0;
	for (i = 1; i <= len; i++) {
		if (path[i] == '/' || path[i] == 0) {
			char c = path[i];
			path[i] = 0;
			if (mkdir(path, 0755) < 0 && errno != EEXIST && errno != EACCES)
				goto out;
			path[i] = c;
		}
	}
	ret = 0;
out:
	free(path);
	return ret;
}

int _rmdirU(const char* dirname)
{
	char* path = linux_path(dirname);
	int r = (path == NULL) ? -1 : rmdir(path);
	free(path);
	return r;
}

char* getenvU(const char* varname)
{
	const char* v = getenv(varname);
	return (v == NULL) ? NULL : strdup(v);
}

int _stat64U(const char* filename, struct __stat64* buffer)
{
	struct stat st;
	char* path = linux_path(filename);
	int r = (path == NULL) ? -1 : stat(path, &st);
	free(path);
	if (r == 0) {
		buffer->st_size = (uint64_t)st.st_size;
		buffer->st_mtim = st.st_mtim;
		buffer->st_mode = st.st_mode;
	}
	return r;
}

uint64_t _filesizeU(const char* path)
{
	struct __stat64 st = { 0 };
	_stat64U(path, &st);
	return st.st_size;
}

static int rm_rf(const char* path)
{
	struct stat st;
	DIR* d;
	struct dirent* de;
	char* child;
	int r = 0;
	if (lstat(path, &st) != 0)
		return (errno == ENOENT) ? 0 : -1;
	if (!S_ISDIR(st.st_mode))
		return unlink(path);
	d = opendir(path);
	if (d == NULL)
		return -1;
	while ((de = readdir(d)) != NULL) {
		if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
			continue;
		if (asprintf(&child, "%s/%s", path, de->d_name) < 0) {
			r = -1;
			break;
		}
		if (rm_rf(child) != 0)
			r = -1;
		free(child);
	}
	closedir(d);
	if (rmdir(path) != 0)
		r = -1;
	return r;
}

int SHDeleteDirectoryExU(HWND hwnd, const char* pszPath, DWORD fFlags)
{
	char* path = linux_path(pszPath);
	int r;
	(void)hwnd; (void)fFlags;
	if (path == NULL || path[0] == 0 || strcmp(path, "/") == 0) {
		free(path);
		return ERROR_INVALID_PARAMETER;
	}
	r = rm_rf(path);
	free(path);
	return (r == 0) ? 0 : ErrnoToWin32(errno);
}

int SHCreateDirectoryExA(HWND hwnd, LPCSTR pszPath, const SECURITY_ATTRIBUTES* psa)
{
	(void)psa;
	return SHCreateDirectoryExU(hwnd, pszPath, NULL);
}

BOOL GetVolumePathNameA(LPCSTR lpszFileName, LPSTR lpszVolumePathName, DWORD cchBufferLength)
{
	/* Return the mount point of the file system holding the path */
	char* path = linux_path(lpszFileName);
	char* dir;
	struct stat st, parent;
	if (path == NULL)
		return FALSE;
	/* Use the existing parent if the file doesn't exist yet */
	while (stat(path, &st) != 0) {
		dir = strrchr(path, '/');
		if (dir == NULL || dir == path) {
			strcpy(path, "/");
			stat(path, &st);
			break;
		}
		*dir = 0;
	}
	for (;;) {
		char* up;
		if (strcmp(path, "/") == 0)
			break;
		dir = strdup(path);
		up = dirname(dir);
		if (stat(up, &parent) != 0 || parent.st_dev != st.st_dev) {
			free(dir);
			break;
		}
		strcpy(path, up);
		free(dir);
	}
	snprintf(lpszVolumePathName, cchBufferLength, "%s", path);
	free(path);
	return TRUE;
}

int SHCreateDirectoryExU(HWND hwnd, const char* pszPath, SECURITY_ATTRIBUTES* psa)
{
	(void)hwnd; (void)psa;
	return (_mkdirExU(pszPath) == 0) ? ERROR_SUCCESS : ErrnoToWin32(errno);
}

char* PathFindFileNameU(const char* szPath)
{
	const char* p;
	if (szPath == NULL)
		return NULL;
	for (p = szPath + strlen(szPath); p > szPath; p--) {
		if (p[-1] == '/' || p[-1] == '\\')
			break;
	}
	return (char*)p;
}

char* PathCombineU(char* lpDest, char* lpDir, char* lpFile)
{
	size_t len;
	if (lpDest == NULL)
		return NULL;
	if (lpFile != NULL && (lpFile[0] == '/' || lpFile[0] == '\\')) {
		strcpy(lpDest, lpFile);
	} else {
		strcpy(lpDest, (lpDir == NULL) ? "" : lpDir);
		len = strlen(lpDest);
		if (lpFile != NULL && lpFile[0] != 0) {
			if (len > 0 && lpDest[len - 1] != '/' && lpDest[len - 1] != '\\')
				strcat(lpDest, "/");
			strcat(lpDest, lpFile);
		}
	}
	to_unix_path_inplace(lpDest);
	return lpDest;
}

DWORD GetFinalPathNameByHandleU(HANDLE hFile, LPSTR lpszFilePath, DWORD cchFilePath, DWORD dwFlags)
{
	char proc[64];
	ssize_t r;
	(void)dwFlags;
	if (!is_valid(hFile, HT_FILE))
		return 0;
	snprintf(proc, sizeof(proc), "/proc/self/fd/%d", hFile->fd);
	r = readlink(proc, lpszFilePath, cchFilePath - 1);
	if (r < 0)
		return 0;
	lpszFilePath[r] = 0;
	return (DWORD)r;
}

BOOL GetVolumeNameForVolumeMountPointU(LPCSTR lpszVolumeMountPoint, LPSTR lpszVolumeName, DWORD cchBufferLength)
{
	/* On Linux, a volume is identified by its device number */
	struct stat st;
	char* path = linux_path(lpszVolumeMountPoint);
	BOOL r = (path != NULL) && (stat(path, &st) == 0);
	free(path);
	if (!r) {
		SET_ERRNO_ERROR();
		return FALSE;
	}
	snprintf(lpszVolumeName, cchBufferLength, "dev:%u:%u", major(st.st_dev), minor(st.st_dev));
	return TRUE;
}

BOOL GetVolumeInformationU(LPCSTR lpRootPathName, LPSTR lpVolumeNameBuffer, DWORD nVolumeNameSize,
	LPDWORD lpVolumeSerialNumber, LPDWORD lpMaximumComponentLength, LPDWORD lpFileSystemFlags,
	LPSTR lpFileSystemNameBuffer, DWORD nFileSystemNameSize)
{
	return linux_get_volume_information(lpRootPathName, lpVolumeNameBuffer, nVolumeNameSize,
		lpVolumeSerialNumber, lpMaximumComponentLength, lpFileSystemFlags,
		lpFileSystemNameBuffer, nFileSystemNameSize);
}

BOOL GetUserNameU(LPSTR lpBuffer, LPDWORD pcbBuffer)
{
	const char* name = linux_invoking_user_name();
	if (name == NULL || strlen(name) + 1 > *pcbBuffer) {
		SetLastError(ERROR_INSUFFICIENT_BUFFER);
		return FALSE;
	}
	strcpy(lpBuffer, name);
	*pcbBuffer = (DWORD)strlen(name) + 1;
	return TRUE;
}

/* ------------------------------------------------------------------------- */
/* Threads and synchronization                                               */
/* ------------------------------------------------------------------------- */
/*
 * Waits on several objects (WaitForMultipleObjects) block on a process wide condition,
 * which is broadcast whenever an object becomes signaled. NB: the lock order is always
 * multi_wait_mutex -> object mutex, and objects are signaled before the broadcast, so
 * that no wakeup can be lost.
 */
static pthread_mutex_t multi_wait_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t multi_wait_cond = PTHREAD_COND_INITIALIZER;

static void notify_multi_waiters(void)
{
	pthread_mutex_lock(&multi_wait_mutex);
	pthread_cond_broadcast(&multi_wait_cond);
	pthread_mutex_unlock(&multi_wait_mutex);
}

static __thread HANDLE self_thread = NULL;

static void thread_signal_exit(HANDLE h, DWORD code)
{
	BOOL detached;
	pthread_mutex_lock(&h->mutex);
	h->exit_code = code;
	h->signaled = TRUE;
	detached = h->detached;
	pthread_cond_broadcast(&h->cond);
	pthread_mutex_unlock(&h->mutex);
	notify_multi_waiters();
	if (detached) {
		/* Nobody holds a reference anymore */
		pthread_detach(h->thread);
		free_handle(h);
	}
}

static void thread_cleanup(void* arg)
{
	HANDLE h = (HANDLE)arg;
	if (!h->signaled)
		thread_signal_exit(h, h->exit_code);
}

static void* thread_trampoline(void* arg)
{
	HANDLE h = (HANDLE)arg;
	DWORD code;
	self_thread = h;
	pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);
	h->exit_code = STILL_ACTIVE;
	pthread_cleanup_push(thread_cleanup, h);
	code = h->start(h->param);
	h->exit_code = code;
	pthread_cleanup_pop(1);
	return NULL;
}

HANDLE CreateThread(LPSECURITY_ATTRIBUTES lpThreadAttributes, SIZE_T dwStackSize, LPTHREAD_START_ROUTINE lpStartAddress,
	LPVOID lpParameter, DWORD dwCreationFlags, LPDWORD lpThreadId)
{
	pthread_attr_t attr;
	HANDLE h;
	int r;
	(void)lpThreadAttributes; (void)dwCreationFlags;
	h = new_handle(HT_THREAD);
	if (h == NULL)
		return NULL;
	h->start = lpStartAddress;
	h->param = lpParameter;
	h->exit_code = STILL_ACTIVE;
	pthread_attr_init(&attr);
	/* Rufus threads use large stack buffers: use at least 16 MB */
	pthread_attr_setstacksize(&attr, max(dwStackSize, (SIZE_T)16 * 1024 * 1024));
	r = pthread_create(&h->thread, &attr, thread_trampoline, h);
	pthread_attr_destroy(&attr);
	if (r != 0) {
		free_handle(h);
		SetLastError(ErrnoToWin32(r));
		return NULL;
	}
	if (lpThreadId != NULL)
		*lpThreadId = (DWORD)(uintptr_t)h;
	return h;
}

static DWORD WINAPI beginthreadex_wrapper(LPVOID param)
{
	void** p = (void**)param;
	unsigned (*fn)(void*) = (unsigned (*)(void*))p[0];
	void* arg = p[1];
	free(p);
	return fn(arg);
}

uintptr_t _beginthreadex(void* security, unsigned stack_size, unsigned (*start_address)(void*),
	void* arglist, unsigned initflag, unsigned* thrdaddr)
{
	void** p = malloc(2 * sizeof(void*));
	HANDLE h;
	(void)security;
	if (p == NULL)
		return 0;
	p[0] = (void*)start_address;
	p[1] = arglist;
	h = CreateThread(NULL, stack_size, beginthreadex_wrapper, p, initflag, (LPDWORD)thrdaddr);
	if (h == NULL)
		free(p);
	return (uintptr_t)h;
}

void ExitThread(DWORD dwExitCode)
{
	if (self_thread != NULL)
		self_thread->exit_code = dwExitCode;
	pthread_exit(NULL);
}

BOOL TerminateThread(HANDLE hThread, DWORD dwExitCode)
{
	if (!is_valid(hThread, HT_THREAD))
		return FALSE;
	hThread->exit_code = dwExitCode;
	if (!hThread->signaled)
		pthread_cancel(hThread->thread);
	return TRUE;
}

BOOL GetExitCodeThread(HANDLE hThread, LPDWORD lpExitCode)
{
	if (!is_valid(hThread, HT_THREAD))
		return FALSE;
	pthread_mutex_lock(&hThread->mutex);
	*lpExitCode = hThread->signaled ? hThread->exit_code : STILL_ACTIVE;
	pthread_mutex_unlock(&hThread->mutex);
	return TRUE;
}

BOOL SetThreadPriority(HANDLE hThread, int nPriority)
{
	(void)hThread; (void)nPriority;
	return TRUE;
}

DWORD_PTR SetThreadAffinityMask(HANDLE hThread, DWORD_PTR dwThreadAffinityMask)
{
	cpu_set_t set;
	pthread_t t;
	int i;
	if (hThread == NULL || hThread == &current_thread)
		t = pthread_self();
	else if (is_valid(hThread, HT_THREAD))
		t = hThread->thread;
	else
		return 0;
	CPU_ZERO(&set);
	for (i = 0; i < (int)(8 * sizeof(DWORD_PTR)); i++)
		if (dwThreadAffinityMask & ((DWORD_PTR)1 << i))
			CPU_SET(i, &set);
	pthread_setaffinity_np(t, sizeof(set), &set);
	return 1;
}

BOOL GetProcessAffinityMask(HANDLE hProcess, PDWORD_PTR lpProcessAffinityMask, PDWORD_PTR lpSystemAffinityMask)
{
	cpu_set_t set;
	DWORD_PTR mask = 0;
	int i;
	(void)hProcess;
	if (sched_getaffinity(0, sizeof(set), &set) != 0)
		return FALSE;
	for (i = 0; i < (int)(8 * sizeof(DWORD_PTR)); i++)
		if (CPU_ISSET(i, &set))
			mask |= (DWORD_PTR)1 << i;
	if (lpProcessAffinityMask != NULL)
		*lpProcessAffinityMask = mask;
	if (lpSystemAffinityMask != NULL)
		*lpSystemAffinityMask = mask;
	return TRUE;
}

HANDLE GetCurrentThread(void)
{
	return &current_thread;
}

HANDLE GetCurrentProcess(void)
{
	return &current_process;
}

DWORD GetCurrentThreadId(void)
{
	return (DWORD)syscall(SYS_gettid);
}

DWORD GetCurrentProcessId(void)
{
	return (DWORD)getpid();
}

DWORD ResumeThread(HANDLE hThread)
{
	(void)hThread;
	return 0;
}

HANDLE CreateEventA(LPSECURITY_ATTRIBUTES lpEventAttributes, BOOL bManualReset, BOOL bInitialState, LPCSTR lpName)
{
	HANDLE h;
	(void)lpEventAttributes; (void)lpName;
	h = new_handle(HT_EVENT);
	if (h == NULL)
		return NULL;
	h->manual_reset = bManualReset;
	h->signaled = bInitialState;
	return h;
}

BOOL SetEvent(HANDLE hEvent)
{
	if (!is_valid(hEvent, HT_EVENT))
		return FALSE;
	pthread_mutex_lock(&hEvent->mutex);
	hEvent->signaled = TRUE;
	pthread_cond_broadcast(&hEvent->cond);
	pthread_mutex_unlock(&hEvent->mutex);
	notify_multi_waiters();
	return TRUE;
}

BOOL ResetEvent(HANDLE hEvent)
{
	if (!is_valid(hEvent, HT_EVENT))
		return FALSE;
	pthread_mutex_lock(&hEvent->mutex);
	hEvent->signaled = FALSE;
	pthread_mutex_unlock(&hEvent->mutex);
	return TRUE;
}

HANDLE CreateMutexA(LPSECURITY_ATTRIBUTES lpMutexAttributes, BOOL bInitialOwner, LPCSTR lpName)
{
	HANDLE h;
	(void)lpMutexAttributes; (void)lpName;
	h = new_handle(HT_MUTEX);
	if (h == NULL)
		return NULL;
	/* Mutexes are modelled as auto-reset events that start signaled when not owned */
	h->manual_reset = FALSE;
	h->signaled = !bInitialOwner;
	return h;
}

BOOL ReleaseMutex(HANDLE hMutex)
{
	if (!is_valid(hMutex, HT_MUTEX))
		return FALSE;
	pthread_mutex_lock(&hMutex->mutex);
	hMutex->signaled = TRUE;
	pthread_cond_broadcast(&hMutex->cond);
	pthread_mutex_unlock(&hMutex->mutex);
	notify_multi_waiters();
	return TRUE;
}

static void deadline_from_ms(struct timespec* ts, DWORD ms)
{
	clock_gettime(CLOCK_REALTIME, ts);
	ts->tv_sec += ms / 1000;
	ts->tv_nsec += (long)(ms % 1000) * 1000000L;
	if (ts->tv_nsec >= 1000000000L) {
		ts->tv_sec++;
		ts->tv_nsec -= 1000000000L;
	}
}

/* Try to acquire a waitable object without blocking (caller holds the lock) */
static BOOL try_acquire_locked(HANDLE h)
{
	if (!h->signaled)
		return FALSE;
	if ((h->type == HT_EVENT && !h->manual_reset) || h->type == HT_MUTEX)
		h->signaled = FALSE;
	return TRUE;
}

DWORD WaitForSingleObject(HANDLE hHandle, DWORD dwMilliseconds)
{
	struct timespec ts;
	DWORD r = WAIT_OBJECT_0;
	int rc = 0;

	if (hHandle == NULL || hHandle == INVALID_HANDLE_VALUE ||
		(hHandle->type != HT_THREAD && hHandle->type != HT_EVENT && hHandle->type != HT_MUTEX)) {
		SetLastError(ERROR_INVALID_HANDLE);
		return WAIT_FAILED;
	}
	if (dwMilliseconds != INFINITE)
		deadline_from_ms(&ts, dwMilliseconds);
	pthread_mutex_lock(&hHandle->mutex);
	while (!try_acquire_locked(hHandle)) {
		if (dwMilliseconds == 0) {
			r = WAIT_TIMEOUT;
			break;
		}
		if (dwMilliseconds == INFINITE)
			rc = pthread_cond_wait(&hHandle->cond, &hHandle->mutex);
		else
			rc = pthread_cond_timedwait(&hHandle->cond, &hHandle->mutex, &ts);
		if (rc == ETIMEDOUT) {
			r = try_acquire_locked(hHandle) ? WAIT_OBJECT_0 : WAIT_TIMEOUT;
			break;
		}
	}
	pthread_mutex_unlock(&hHandle->mutex);
	return r;
}

static int compare_handles(const void* a, const void* b)
{
	uintptr_t x = (uintptr_t)*(const HANDLE*)a, y = (uintptr_t)*(const HANDLE*)b;
	return (x > y) - (x < y);
}

/*
 * Like Windows, a wait for all the objects is atomic: either all the objects are
 * signaled, and they are all acquired (auto-reset events are reset), or none of
 * them is acquired. The objects are locked in address order to avoid deadlocks.
 */
static BOOL try_acquire_all(DWORD nCount, const HANDLE* lpHandles)
{
	HANDLE sorted[MAXIMUM_WAIT_OBJECTS];
	DWORD i;
	BOOL all = TRUE;

	memcpy(sorted, lpHandles, nCount * sizeof(HANDLE));
	qsort(sorted, nCount, sizeof(HANDLE), compare_handles);
	for (i = 0; i < nCount; i++)
		if (i == 0 || sorted[i] != sorted[i - 1])
			pthread_mutex_lock(&sorted[i]->mutex);
	for (i = 0; i < nCount && all; i++)
		all = sorted[i]->signaled;
	if (all) {
		for (i = 0; i < nCount; i++)
			if (i == 0 || sorted[i] != sorted[i - 1])
				try_acquire_locked(sorted[i]);
	}
	for (i = nCount; i-- > 0; )
		if (i == 0 || sorted[i] != sorted[i - 1])
			pthread_mutex_unlock(&sorted[i]->mutex);
	return all;
}

/* Try to acquire all (bWaitAll) or any of the objects. Returns the WAIT_ code or WAIT_TIMEOUT */
static DWORD try_acquire_multiple(DWORD nCount, const HANDLE* lpHandles, BOOL bWaitAll)
{
	DWORD i;
	if (bWaitAll)
		return try_acquire_all(nCount, lpHandles) ? WAIT_OBJECT_0 : WAIT_TIMEOUT;
	for (i = 0; i < nCount; i++) {
		if (WaitForSingleObject(lpHandles[i], 0) == WAIT_OBJECT_0)
			return WAIT_OBJECT_0 + i;
	}
	return WAIT_TIMEOUT;
}

DWORD WaitForMultipleObjects(DWORD nCount, const HANDLE* lpHandles, BOOL bWaitAll, DWORD dwMilliseconds)
{
	struct timespec ts;
	DWORD i, r;
	int rc;

	if (nCount == 0 || nCount > MAXIMUM_WAIT_OBJECTS || lpHandles == NULL) {
		SetLastError(ERROR_INVALID_PARAMETER);
		return WAIT_FAILED;
	}
	for (i = 0; i < nCount; i++) {
		if (lpHandles[i] == NULL || lpHandles[i] == INVALID_HANDLE_VALUE || (lpHandles[i]->type != HT_THREAD &&
			lpHandles[i]->type != HT_EVENT && lpHandles[i]->type != HT_MUTEX)) {
			SetLastError(ERROR_INVALID_HANDLE);
			return WAIT_FAILED;
		}
	}
	if (dwMilliseconds != INFINITE)
		deadline_from_ms(&ts, dwMilliseconds);
	pthread_mutex_lock(&multi_wait_mutex);
	for (;;) {
		r = try_acquire_multiple(nCount, lpHandles, bWaitAll);
		if (r != WAIT_TIMEOUT || dwMilliseconds == 0)
			break;
		if (dwMilliseconds == INFINITE)
			rc = pthread_cond_wait(&multi_wait_cond, &multi_wait_mutex);
		else
			rc = pthread_cond_timedwait(&multi_wait_cond, &multi_wait_mutex, &ts);
		if (rc == ETIMEDOUT) {
			r = try_acquire_multiple(nCount, lpHandles, bWaitAll);
			break;
		}
	}
	pthread_mutex_unlock(&multi_wait_mutex);
	return r;
}

void Sleep(DWORD dwMilliseconds)
{
	struct timespec ts = { dwMilliseconds / 1000, (long)(dwMilliseconds % 1000) * 1000000L };
	while (nanosleep(&ts, &ts) != 0 && errno == EINTR);
}

ULONGLONG GetTickCount64(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (ULONGLONG)ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000L;
}

DWORD GetTickCount(void)
{
	return (DWORD)GetTickCount64();
}

BOOL QueryPerformanceCounter(LARGE_INTEGER* lpPerformanceCount)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	lpPerformanceCount->QuadPart = (LONGLONG)ts.tv_sec * 1000000000LL + ts.tv_nsec;
	return TRUE;
}

BOOL QueryPerformanceFrequency(LARGE_INTEGER* lpFrequency)
{
	lpFrequency->QuadPart = 1000000000LL;
	return TRUE;
}

void InitializeCriticalSection(LPCRITICAL_SECTION cs)
{
	pthread_mutexattr_t attr;
	pthread_mutex_t* m = malloc(sizeof(pthread_mutex_t));
	pthread_mutexattr_init(&attr);
	pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
	pthread_mutex_init(m, &attr);
	pthread_mutexattr_destroy(&attr);
	cs->opaque = m;
}

void DeleteCriticalSection(LPCRITICAL_SECTION cs)
{
	if (cs->opaque != NULL) {
		pthread_mutex_destroy((pthread_mutex_t*)cs->opaque);
		free(cs->opaque);
		cs->opaque = NULL;
	}
}

void EnterCriticalSection(LPCRITICAL_SECTION cs)
{
	pthread_mutex_lock((pthread_mutex_t*)cs->opaque);
}

void LeaveCriticalSection(LPCRITICAL_SECTION cs)
{
	pthread_mutex_unlock((pthread_mutex_t*)cs->opaque);
}

/* ------------------------------------------------------------------------- */
/* Time                                                                      */
/* ------------------------------------------------------------------------- */
static void tm_to_systemtime(const struct tm* tm, long nsec, LPSYSTEMTIME st)
{
	st->wYear = (WORD)(tm->tm_year + 1900);
	st->wMonth = (WORD)(tm->tm_mon + 1);
	st->wDayOfWeek = (WORD)tm->tm_wday;
	st->wDay = (WORD)tm->tm_mday;
	st->wHour = (WORD)tm->tm_hour;
	st->wMinute = (WORD)tm->tm_min;
	st->wSecond = (WORD)tm->tm_sec;
	st->wMilliseconds = (WORD)(nsec / 1000000L);
}

void GetSystemTime(LPSYSTEMTIME lpSystemTime)
{
	struct timespec ts;
	struct tm tm;
	clock_gettime(CLOCK_REALTIME, &ts);
	gmtime_r(&ts.tv_sec, &tm);
	tm_to_systemtime(&tm, ts.tv_nsec, lpSystemTime);
}

void GetLocalTime(LPSYSTEMTIME lpSystemTime)
{
	struct timespec ts;
	struct tm tm;
	clock_gettime(CLOCK_REALTIME, &ts);
	localtime_r(&ts.tv_sec, &tm);
	tm_to_systemtime(&tm, ts.tv_nsec, lpSystemTime);
}

void GetSystemTimeAsFileTime(LPFILETIME lpSystemTimeAsFileTime)
{
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	timespec_to_filetime(&ts, lpSystemTimeAsFileTime);
}

BOOL SystemTimeToFileTime(const SYSTEMTIME* st, LPFILETIME ft)
{
	struct tm tm = { 0 };
	struct timespec ts;
	tm.tm_year = st->wYear - 1900;
	tm.tm_mon = st->wMonth - 1;
	tm.tm_mday = st->wDay;
	tm.tm_hour = st->wHour;
	tm.tm_min = st->wMinute;
	tm.tm_sec = st->wSecond;
	ts.tv_sec = timegm(&tm);
	ts.tv_nsec = (long)st->wMilliseconds * 1000000L;
	timespec_to_filetime(&ts, ft);
	return TRUE;
}

BOOL FileTimeToSystemTime(const FILETIME* ft, LPSYSTEMTIME st)
{
	struct timespec ts;
	struct tm tm;
	filetime_to_timespec(ft, &ts);
	gmtime_r(&ts.tv_sec, &tm);
	tm_to_systemtime(&tm, ts.tv_nsec, st);
	return TRUE;
}

BOOL LocalFileTimeToFileTime(const FILETIME* lpLocalFileTime, LPFILETIME lpFileTime)
{
	struct timespec ts;
	struct tm tm;
	filetime_to_timespec(lpLocalFileTime, &ts);
	localtime_r(&ts.tv_sec, &tm);
	ts.tv_sec -= tm.tm_gmtoff;
	timespec_to_filetime(&ts, lpFileTime);
	return TRUE;
}

/* ------------------------------------------------------------------------- */
/* Strings and locale                                                        */
/* ------------------------------------------------------------------------- */
/* Decode one UTF-8 sequence. Returns the number of bytes consumed or -1 */
static int utf8_decode(const unsigned char* s, size_t len, uint32_t* cp)
{
	if (len == 0)
		return -1;
	if (s[0] < 0x80) {
		*cp = s[0];
		return 1;
	}
	if ((s[0] & 0xE0) == 0xC0 && len >= 2 && (s[1] & 0xC0) == 0x80) {
		*cp = ((uint32_t)(s[0] & 0x1F) << 6) | (s[1] & 0x3F);
		return (*cp >= 0x80) ? 2 : -1;
	}
	if ((s[0] & 0xF0) == 0xE0 && len >= 3 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
		*cp = ((uint32_t)(s[0] & 0x0F) << 12) | ((uint32_t)(s[1] & 0x3F) << 6) | (s[2] & 0x3F);
		return (*cp >= 0x800) ? 3 : -1;
	}
	if ((s[0] & 0xF8) == 0xF0 && len >= 4 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80 && (s[3] & 0xC0) == 0x80) {
		*cp = ((uint32_t)(s[0] & 0x07) << 18) | ((uint32_t)(s[1] & 0x3F) << 12) | ((uint32_t)(s[2] & 0x3F) << 6) | (s[3] & 0x3F);
		return (*cp >= 0x10000 && *cp <= 0x10FFFF) ? 4 : -1;
	}
	return -1;
}

static int utf8_encode(uint32_t cp, char* out)
{
	if (cp < 0x80) {
		out[0] = (char)cp;
		return 1;
	}
	if (cp < 0x800) {
		out[0] = (char)(0xC0 | (cp >> 6));
		out[1] = (char)(0x80 | (cp & 0x3F));
		return 2;
	}
	if (cp < 0x10000) {
		out[0] = (char)(0xE0 | (cp >> 12));
		out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
		out[2] = (char)(0x80 | (cp & 0x3F));
		return 3;
	}
	out[0] = (char)(0xF0 | (cp >> 18));
	out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
	out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
	out[3] = (char)(0x80 | (cp & 0x3F));
	return 4;
}

int MultiByteToWideChar(UINT CodePage, DWORD dwFlags, LPCSTR src, int cbMultiByte, LPWSTR dst, int cchWideChar)
{
	size_t len, i = 0;
	int n = 0, r;
	uint32_t cp;
	(void)CodePage;
	if (src == NULL) {
		SetLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}
	len = (cbMultiByte < 0) ? strlen(src) + 1 : (size_t)cbMultiByte;
	while (i < len) {
		r = utf8_decode((const unsigned char*)&src[i], len - i, &cp);
		if (r < 0) {
			if (dwFlags & MB_ERR_INVALID_CHARS) {
				SetLastError(ERROR_NO_UNICODE_TRANSLATION_COMPAT);
				return 0;
			}
			/* Latin-1 fallback, like the ACP conversion would do */
			cp = (unsigned char)src[i];
			r = 1;
		}
		if (cchWideChar > 0) {
			if (n >= cchWideChar) {
				SetLastError(ERROR_INSUFFICIENT_BUFFER);
				return 0;
			}
			dst[n] = (wchar_t)cp;
		}
		n++;
		i += r;
	}
	return n;
}

int WideCharToMultiByte(UINT CodePage, DWORD dwFlags, LPCWSTR src, int cchWideChar, LPSTR dst,
	int cbMultiByte, LPCSTR lpDefaultChar, LPBOOL lpUsedDefaultChar)
{
	size_t len, i;
	int n = 0, r;
	char tmp[4];
	(void)CodePage; (void)dwFlags; (void)lpDefaultChar;
	if (lpUsedDefaultChar != NULL)
		*lpUsedDefaultChar = FALSE;
	if (src == NULL) {
		SetLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}
	len = (cchWideChar < 0) ? wcslen(src) + 1 : (size_t)cchWideChar;
	for (i = 0; i < len; i++) {
		r = utf8_encode((uint32_t)src[i], tmp);
		if (cbMultiByte > 0) {
			if (n + r > cbMultiByte) {
				SetLastError(ERROR_INSUFFICIENT_BUFFER);
				return 0;
			}
			memcpy(&dst[n], tmp, r);
		}
		n += r;
	}
	return n;
}

char* wchar_to_utf8(const wchar_t* wstr)
{
	int size;
	char* str;
	if (wstr == NULL)
		return NULL;
	size = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, NULL, 0, NULL, NULL);
	str = calloc(size + 1, 1);
	if (str != NULL)
		WideCharToMultiByte(CP_UTF8, 0, wstr, -1, str, size, NULL, NULL);
	return str;
}

wchar_t* utf8_to_wchar(const char* str)
{
	int size;
	wchar_t* wstr;
	if (str == NULL)
		return NULL;
	size = MultiByteToWideChar(CP_UTF8, 0, str, -1, NULL, 0);
	wstr = calloc(size + 1, sizeof(wchar_t));
	if (wstr != NULL)
		MultiByteToWideChar(CP_UTF8, 0, str, -1, wstr, size);
	return wstr;
}

LPSTR CharLowerA(LPSTR lpsz)
{
	return _strlwr(lpsz);
}

LPSTR CharUpperA(LPSTR lpsz)
{
	return _strupr(lpsz);
}

LPSTR CharNextA(LPCSTR lpsz)
{
	uint32_t cp;
	int r;
	if (*lpsz == 0)
		return (LPSTR)lpsz;
	r = utf8_decode((const unsigned char*)lpsz, strlen(lpsz), &cp);
	return (LPSTR)lpsz + ((r < 0) ? 1 : r);
}

PCSTR StrStrIA(PCSTR pszFirst, PCSTR pszSrch)
{
	if (pszFirst == NULL || pszSrch == NULL)
		return NULL;
	return strcasestr(pszFirst, pszSrch);
}

PCSTR StrRStrIA(PCSTR pszSource, PCSTR pszLast, PCSTR pszSrch)
{
	PCSTR p, found = NULL;
	if (pszSource == NULL || pszSrch == NULL)
		return NULL;
	for (p = pszSource; (p = strcasestr(p, pszSrch)) != NULL; p++) {
		if (pszLast != NULL && p >= pszLast)
			break;
		found = p;
	}
	return found;
}

BOOL PathMatchSpecA(LPCSTR pszFile, LPCSTR pszSpec)
{
	/* The spec can contain multiple patterns, separated by ';' */
	char* spec = strdup(pszSpec);
	char *tok, *save = NULL;
	BOOL r = FALSE;
	if (spec == NULL)
		return FALSE;
	for (tok = strtok_r(spec, ";", &save); tok != NULL; tok = strtok_r(NULL, ";", &save)) {
		while (*tok == ' ')
			tok++;
		if (fnmatch((strcmp(tok, "*.*") == 0) ? "*" : tok, pszFile, FNM_CASEFOLD) == 0) {
			r = TRUE;
			break;
		}
	}
	free(spec);
	return r;
}

/* Map the POSIX locale (e.g. "tr_TR.UTF-8") to a Windows LANGID */
LANGID GetUserDefaultUILanguage(void)
{
	return (LANGID)linux_locale_to_lcid(NULL);
}

LANGID GetSystemDefaultUILanguage(void)
{
	return GetUserDefaultUILanguage();
}

LANGID GetUserDefaultLangID(void)
{
	return GetUserDefaultUILanguage();
}

LCID GetUserDefaultLCID(void)
{
	return (LCID)GetUserDefaultUILanguage();
}

LCID GetSystemDefaultLCID(void)
{
	return (LCID)GetUserDefaultUILanguage();
}

int GetLocaleInfoA(LCID Locale, LCTYPE LCType, LPSTR lpLCData, int cchData)
{
	(void)Locale; (void)LCType;
	if (cchData > 0 && lpLCData != NULL)
		lpLCData[0] = 0;
	return 0;
}

int MulDiv(int nNumber, int nNumerator, int nDenominator)
{
	if (nDenominator == 0)
		return -1;
	return (int)(((int64_t)nNumber * nNumerator + nDenominator / 2) / nDenominator);
}

int lstrlenA(LPCSTR lpString)
{
	return (lpString == NULL) ? 0 : (int)strlen(lpString);
}

/* ------------------------------------------------------------------------- */
/* Environment and modules                                                   */
/* ------------------------------------------------------------------------- */
DWORD GetEnvironmentVariableA(LPCSTR lpName, LPSTR lpBuffer, DWORD nSize)
{
	const char* v = getenv(lpName);
	size_t len;
	if (v == NULL) {
		SetLastError(ERROR_ENVVAR_NOT_FOUND);
		return 0;
	}
	len = strlen(v);
	if (len + 1 > nSize)
		return (DWORD)len + 1;
	strcpy(lpBuffer, v);
	return (DWORD)len;
}

BOOL SetEnvironmentVariableA(LPCSTR lpName, LPCSTR lpValue)
{
	return ((lpValue == NULL) ? unsetenv(lpName) : setenv(lpName, lpValue, 1)) == 0;
}

DWORD ExpandEnvironmentStringsA(LPCSTR lpSrc, LPSTR lpDst, DWORD nSize)
{
	/* Expand %VAR% references */
	char out[MAX_PATH * 2];
	size_t o = 0;
	const char *p = lpSrc, *end;
	while (*p != 0 && o < sizeof(out) - 1) {
		if (*p == '%' && (end = strchr(p + 1, '%')) != NULL && end > p + 1) {
			char name[256];
			const char* v;
			size_t n = min((size_t)(end - p - 1), sizeof(name) - 1);
			memcpy(name, p + 1, n);
			name[n] = 0;
			v = getenv(name);
			if (v != NULL) {
				size_t vl = strlen(v);
				if (o + vl >= sizeof(out) - 1)
					break;
				memcpy(&out[o], v, vl);
				o += vl;
				p = end + 1;
				continue;
			}
		}
		out[o++] = *p++;
	}
	out[o] = 0;
	if (o + 1 > nSize)
		return (DWORD)o + 1;
	strcpy(lpDst, out);
	return (DWORD)o + 1;
}

UINT GetSystemDirectoryA(LPSTR lpBuffer, UINT uSize)
{
	return (UINT)snprintf(lpBuffer, uSize, "/usr/bin");
}

UINT GetWindowsDirectoryA(LPSTR lpBuffer, UINT uSize)
{
	return (UINT)snprintf(lpBuffer, uSize, "/usr");
}

HMODULE GetModuleHandleA(LPCSTR lpModuleName)
{
	(void)lpModuleName;
	return NULL;
}

HMODULE GetModuleHandleW(LPCWSTR lpModuleName)
{
	(void)lpModuleName;
	return NULL;
}

HMODULE LoadLibraryA(LPCSTR lpLibFileName)
{
	(void)lpLibFileName;
	SetLastError(ERROR_MOD_NOT_FOUND);
	return NULL;
}

HMODULE LoadLibraryExW(LPCWSTR lpLibFileName, HANDLE hFile, DWORD dwFlags)
{
	(void)lpLibFileName; (void)hFile; (void)dwFlags;
	SetLastError(ERROR_MOD_NOT_FOUND);
	return NULL;
}

FARPROC GetProcAddress(HMODULE hModule, LPCSTR lpProcName)
{
	(void)hModule; (void)lpProcName;
	SetLastError(ERROR_PROC_NOT_FOUND);
	return NULL;
}

BOOL FreeLibrary(HMODULE hLibModule)
{
	(void)hLibModule;
	return TRUE;
}

void OutputDebugStringA(LPCSTR lpOutputString)
{
	if (getenv("RUFUS_DEBUG") != NULL)
		fputs(lpOutputString, stderr);
}

BOOL IsDebuggerPresent(void)
{
	return FALSE;
}

/* ------------------------------------------------------------------------- */
/* Memory                                                                    */
/* ------------------------------------------------------------------------- */
HGLOBAL GlobalAlloc(UINT uFlags, SIZE_T dwBytes)
{
	return (uFlags & GMEM_ZEROINIT) ? calloc(1, dwBytes) : malloc(dwBytes);
}

HGLOBAL GlobalFree(HGLOBAL hMem)
{
	free(hMem);
	return NULL;
}

LPVOID GlobalLock(HGLOBAL hMem)
{
	return hMem;
}

BOOL GlobalUnlock(HGLOBAL hMem)
{
	(void)hMem;
	return TRUE;
}

/* ------------------------------------------------------------------------- */
/* GUIDs                                                                     */
/* ------------------------------------------------------------------------- */
HRESULT CoCreateGuid(GUID* pguid)
{
	int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
	ssize_t r = -1;
	if (fd >= 0) {
		r = read(fd, pguid, sizeof(GUID));
		close(fd);
	}
	if (r != sizeof(GUID))
		return E_FAIL;
	/* RFC 4122 version 4 */
	pguid->Data3 = (pguid->Data3 & 0x0FFF) | 0x4000;
	pguid->Data4[0] = (pguid->Data4[0] & 0x3F) | 0x80;
	return S_OK;
}

int StringFromGUID2(REFGUID g, LPWSTR lpsz, int cchMax)
{
	return swprintf(lpsz, cchMax, L"{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
		g->Data1, g->Data2, g->Data3, g->Data4[0], g->Data4[1], g->Data4[2],
		g->Data4[3], g->Data4[4], g->Data4[5], g->Data4[6], g->Data4[7]) + 1;
}

HRESULT CLSIDFromString(LPCWSTR lpsz, LPCLSID pclsid)
{
	unsigned int d[11];
	int i;
	if (swscanf(lpsz, L"{%8x-%4x-%4x-%2x%2x-%2x%2x%2x%2x%2x%2x}", &d[0], &d[1], &d[2],
		&d[3], &d[4], &d[5], &d[6], &d[7], &d[8], &d[9], &d[10]) != 11)
		return E_INVALIDARG;
	pclsid->Data1 = d[0];
	pclsid->Data2 = (uint16_t)d[1];
	pclsid->Data3 = (uint16_t)d[2];
	for (i = 0; i < 8; i++)
		pclsid->Data4[i] = (uint8_t)d[3 + i];
	return S_OK;
}
