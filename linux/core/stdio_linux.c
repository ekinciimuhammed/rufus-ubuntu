/*
 * Rufus for Linux: I/O and logging helpers (Linux versions of the parts of
 * src/stdio.c that depend on Windows)
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
#include <windows.h>
#include <msapi_utf8_linux.h>
#include <pthread.h>
#include <sys/stat.h>
#include <linux/falloc.h>

#include "rufus.h"
#include "missing.h"
#include "linux_drive.h"

/*
 * Log output: everything goes to stderr when RUFUS_DEBUG is set, and to the
 * log sink installed by the user interface (which displays it in the log dialog).
 */
static void (*log_sink)(const char* text) = NULL;
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;
static BOOL log_to_stderr = -1;

void linux_set_log_sink(void (*sink)(const char* text))
{
	pthread_mutex_lock(&log_lock);
	log_sink = sink;
	pthread_mutex_unlock(&log_lock);
}

static void log_output(const char* text)
{
	pthread_mutex_lock(&log_lock);
	if (log_to_stderr < 0)
		log_to_stderr = (getenv("RUFUS_DEBUG") != NULL);
	if (log_to_stderr)
		fputs(text, stderr);
	if (log_sink != NULL)
		log_sink(text);
	pthread_mutex_unlock(&log_lock);
}

void uprintf(const char* format, ...)
{
	char buf[4096];
	char* p = buf;
	va_list args;
	int n;

	va_start(args, format);
	n = vsnprintf(p, sizeof(buf) - 2, format, args);
	va_end(args);
	if (n < 0)
		return;
	p += (n >= (int)sizeof(buf) - 2) ? (int)sizeof(buf) - 3 : n;
	while ((p > buf) && isspaceU(p[-1]))
		*--p = '\0';
	*p++ = '\n';
	*p = '\0';
	/* Windows line endings are used throughout Rufus' messages */
	for (p = buf; *p != 0; p++) {
		if (p[0] == '\r' && p[1] == '\n')
			memmove(p, p + 1, strlen(p));
	}
	log_output(buf);
}

void uprintfs(const char* str)
{
	char* copy;
	char* p;
	if (str == NULL)
		return;
	copy = strdup(str);
	if (copy == NULL)
		return;
	for (p = copy; *p != 0; p++) {
		if (p[0] == '\r' && p[1] == '\n')
			memmove(p, p + 1, strlen(p));
	}
	log_output(copy);
	free(copy);
}

void wuprintf(const wchar_t* format, ...)
{
	wchar_t wbuf[4096];
	char* buf;
	va_list args;
	int n;

	va_start(args, format);
	n = vswprintf(wbuf, ARRAYSIZE(wbuf) - 1, format, args);
	va_end(args);
	if (n < 0)
		return;
	buf = wchar_to_utf8(wbuf);
	if (buf != NULL)
		uprintfs(buf);
	free(buf);
}

