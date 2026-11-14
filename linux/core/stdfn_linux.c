/*
 * Rufus for Linux: standard functions (Linux versions of the parts of
 * src/stdfn.c that depend on Windows)
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
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/utsname.h>
#include <sys/wait.h>

#include "rufus.h"
#include "cregex.h"
#include "missing.h"
#include "resource.h"
#include "settings.h"
#include "localization.h"
#include "linux_drive.h"
#include "compat.h"
#include "resources.h"

extern char** environ;
extern windows_version_t WindowsVersion;

/* ------------------------------------------------------------------------- */
/* Resources                                                                 */
/* ------------------------------------------------------------------------- */
/*
 * On Windows, Rufus' data files are embedded in the executable. On Linux they
 * are installed in RUFUS_DATADIR (or found in the source tree when running
 * from the build directory), and loaded once, on first use.
 */
typedef struct {
	int id;
	uint8_t* data;
	DWORD size;
} loaded_resource;

static loaded_resource loaded[ARRAYSIZE(rufus_resources)];
static pthread_mutex_t res_lock = PTHREAD_MUTEX_INITIALIZER;

/* Find the directory holding our data files */
const char* linux_get_data_dir(void)
{
	static char data_dir[MAX_PATH] = { 0 };
	char exe_dir[MAX_PATH], candidate[MAX_PATH + 64], *p;
	struct stat st;
	const char* env;

	if (data_dir[0] != 0)
		return data_dir;
	env = getenv("RUFUS_DATADIR");
	if (env != NULL && env[0] != 0) {
		static_strcpy(data_dir, env);
		return data_dir;
	}
	/* Running from the build directory: <top>/linux/build/rufus -> <top>/res */
	if (GetModuleFileNameA(NULL, exe_dir, sizeof(exe_dir)) != 0) {
		p = strrchr(exe_dir, '/');
		if (p != NULL)
			*p = 0;
		snprintf(candidate, sizeof(candidate), "%s/../../res/loc/rufus.loc", exe_dir);
		if (stat(candidate, &st) == 0) {
			snprintf(candidate, sizeof(candidate), "%s/../../res", exe_dir);
			if (realpath(candidate, data_dir) != NULL)
				return data_dir;
		}
	}
	static_strcpy(data_dir, RUFUS_DATADIR);
	return data_dir;
}

static const char* resource_path(int id)
{
	size_t i;
	for (i = 0; i < ARRAYSIZE(rufus_resources); i++)
		if (rufus_resources[i].id == id)
			return rufus_resources[i].path;
	return NULL;
}

HRSRC FindResourceA(HMODULE hModule, LPCSTR lpName, LPCSTR lpType)
{
	int id = (int)(uintptr_t)lpName;
	const char* rel;
	char path[MAX_PATH];
	size_t i, slot = ARRAYSIZE(loaded);
	FILE* f;
	long len;
	uint8_t* data;
	(void)hModule; (void)lpType;

	if (!IS_INTRESOURCE(lpName)) {
		SetLastError(ERROR_RESOURCE_NAME_NOT_FOUND);
		return NULL;
	}
	pthread_mutex_lock(&res_lock);
	for (i = 0; i < ARRAYSIZE(loaded); i++) {
		if (loaded[i].data != NULL && loaded[i].id == id) {
			pthread_mutex_unlock(&res_lock);
			return (HRSRC)&loaded[i];
		}
		if (loaded[i].data == NULL && slot == ARRAYSIZE(loaded))
			slot = i;
	}
	rel = resource_path(id);
	if (rel == NULL || slot == ARRAYSIZE(loaded)) {
		pthread_mutex_unlock(&res_lock);
		SetLastError(ERROR_RESOURCE_NAME_NOT_FOUND);
		return NULL;
	}
	snprintf(path, sizeof(path), "%s/%s", linux_get_data_dir(), rel);
	f = fopenU(path, "rb");
	if (f == NULL) {
		pthread_mutex_unlock(&res_lock);
		SetLastError(ERROR_RESOURCE_DATA_NOT_FOUND);
		return NULL;
	}
	fseek(f, 0, SEEK_END);
	len = ftell(f);
	fseek(f, 0, SEEK_SET);
	/* Always allocate at least one byte, for empty resources */
	data = malloc((len > 0) ? (size_t)len : 1);
	if (data == NULL || (len > 0 && fread(data, 1, (size_t)len, f) != (size_t)len)) {
		free(data);
		fclose(f);
		pthread_mutex_unlock(&res_lock);
		SetLastError(ERROR_RESOURCE_DATA_NOT_FOUND);
		return NULL;
	}
	fclose(f);
	loaded[slot].id = id;
	loaded[slot].data = data;
	loaded[slot].size = (DWORD)max(len, 0L);
	pthread_mutex_unlock(&res_lock);
	return (HRSRC)&loaded[slot];
}