/* English descriptions, as returned by FormatMessage() for the codes we produce */
static const struct { DWORD code; const char* msg; } win32_messages[] = {
	{ ERROR_SUCCESS, "The operation completed successfully." },
	{ ERROR_INVALID_FUNCTION, "Incorrect function." },
	{ ERROR_FILE_NOT_FOUND, "The system cannot find the file specified." },
	{ ERROR_PATH_NOT_FOUND, "The system cannot find the path specified." },
	{ ERROR_TOO_MANY_OPEN_FILES, "The system cannot open the file." },
	{ ERROR_ACCESS_DENIED, "Access is denied." },
	{ ERROR_INVALID_HANDLE, "The handle is invalid." },
	{ ERROR_NOT_ENOUGH_MEMORY, "Not enough memory resources are available to process this command." },
	{ ERROR_INVALID_DATA, "The data is invalid." },
	{ ERROR_OUTOFMEMORY, "Not enough memory resources are available to complete this operation." },
	{ ERROR_WRITE_PROTECT, "The media is write protected." },
	{ ERROR_NOT_READY, "The device is not ready." },
	{ ERROR_CRC, "Data error (cyclic redundancy check)." },
	{ ERROR_SEEK, "The drive cannot locate a specific area or track on the disk." },
	{ ERROR_WRITE_FAULT, "The system cannot write to the specified device." },
	{ ERROR_READ_FAULT, "The system cannot read from the specified device." },
	{ ERROR_GEN_FAILURE, "A device attached to the system is not functioning." },
	{ ERROR_SHARING_VIOLATION, "The process cannot access the file because it is being used by another process." },
	{ ERROR_HANDLE_EOF, "Reached the end of the file." },
	{ ERROR_NOT_SUPPORTED, "The request is not supported." },
	{ ERROR_DEV_NOT_EXIST, "The specified network resource or device is no longer available." },
	{ ERROR_FILE_EXISTS, "The file exists." },
	{ ERROR_INVALID_PARAMETER, "The parameter is incorrect." },
	{ ERROR_OPEN_FAILED, "The system cannot open the device or file specified." },
	{ ERROR_DISK_FULL, "There is not enough space on the disk." },
	{ ERROR_INSUFFICIENT_BUFFER, "The data area passed to a system call is too small." },
	{ ERROR_INVALID_NAME, "The filename, directory name, or volume label syntax is incorrect." },
	{ ERROR_MOD_NOT_FOUND, "The specified module could not be found." },
	{ ERROR_PROC_NOT_FOUND, "The specified procedure could not be found." },
	{ ERROR_NEGATIVE_SEEK, "An attempt was made to move the file pointer before the beginning of the file." },
	{ ERROR_DIR_NOT_EMPTY, "The directory is not empty." },
	{ ERROR_BUSY, "The requested resource is in use." },
	{ ERROR_ALREADY_EXISTS, "Cannot create a file when that file already exists." },
	{ ERROR_FILENAME_EXCED_RANGE, "The filename or extension is too long." },
	{ ERROR_FILE_TOO_LARGE, "The file size exceeds the limit allowed and cannot be saved." },
	{ ERROR_BROKEN_PIPE, "The pipe has been ended." },
	{ ERROR_NO_MORE_FILES, "There are no more files." },
	{ ERROR_NO_MORE_ITEMS, "No more data is available." },
	{ WAIT_TIMEOUT, "The wait operation timed out." },
	{ ERROR_OPERATION_ABORTED, "The I/O operation has been aborted because of either a thread exit or an application request." },
	{ ERROR_IO_DEVICE, "The request could not be performed because of an I/O device error." },
	{ ERROR_NO_MEDIA_IN_DRIVE, "No media in drive." },
	{ ERROR_UNRECOGNIZED_VOLUME, "The volume does not contain a recognized file system." },
	{ ERROR_NOT_FOUND, "Element not found." },
	{ ERROR_CANCELLED, "The operation was canceled by the user." },
	{ ERROR_TIMEOUT, "This operation returned because the timeout period expired." },
	{ ERROR_UNRECOGNIZED_MEDIA, "The disk media is not recognized. It may not be formatted." },
	{ ERROR_PARTITION_FAILURE, "Tried to reformat the disk, but the partition table could not be written." },
	{ ERROR_NOT_ENOUGH_QUOTA, "Not enough quota is available to process this command." },
	{ ERROR_CANT_RESOLVE_FILENAME, "The name of the file cannot be resolved by the system." },
	{ ERROR_DEVICE_IN_USE, "The device is in use by an active process and cannot be disconnected." },
	{ ERROR_NO_VOLUME_LABEL, "The disk has no volume label." },
	{ ERROR_LABEL_TOO_LONG, "The volume label you entered exceeds the label character limit of the target file system." },
	{ ERROR_INTERNET_TIMEOUT, "The operation timed out." },
	{ ERROR_INTERNET_NAME_NOT_RESOLVED, "The server name or address could not be resolved." },
	{ ERROR_INTERNET_CANNOT_CONNECT, "The attempt to connect to the server failed." },
	{ ERROR_INTERNET_SEC_CERT_ERRORS, "The SSL certificate contains errors." },
	{ ERROR_HTTP_INVALID_SERVER_RESPONSE, "The server response could not be parsed." },
};