HGLOBAL LoadResource(HMODULE hModule, HRSRC hResInfo)
{
	(void)hModule;
	return (HGLOBAL)hResInfo;
}

LPVOID LockResource(HGLOBAL hResData)
{
	return (hResData == NULL) ? NULL : ((loaded_resource*)hResData)->data;
}

DWORD SizeofResource(HMODULE hModule, HRSRC hResInfo)
{
	(void)hModule;
	return (hResInfo == NULL) ? 0 : ((loaded_resource*)hResInfo)->size;
}

/* Same as stdfn.c's GetResource() */
uint8_t* GetResource(HMODULE module, char* name, char* type, const char* desc, DWORD* len, BOOL duplicate)
{
	HGLOBAL res_handle;
	HRSRC res;
	DWORD res_len;
	uint8_t* p = NULL;

	res = FindResourceA(module, name, type);
	if (res == NULL) {
		uprintf("Could not locate resource '%s': %s", desc, WindowsErrorString());
		goto out;
	}
	res_handle = LoadResource(module, res);
	if (res_handle == NULL) {
		uprintf("Could not load resource '%s': %s", desc, WindowsErrorString());
		goto out;
	}
	res_len = SizeofResource(module, res);

	if (duplicate) {
		if (*len == 0)
			*len = res_len;
		p = calloc(*len, 1);
		if (p == NULL) {
			uprintf("Could not allocate resource '%s'", desc);
			goto out;
		}
		memcpy(p, LockResource(res_handle), min(res_len, *len));
		if (res_len > *len)
			uprintf("WARNING: Resource '%s' was truncated by %d bytes!", desc, res_len - *len);
	} else {
		p = LockResource(res_handle);
	}
	*len = res_len;

out:
	return p;
}

DWORD GetResourceSize(HMODULE module, char* name, char* type, const char* desc)
{
	DWORD len = 0;
	return (GetResource(module, name, type, desc, &len, FALSE) == NULL) ? 0 : len;
}

/* ------------------------------------------------------------------------- */
/* Files                                                                     */
/* ------------------------------------------------------------------------- */
/*
 * Same as stdfn.c's FileIO(). On Windows, files are created with the user's
 * ownership rather than the administrator's: likewise, files we write are
 * handed over to the user who launched us through pkexec/sudo.
 */
BOOL FileIO(enum file_io_type io_type, char* path, char** buffer, DWORD* size)
{
	const LARGE_INTEGER liZero = { .QuadPart = 0ULL };
	HANDLE handle;
	DWORD dwDesiredAccess = 0, dwCreationDisposition = 0;
	BOOL r = FALSE, ret = FALSE;
	uid_t uid;
	gid_t gid;

	switch (io_type) {
	case FILE_IO_READ:
		*buffer = NULL;
		dwDesiredAccess = GENERIC_READ;
		dwCreationDisposition = OPEN_EXISTING;
		break;
	case FILE_IO_WRITE:
		dwDesiredAccess = GENERIC_WRITE;
		dwCreationDisposition = CREATE_ALWAYS;
		break;
	case FILE_IO_APPEND:
		dwDesiredAccess = FILE_APPEND_DATA;
		dwCreationDisposition = OPEN_ALWAYS;
		break;
	default:
		assert(FALSE);
		break;
	}
	handle = CreateFileU(path, dwDesiredAccess, FILE_SHARE_READ, NULL,
		dwCreationDisposition, FILE_ATTRIBUTE_NORMAL, NULL);
	if (handle == INVALID_HANDLE_VALUE) {
		uprintf("Could not open '%s': %s", path, WindowsErrorString());
		goto out;
	}
	switch (io_type) {
	case FILE_IO_READ:
		*size = GetFileSize(handle, NULL);
		*buffer = (char*)malloc(*size);
		if (*buffer == NULL) {
			uprintf("Could not allocate buffer for reading file");
			goto out;
		}
		r = ReadFile(handle, *buffer, *size, size, NULL);
		break;
	case FILE_IO_APPEND:
		SetFilePointerEx(handle, liZero, NULL, FILE_END);
		// Fall through
	case FILE_IO_WRITE:
		r = WriteFile(handle, *buffer, *size, size, NULL);
		if (r && geteuid() == 0) {
			linux_invoking_user_ids(&uid, &gid);
			if (fchown(HandleToFd(handle), uid, gid) != 0)
				uprintf("Could not set the ownership of '%s': %s", path, strerror(errno));
		}
		break;
	}
	if (!r) {
		uprintf("I/O Error: %s", WindowsErrorString());
		goto out;
	}
	PrintInfoDebug(0, (io_type == FILE_IO_READ) ? MSG_215 : MSG_216, path);
	ret = TRUE;
out:
	if (handle != INVALID_HANDLE_VALUE)
		CloseHandle(handle);
	if (!ret && (io_type == FILE_IO_READ)) {
		safe_free(*buffer);
		*size = 0;
	}
	return ret;
}