const char* WindowsErrorString(void)
{
	static __thread char err_string[256];
	DWORD error_code = GetLastError(), code;
	size_t i;

	code = SCODE_CODE(error_code);
	for (i = 0; i < ARRAYSIZE(win32_messages); i++) {
		if (win32_messages[i].code == code) {
			snprintf(err_string, sizeof(err_string), "[0x%08X] %s", error_code, win32_messages[i].msg);
			goto out;
		}
	}
	snprintf(err_string, sizeof(err_string), "[0x%08X] Unknown error 0x%08X", error_code, error_code);
out:
	SetLastError(error_code);
	return err_string;
}

/* Same as CreateFile(), but with a timeout on blocking devices */
HANDLE CreateFileWithTimeout(LPCSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode,
	LPSECURITY_ATTRIBUTES lpSecurityAttributes, DWORD dwCreationDisposition, DWORD dwFlagsAndAttributes,
	HANDLE hTemplateFile, DWORD dwTimeOut)
{
	(void)dwTimeOut;
	/* Linux opens of block devices don't block the way Windows ones can */
	return CreateFileA(lpFileName, dwDesiredAccess, dwShareMode, lpSecurityAttributes,
		dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);
}

DWORD WaitForSingleObjectWithMessages(HANDLE hHandle, DWORD dwMilliseconds)
{
	/* Our UI doesn't run its message loop on threads that wait */
	return WaitForSingleObject(hHandle, dwMilliseconds);
}

/*
 * Create a file and preallocate its space. On Windows, this is done to have the
 * AMI NTFS driver find contiguous files. On Linux we use fallocate() with
 * FALLOC_FL_KEEP_SIZE, which avoids vfat zero filling the whole file.
 */
HANDLE CreatePreallocatedFile(const char* lpFileName, DWORD dwDesiredAccess,
	DWORD dwShareMode, LPSECURITY_ATTRIBUTES lpSecurityAttributes, DWORD dwCreationDisposition,
	DWORD dwFlagsAndAttributes, LONGLONG llFileSize)
{
	HANDLE hFile;
	int fd;

	hFile = CreateFileA(lpFileName, dwDesiredAccess, dwShareMode, lpSecurityAttributes,
		dwCreationDisposition, dwFlagsAndAttributes, NULL);
	if (hFile == INVALID_HANDLE_VALUE || llFileSize <= 0)
		return hFile;
#if defined(VALIDATE_SAME_VOLUME)
	{
		/* Make sure the file was created on our target drive (see GHSA-w4w7-p3x2-3p83) */
		extern char target_volume[64];
		char vol[64];
		if (target_volume[0] != 0 && (!GetVolumeNameForVolumeMountPointU(lpFileName, vol, sizeof(vol)) ||
			strcmp(vol, target_volume) != 0)) {
			uprintf("FATAL: '%s' does not reside on the target volume!", lpFileName);
			CloseHandle(hFile);
			DeleteFileU(lpFileName);
			SetLastError(ERROR_ACCESS_DENIED);
			return INVALID_HANDLE_VALUE;
		}
	}
#endif
	fd = HandleToFd(hFile);
	if (fallocate(fd, FALLOC_FL_KEEP_SIZE, 0, (off_t)llFileSize) != 0 && errno != EOPNOTSUPP && errno != ENOSYS) {
		if (errno == ENOSPC) {
			SetLastError(ERROR_DISK_FULL);
			uprintf("Could not preallocate '%s': %s", lpFileName, WindowsErrorString());
			CloseHandle(hFile);
			DeleteFileU(lpFileName);
			return INVALID_HANDLE_VALUE;
		}
	}
	return hFile;
}

uint32_t ResolveDllAddress(dll_resolver_t* resolver)
{
	/* Only used on Windows, to locate unexported Windows DLL functions */
	(void)resolver;
	return 0;
}