/* ------------------------------------------------------------------------- */
/* Commands                                                                  */
/* ------------------------------------------------------------------------- */
/*
 * Run a command, with optional redirection of stdout and stderr to our log
 * as well as optional progress reporting if msg is not 0 (see stdfn.c).
 */
DWORD RunCommandWithProgress(const char* cmd, const char* dir, BOOL log, int msg, const char* pattern)
{
	posix_spawn_file_actions_t fa;
	int pipefd[2] = { -1, -1 }, status = 0, rc;
	pid_t pid = -1;
	DWORD ret = ERROR_GEN_FAILURE;
	char output[4097];
	ssize_t n;
	cregex_node_t* node = NULL;
	cregex_program_t* program = NULL;
	char* matches[REGEX_VM_MAX_MATCHES];
	char* lcmd = linux_path(cmd);
	char* ldir = (dir == NULL) ? NULL : linux_path(dir);
	const char* argv[] = { "/bin/sh", "-c", lcmd, NULL };
	struct pollfd pfd;

	if (lcmd == NULL)
		goto out;
	if (pipe2(pipefd, O_CLOEXEC) != 0) {
		ret = ErrnoToWin32(errno);
		uprintf("Could not set commandline pipe: %s", strerror(errno));
		goto out;
	}
	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_adddup2(&fa, pipefd[1], STDOUT_FILENO);
	posix_spawn_file_actions_adddup2(&fa, pipefd[1], STDERR_FILENO);
	posix_spawn_file_actions_addopen(&fa, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
	if (ldir != NULL)
		posix_spawn_file_actions_addchdir_np(&fa, ldir);
	rc = posix_spawn(&pid, argv[0], &fa, NULL, (char* const*)argv, environ);
	posix_spawn_file_actions_destroy(&fa);
	close(pipefd[1]);
	pipefd[1] = -1;
	if (rc != 0) {
		ret = ErrnoToWin32(rc);
		uprintf("Unable to launch command '%s': %s", cmd, strerror(rc));
		goto out;
	}

	if (msg != 0) {
		node = cregex_parse(pattern);
		if (node != NULL) {
			program = cregex_compile_node(node);
			cregex_parse_free(node);
		}
		if (node == NULL || program == NULL)
			uprintf("Internal error: Failed to parse '%s'", pattern);
		else
			UpdateProgressWithInfoInit(NULL, FALSE);
	}

	pfd.fd = pipefd[0];
	pfd.events = POLLIN;
	for (;;) {
		/* Check for user cancel */
		if (IS_ERROR(ErrorStatus) && (SCODE_CODE(ErrorStatus) == ERROR_CANCELLED)) {
			kill(pid, SIGTERM);
			uprintf("Command was terminated by user");
			waitpid(pid, &status, 0);
			ret = ERROR_CANCELLED;
			goto out;
		}
		if (poll(&pfd, 1, 100) > 0) {
			n = read(pipefd[0], output, sizeof(output) - 1);
			if (n == 0)
				break;
			if (n > 0) {
				output[n] = 0;
				if (program != NULL && cregex_program_run(program, output, (const char**)matches, ARRAYSIZE(matches)) > 0 &&
					matches[2] != NULL && matches[3] != NULL) {
					float f = 0.0f;
					matches[3][0] = '\0';
					IGNORE_RETVAL(sscanf(matches[2], "%f", &f));
					UpdateProgressWithInfo(OP_FORMAT, msg, (uint64_t)(f * 100.0f), 100 * 100ULL);
				} else if (log) {
					uprintf("%s", output);
				}
			}
		}
	}
	while (waitpid(pid, &status, 0) < 0 && errno == EINTR);
	ret = WIFEXITED(status) ? (DWORD)WEXITSTATUS(status) : ERROR_GEN_FAILURE;

out:
	cregex_compile_free(program);
	if (pipefd[0] >= 0)
		close(pipefd[0]);
	if (pipefd[1] >= 0)
		close(pipefd[1]);
	free(lcmd);
	free(ldir);
	return ret;
}

/* ------------------------------------------------------------------------- */
/* System information                                                        */
/* ------------------------------------------------------------------------- */
/*
 * Rufus uses the host Windows version to gate features (e.g. the Windows User
 * Experience options, which require Windows 8 or later on the host). On Linux,
 * the host is equivalent to a current Windows 11 system.
 */
void GetWindowsVersion(windows_version_t* WinVer)
{
	struct utsname un;
	char pretty[96] = "Linux";
	FILE* f;
	char line[256];

	memset(WinVer, 0, sizeof(*WinVer));
	WinVer->Version = WINDOWS_11;
	WinVer->Major = 10;
	WinVer->Minor = 0;
	WinVer->BuildNumber = 26100;
	WinVer->Edition = 0;
#if defined(__x86_64__)
	WinVer->Arch = IMAGE_FILE_MACHINE_AMD64;
#elif defined(__i386__)
	WinVer->Arch = IMAGE_FILE_MACHINE_I386;
#elif defined(__aarch64__)
	WinVer->Arch = IMAGE_FILE_MACHINE_ARM64;
#elif defined(__arm__)
	WinVer->Arch = IMAGE_FILE_MACHINE_ARMNT;
#elif defined(__riscv) && (__riscv_xlen == 64)
	WinVer->Arch = IMAGE_FILE_MACHINE_RISCV64;
#elif defined(__loongarch64)
	WinVer->Arch = IMAGE_FILE_MACHINE_LOONGARCH64;
#endif
	f = fopenU("/etc/os-release", "r");
	if (f != NULL) {
		while (fgets(line, sizeof(line), f) != NULL) {
			if (strncmp(line, "PRETTY_NAME=", 12) == 0) {
				char* p = &line[12];
				if (*p == '"')
					p++;
				p[strcspn(p, "\"\r\n")] = 0;
				static_strcpy(pretty, p);
				break;
			}
		}
		fclose(f);
	}
	if (uname(&un) == 0)
		static_sprintf(WinVer->VersionStr, "%s (Linux %s %s)", pretty, un.release, un.machine);
	else
		static_strcpy(WinVer->VersionStr, pretty);
}

BOOL isSMode(void)
{
	return FALSE;
}

version_t* GetExecutableVersion(const char* path)
{
	(void)path;
	return NULL;
}

BOOL IsFontAvailable(const char* font_name)
{
	(void)font_name;
	return TRUE;
}

BOOL SetNDTA(BOOL bRestore, BOOL* bExistingKey, DWORD dwValue)
{
	(void)bRestore; (void)dwValue;
	if (bExistingKey != NULL)
		*bExistingKey = FALSE;
	return TRUE;
}

BOOL IsCurrentProcessElevated(void)
{
	return (geteuid() == 0);
}

BOOL SetPrivilege(HANDLE hToken, LPCSTR lpszPrivilege, BOOL bEnablePrivilege)
{
	(void)hToken; (void)lpszPrivilege; (void)bEnablePrivilege;
	return TRUE;
}

/*
 * Offline registry hives cannot be mounted on Linux. Callers (WUE) then fall
 * back to applying the same registry settings through unattend.xml, which is
 * also what the Windows Store version of Rufus does.
 */
BOOL MountRegistryHive(const HKEY key, const char* pszHiveName, const char* pszHivePath)
{
	(void)key; (void)pszHiveName; (void)pszHivePath;
	SetLastError(ERROR_NOT_SUPPORTED);
	return FALSE;
}

BOOL UnmountRegistryHive(const HKEY key, const char* pszHiveName)
{
	(void)key; (void)pszHiveName;
	return TRUE;
}

BOOL TakeOwnership(LPCSTR lpszOwnFile)
{
	(void)lpszOwnFile;
	return TRUE;
}
