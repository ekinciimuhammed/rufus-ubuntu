/*
 * Rufus for Linux: search for the processes that are accessing a drive
 * Copyright © 2017-2026 Pete Batard <pete@akeo.ie>
 * Copyright © 2026 Rufus Linux port contributors
 *
 * This is the Linux counterpart of src/process.c, with the same public API
 * and semantics: a background thread repeatedly looks for processes that hold
 * the selected drive open, and GetProcessSearch() reports them, with their
 * access rights (r = 0x1, w = 0x2, x = 0x4).
 *
 * Instead of enumerating the system handle table through ntdll, the Linux
 * version scans /proc: open file descriptors on the disk or its partitions
 * (with the access mode taken from fdinfo), open files on the file systems
 * mounted from them, as well as current/root directories, executables and
 * memory mapped files residing on those file systems (reported as 'x', as
 * these are what will prevent a volume from being unmounted).
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
#include <assert.h>
#include <dirent.h>
#include <inttypes.h>
#include <limits.h>
#include <sys/stat.h>

#include "rufus.h"
#include "drive.h"
#include "missing.h"
#include "msapi_utf8.h"
#include "linux_drive.h"

/* From src/ntdll.h */
#define MAX_NUM_HANDLES             64	// Disk + partitions + mounted file systems
#define MAX_BLOCKING_PROCESSES      16
#define SEARCH_PROCESS_LOCK_TIMEOUT 2000

#define ACCESS_READ                 0x1
#define ACCESS_WRITE                0x2
#define ACCESS_EXEC                 0x4

typedef struct {
	uint64_t pid;			// PID of the process
	uint8_t access_rights;	// rwx access rights
	uint32_t seen_on_pass;	// nPass value of when this process was last detected
	char cmdline[2 * KB];	// Command line for the process
} ProcessEntry;

/* What we look for: block devices (disk and partitions) and the file systems mounted from them */
typedef struct {
	dev_t devt;
	BOOL is_fs;				// TRUE for a file system's st_dev, FALSE for a block device's st_rdev
} SearchTarget;

typedef struct {
	BOOL bActive;			// Indicates whether the search for processes is currently active
	uint32_t nVersion[2];	// Version indicator for the target list.
	uint32_t nHandles;		// Number of targets in the list below
	SearchTarget* Target;	// Targets we search against
	HANDLE hLock;			// Global lock to request for modifying this structure
	HANDLE hStart;			// Event indicating that the search for processes can be started
	ProcessEntry Process[MAX_BLOCKING_PROCESSES];	// Fixed size process list
	uint32_t nPass;			// Incremental counter of how many passes we ran
} BlockingProcess;

static HANDLE hSearchProcessThread = NULL;
static BlockingProcess blocking_process = { 0 };

extern BOOL usb_debug;
extern StrArray BlockingProcessList;

/* Read a whole (small) file into buf, NUL terminated. Returns the number of bytes read or -1. */
static ssize_t read_small_file(const char* path, char* buf, size_t size)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	ssize_t r, total = 0;
	if (fd < 0)
		return -1;
	while ((size_t)total < size - 1) {
		r = read(fd, buf + total, size - 1 - total);
		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0)
			break;
		total += r;
	}
	close(fd);
	buf[total] = 0;
	return total;
}

/* Find whether a device matches one of our targets */
static BOOL match_target(const SearchTarget* target, uint32_t n, dev_t devt, BOOL is_fs)
{
	uint32_t i;
	for (i = 0; i < n; i++)
		if (target[i].is_fs == is_fs && target[i].devt == devt)
			return TRUE;
	return FALSE;
}

/* Convert the open flags of a file descriptor (from /proc/PID/fdinfo/FD) to rwx access rights */
static uint8_t fd_access_rights(const char* pid, const char* fd)
{
	char path[64], buf[512], *p;
	unsigned long flags;

	snprintf(path, sizeof(path), "/proc/%s/fdinfo/%s", pid, fd);
	if (read_small_file(path, buf, sizeof(buf)) <= 0)
		return ACCESS_READ;
	p = strstr(buf, "flags:");
	if (p == NULL)
		return ACCESS_READ;
	flags = strtoul(p + 6, NULL, 8);
	switch (flags & O_ACCMODE) {
	case O_WRONLY: return ACCESS_WRITE;
	case O_RDWR: return ACCESS_READ | ACCESS_WRITE;
	default: return ACCESS_READ;
	}
}

/* Retrieve the command line of a process, as a single string */
static BOOL GetProcessCommandLine(const char* pid, char* cmdline, size_t size)
{
	char path[64];
	ssize_t i, len;

	snprintf(path, sizeof(path), "/proc/%s/cmdline", pid);
	len = read_small_file(path, cmdline, size);
	if (len > 0) {
		// Arguments are NUL separated
		while (len > 0 && cmdline[len - 1] == 0)
			len--;
		for (i = 0; i < len; i++)
			if (cmdline[i] == 0)
				cmdline[i] = ' ';
		cmdline[len] = 0;
		if (len > 0)
			return TRUE;
	}
	// Kernel threads and zombies have no command line: use the process name
	snprintf(path, sizeof(path), "/proc/%s/comm", pid);
	len = read_small_file(path, cmdline, size - 2);
	if (len > 0) {
		while (len > 0 && (cmdline[len - 1] == '\n' || cmdline[len - 1] == 0))
			cmdline[--len] = 0;
		memmove(&cmdline[1], cmdline, len + 1);
		cmdline[0] = '[';
		cmdline[len + 1] = ']';
		cmdline[len + 2] = 0;
		return TRUE;
	}
	return FALSE;
}

/*
 * Look for the accesses a process has on our targets.
 * Returns the combined rwx access rights (0 if the process doesn't access any target).
 */
static uint8_t GetProcessAccess(const char* pid, const SearchTarget* target, uint32_t n)
{
	static const char* links[] = { "cwd", "root", "exe" };
	char path[PATH_MAX], fd_path[PATH_MAX + 64], line[PATH_MAX + 128];
	uint8_t access_rights = 0;
	unsigned int maj, min;
	struct stat st;
	DIR* dir;
	struct dirent* de;
	FILE* f;
	size_t i;

	// Open file descriptors
	snprintf(path, sizeof(path), "/proc/%s/fd", pid);
	dir = opendir(path);
	if (dir != NULL) {
		while ((de = readdir(dir)) != NULL) {
			if (de->d_name[0] == '.')
				continue;
			snprintf(fd_path, sizeof(fd_path), "%s/%s", path, de->d_name);
			if (stat(fd_path, &st) != 0)
				continue;
			if ((S_ISBLK(st.st_mode) && match_target(target, n, st.st_rdev, FALSE)) ||
				(!S_ISBLK(st.st_mode) && !S_ISCHR(st.st_mode) && match_target(target, n, st.st_dev, TRUE)))
				access_rights |= fd_access_rights(pid, de->d_name);
		}
		closedir(dir);
	}

	// Current directory, root directory and executable residing on one of our file systems
	for (i = 0; i < ARRAYSIZE(links); i++) {
		snprintf(fd_path, sizeof(fd_path), "/proc/%s/%s", pid, links[i]);
		if (stat(fd_path, &st) == 0 && match_target(target, n, st.st_dev, TRUE))
			access_rights |= ACCESS_EXEC;
	}

	// Memory mapped files (shared libraries, etc.)
	snprintf(path, sizeof(path), "/proc/%s/maps", pid);
	f = fopen(path, "r");
	if (f != NULL) {
		while (fgets(line, sizeof(line), f) != NULL) {
			char perms[8];
			unsigned long inode;
			if (sscanf(line, "%*s %7s %*s %x:%x %lu", perms, &maj, &min, &inode) != 4 || inode == 0)
				continue;
			if (match_target(target, n, makedev(maj, min), TRUE))
				access_rights |= (strchr(perms, 'x') != NULL) ? ACCESS_EXEC : ACCESS_READ;
		}
		fclose(f);
	}
	return access_rights;
}

static BOOL is_pid(const char* name)
{
	if (*name == 0)
		return FALSE;
	for (; *name != 0; name++)
		if (*name < '0' || *name > '9')
			return FALSE;
	return TRUE;
}

/**
 * The search process thread.
 * Note: Avoid using uprintf statements here, as it may lock the thread.
 *
 * \param param The thread parameters.
 *
 * \return A thread exit code.
 */
static DWORD WINAPI SearchProcessThread(LPVOID param)
{
	BOOL bInitSuccess = FALSE;
	HANDLE hLock = NULL;
	SearchTarget* Target = NULL;
	uint32_t nHandles = 0;
	uint8_t access_rights;
	uint64_t start_time, pid;
	char cmdline[2 * KB], tmp[64];
	int j;
	DIR* dir;
	struct dirent* de;
	pid_t self = getpid();
	(void)param;

	// Initialize the blocking process struct
	memset(&blocking_process, 0, sizeof(blocking_process));
	hLock = CreateMutexA(NULL, TRUE, NULL);
	if (hLock == NULL)
		goto out;
	blocking_process.hStart = CreateEventA(NULL, TRUE, FALSE, NULL);
	if (blocking_process.hStart == NULL)
		goto out;
	if (!ReleaseMutex(hLock))
		goto out;
	// Only assign the mutex handle once our init is complete
	blocking_process.hLock = hLock;

	// Wait until we are signaled active one way or another
	if (!blocking_process.bActive &&
		(WaitForSingleObject(blocking_process.hStart, INFINITE) != WAIT_OBJECT_0)) {
		goto out;
	}

	bInitSuccess = TRUE;
	while (blocking_process.bActive) {
		// Get a lock to our data
		if (WaitForSingleObject(hLock, SEARCH_PROCESS_LOCK_TIMEOUT) != WAIT_OBJECT_0)
			goto out;
		// No targets to check => just sleep for a while
		if (blocking_process.nHandles == 0) {
			ReleaseMutex(hLock);
			Sleep(500);
			continue;
		}
		// Work on our own copy of the targets
		if (blocking_process.nVersion[0] != blocking_process.nVersion[1]) {
			if_assert_fails(blocking_process.Target != NULL && blocking_process.nHandles != 0) {
				ReleaseMutex(hLock);
				goto out;
			}
			free(Target);
			nHandles = blocking_process.nHandles;
			Target = calloc(nHandles, sizeof(SearchTarget));
			if (Target == NULL) {
				ReleaseMutex(hLock);
				goto out;
			}
			memcpy(Target, blocking_process.Target, nHandles * sizeof(SearchTarget));
			blocking_process.nVersion[1] = blocking_process.nVersion[0];
			blocking_process.nPass = 0;
		}
		ReleaseMutex(hLock);

		start_time = GetTickCount64();
		dir = opendir("/proc");
		if (dir == NULL) {
			Sleep(1000);
			continue;
		}
		while (blocking_process.bActive && (de = readdir(dir)) != NULL) {
			if (!is_pid(de->d_name))
				continue;
			pid = strtoull(de->d_name, NULL, 10);
			if (pid == (uint64_t)self)
				continue;
			access_rights = GetProcessAccess(de->d_name, Target, nHandles);
			if (access_rights == 0)
				continue;
			if (!GetProcessCommandLine(de->d_name, cmdline, sizeof(cmdline)))
				static_sprintf(cmdline, "Unknown_Process_%" PRIu64, pid);

			// Record the process
			if (WaitForSingleObject(hLock, SEARCH_PROCESS_LOCK_TIMEOUT) == WAIT_OBJECT_0) {
				ProcessEntry* pe = blocking_process.Process;
				// Prune entries that have not been detected on recent passes
				for (j = 0; j < MAX_BLOCKING_PROCESSES; j++)
					if (pe[j].pid != 0 && pe[j].seen_on_pass + 1 < blocking_process.nPass)
						pe[j].pid = 0;
				// Look for an existing entry or an empty slot
				for (j = 0; (j < MAX_BLOCKING_PROCESSES) && (pe[j].pid != pid); j++);
				if (j == MAX_BLOCKING_PROCESSES)
					for (j = 0; (j < MAX_BLOCKING_PROCESSES) && (pe[j].pid != 0); j++);
				if (j != MAX_BLOCKING_PROCESSES) {
					pe[j].pid = pid;
					pe[j].access_rights = access_rights & 0x7;
					pe[j].seen_on_pass = blocking_process.nPass;
					static_strcpy(pe[j].cmdline, cmdline);
				} else if (usb_debug) {
					OutputDebugStringA("SearchProcessThread: No empty slot!\n");
				}
				ReleaseMutex(hLock);
			}
		}
		closedir(dir);
		blocking_process.nPass++;
		if (usb_debug) {
			static_sprintf(tmp, "Process search run #%d completed in %llu ms\n",
				blocking_process.nPass, (unsigned long long)(GetTickCount64() - start_time));
			OutputDebugStringA(tmp);
		}
		Sleep(1000);
	}

out:
	if (!bInitSuccess)
		uprintf("WARNING: Could not start process handle enumerator!");

	free(Target);

	if ((hLock != NULL) && (hLock != INVALID_HANDLE_VALUE) &&
		(WaitForSingleObject(hLock, 1000) == WAIT_OBJECT_0)) {
		blocking_process.hLock = NULL;
		blocking_process.bActive = FALSE;
		safe_free(blocking_process.Target);
		blocking_process.nHandles = 0;
		safe_closehandle(blocking_process.hStart);
		ReleaseMutex(hLock);
	}
	safe_closehandle(hLock);

	ExitThread(0);
}

/**
 * Start the process search thread.
 *
 * \return TRUE on success, FALSE otherwise.
 *
 */
BOOL StartProcessSearch(void)
{
	int i;

	if (hSearchProcessThread != NULL)
		return TRUE;

	hSearchProcessThread = CreateThread(NULL, 0, SearchProcessThread, NULL, 0, NULL);
	if (hSearchProcessThread == NULL) {
		uprintf("Failed to start process search thread: %s", WindowsErrorString());
		return FALSE;
	}
	SetThreadPriority(hSearchProcessThread, THREAD_PRIORITY_LOWEST);

	for (i = 0; (i < 50) && (blocking_process.hLock == NULL); i++)
		Sleep(100);
	if (i >= 50) {
		uprintf("Failed to start process search thread: hLock init failure!");
		TerminateThread(hSearchProcessThread, 0);
		CloseHandle(hSearchProcessThread);
		hSearchProcessThread = NULL;
		return FALSE;
	}

	return TRUE;
}

/**
 * Stop the process search thread..
 *
 */
void StopProcessSearch(void)
{
	if (hSearchProcessThread == NULL)
		return;

	blocking_process.bActive = FALSE;
	// Wake the thread up if it's still waiting to be started
	if (blocking_process.hStart != NULL)
		SetEvent(blocking_process.hStart);
	if (WaitForSingleObject(hSearchProcessThread, SEARCH_PROCESS_LOCK_TIMEOUT) != WAIT_OBJECT_0) {
		uprintf("Process search thread did not exit within timeout - forcefully terminating it!");
		TerminateThread(hSearchProcessThread, 0);
	}
	CloseHandle(hSearchProcessThread);
	hSearchProcessThread = NULL;
}

/* Add a target, if not already present */
static void add_target(SearchTarget* target, uint32_t* n, dev_t devt, BOOL is_fs)
{
	if (*n >= MAX_NUM_HANDLES || match_target(target, *n, devt, is_fs))
		return;
	target[*n].devt = devt;
	target[*n].is_fs = is_fs;
	(*n)++;
}

/* Parse a sysfs "major:minor" dev attribute */
static BOOL read_dev_attr(const char* path, dev_t* devt)
{
	char buf[32];
	unsigned int maj, min;
	if (read_small_file(path, buf, sizeof(buf)) <= 0 || sscanf(buf, "%u:%u", &maj, &min) != 2)
		return FALSE;
	*devt = makedev(maj, min);
	return TRUE;
}

/*
 * Add the file systems that are mounted from one of the block devices of our
 * target list (the st_dev of their files can differ from the device's st_rdev,
 * e.g. for btrfs or FUSE based ntfs-3g mounts).
 */
static void add_mounted_targets(SearchTarget* target, uint32_t* n)
{
	FILE* f;
	char line[4096], source[PATH_MAX], *sep;
	unsigned int maj, min;
	struct stat st;
	uint32_t nb_devices = *n;

	f = fopen("/proc/self/mountinfo", "r");
	if (f == NULL)
		return;
	while (fgets(line, sizeof(line), f) != NULL) {
		if (sscanf(line, "%*d %*d %u:%u", &maj, &min) != 2)
			continue;
		if (match_target(target, nb_devices, makedev(maj, min), FALSE)) {
			add_target(target, n, makedev(maj, min), TRUE);
			continue;
		}
		sep = strstr(line, " - ");
		if (sep != NULL && sscanf(sep + 3, "%*s %4095s", source) == 1 && strncmp(source, "/dev/", 5) == 0 &&
			stat(source, &st) == 0 && S_ISBLK(st.st_mode) && match_target(target, nb_devices, st.st_rdev, FALSE))
			add_target(target, n, makedev(maj, min), TRUE);
	}
	fclose(f);
}

/* Fill a target list with a disk, its partitions and the file systems mounted from them */
static uint32_t build_disk_targets(DWORD DeviceNum, SearchTarget* target)
{
	const char* kname = linux_get_disk_kname(DeviceNum);
	char path[PATH_MAX];
	uint32_t n = 0;
	dev_t devt;
	DIR* dir;
	struct dirent* de;

	if (kname == NULL)
		return 0;
	static_sprintf(path, "/sys/block/%s/dev", kname);
	if (read_dev_attr(path, &devt))
		add_target(target, &n, devt, FALSE);
	static_sprintf(path, "/sys/block/%s", kname);
	dir = opendir(path);
	if (dir != NULL) {
		while ((de = readdir(dir)) != NULL) {
			if (strncmp(de->d_name, kname, strlen(kname)) != 0)
				continue;
			static_sprintf(path, "/sys/block/%s/%s/dev", kname, de->d_name);
			if (read_dev_attr(path, &devt))
				add_target(target, &n, devt, FALSE);
		}
		closedir(dir);
	}
	add_mounted_targets(target, &n);
	return n;
}

/**
 * Set up the targets that the process search will run against.
 *
 * \param DeviceNum The device number for the currently selected drive.
 *
 * \return TRUE on success, FALSE otherwise.
 *
 */
BOOL SetProcessSearch(DWORD DeviceNum)
{
	SearchTarget* Target = NULL;
	uint32_t nHandles = 0;

	if (hSearchProcessThread == NULL) {
		uprintf("Process search thread is not started!");
		return FALSE;
	}

	assert(blocking_process.hLock != NULL);

	Target = calloc(MAX_NUM_HANDLES, sizeof(SearchTarget));
	if (Target == NULL)
		return FALSE;
	nHandles = build_disk_targets(DeviceNum, Target);

	if (WaitForSingleObject(blocking_process.hLock, SEARCH_PROCESS_LOCK_TIMEOUT) != WAIT_OBJECT_0) {
		uprintf("Could not obtain process search lock");
		free(Target);
		nHandles = 0;
		return FALSE;
	}

	free(blocking_process.Target);
	blocking_process.Target = Target;
	blocking_process.nHandles = nHandles;
	blocking_process.nVersion[0]++;
	blocking_process.bActive = TRUE;
	if (!SetEvent(blocking_process.hStart))
		uprintf("Could not signal start event to process search: %s", WindowsErrorString());
	return ReleaseMutex(blocking_process.hLock);
}

/**
 * Return the parent PID of a process (0 if unknown).
 */
DWORD GetPPID(DWORD pid)
{
	char path[64], buf[1024], *p;
	unsigned long ppid;

	snprintf(path, sizeof(path), "/proc/%u/stat", pid);
	if (read_small_file(path, buf, sizeof(buf)) <= 0)
		return 0;
	// The process name can contain spaces and parentheses: skip to the last ')'
	p = strrchr(buf, ')');
	if (p == NULL || sscanf(p + 1, " %*c %lu", &ppid) != 1)
		return 0;
	return (DWORD)ppid;
}

/**
 * Check whether the corresponding PID is that of a running process.
 *
 * \param pid The PID of the process to check.
 *
 * \return TRUE if the process is detected as currently running, FALSE otherwise.
 *
 */
static BOOL IsProcessRunning(uint64_t pid)
{
	char path[64], buf[1024], *p;
	char state;

	snprintf(path, sizeof(path), "/proc/%" PRIu64 "/stat", pid);
	if (read_small_file(path, buf, sizeof(buf)) <= 0)
		return FALSE;
	p = strrchr(buf, ')');
	if (p == NULL || sscanf(p + 1, " %c", &state) != 1)
		return FALSE;
	// Zombies and dead processes don't hold any resources anymore
	return (state != 'Z' && state != 'X' && state != 'x');
}

/*
 * Ignore read-only access from the desktop's device probing services, as it's
 * usually no big deal (this is the equivalent of ignoring explorer.exe on Windows).
 */
static BOOL IsBenignReader(const char* cmdline)
{
	static const char* benign[] = { "udisksd", "systemd-udevd", "(udev-worker)", "[udev-worker]",
		"gvfs-udisks2-volume-monitor", "gvfsd" };
	char exe[256], *p;
	size_t i;

	static_strcpy(exe, cmdline);
	p = strchr(exe, ' ');
	if (p != NULL)
		*p = 0;
	p = strrchr(exe, '/');
	p = (p == NULL) ? exe : p + 1;
	for (i = 0; i < ARRAYSIZE(benign); i++)
		if (strcmp(p, benign[i]) == 0)
			return TRUE;
	return FALSE;
}

/**
 * Report the result of the process search.
 *
 * \param timeout Maximum time that should be spend in this function before aborting (in ms).
 * \param access_mask Desired access mask (x = 0x4, w = 0x2, r = 0x1).
 * \param bIgnoreStaleProcesses Whether to ignore processes that are no longer active.
 *
 * \return The combined access mask of all the matching processes found.
 *         The BlockingProcessList string array is also updated with the results.
 *
 */
BYTE GetProcessSearch(uint32_t timeout, uint8_t access_mask, BOOL bIgnoreStaleProcesses)
{
	const char* access_rights_str[8] = { "n", "r", "w", "rw", "x", "rx", "wx", "rwx" };
	char tmp[2 * KB + 64];
	int i, j;
	uint32_t elapsed = 0;
	BYTE returned_mask = 0;

	StrArrayClear(&BlockingProcessList);
	if (hSearchProcessThread == NULL) {
		uprintf("Process search thread is not started!");
		return 0;
	}

	if_assert_fails(blocking_process.hLock != NULL)
		return 0;

retry:
	if (WaitForSingleObject(blocking_process.hLock, SEARCH_PROCESS_LOCK_TIMEOUT) != WAIT_OBJECT_0)
		return 0;

	// Make sure we have at least one pass with the current handles in order to report them.
	// If we have a timeout, wait until timeout has elapsed to give up.
	if ((blocking_process.nVersion[0] != blocking_process.nVersion[1]) ||
		(blocking_process.nPass < 1)) {
		ReleaseMutex(blocking_process.hLock);
		if (elapsed < timeout) {
			Sleep(100);
			elapsed += 100;
			goto retry;
		}
		if (timeout != 0)
			uprintf("Timeout while retrieving conflicting process list");
		return 0;
	}

	for (i = 0, j = 0; i < MAX_BLOCKING_PROCESSES; i++) {
		if (blocking_process.Process[i].pid == 0)
			continue;
		if ((blocking_process.Process[i].access_rights & access_mask) == 0)
			continue;
		if (bIgnoreStaleProcesses && !IsProcessRunning(blocking_process.Process[i].pid))
			continue;
		// Ignore read-only access from the desktop's device probing services
		if (blocking_process.Process[i].access_rights == 0x1 &&
			IsBenignReader(blocking_process.Process[i].cmdline)) {
			continue;
		}
		returned_mask |= blocking_process.Process[i].access_rights;
		static_sprintf(tmp, "● [%llu] %s (%s)", (unsigned long long)blocking_process.Process[i].pid,
			blocking_process.Process[i].cmdline, access_rights_str[blocking_process.Process[i].access_rights & 0x7]);
		StrArrayAdd(&BlockingProcessList, tmp, TRUE);
		if (j++ == 0)
			uprintf("WARNING: The following application(s) or service(s) are accessing the drive:");
		// tmp may contain a '%' so don't feed it as a naked format string
		uprintf("%s", tmp);
	}
	if (j != 0)
		uprintf("You should close these applications before retrying the operation.");
	ReleaseMutex(blocking_process.hLock);

	return returned_mask & access_mask;
}

/**
 * Alternative search for processes keeping a handle on a specific disk, volume
 * or file system path. Unlike the search thread, this is a one-shot scan.
 *
 * \param HandleName The device (e.g. "/dev/sdb1") or mounted path to look for.
 *
 * \return TRUE if processes were found, FALSE otherwise.
 */
BOOL SearchProcessAlt(char* HandleName)
{
	SearchTarget target[MAX_NUM_HANDLES];
	uint32_t n = 0;
	char* path = linux_path(HandleName);
	struct stat st;
	BOOL bFound = FALSE;
	DIR* dir = NULL;
	struct dirent* de;
	pid_t self = getpid();

	if (path == NULL || stat(path, &st) != 0) {
		uprintf("SearchProcessAlt('%s') failed: %s", HandleName, WindowsErrorString());
		goto out;
	}
	if (S_ISBLK(st.st_mode)) {
		add_target(target, &n, st.st_rdev, FALSE);
		add_mounted_targets(target, &n);
	} else {
		add_target(target, &n, st.st_dev, TRUE);
	}

	dir = opendir("/proc");
	if (dir == NULL) {
		uprintf("SearchProcessAlt('%s') failed: %s", HandleName, WindowsErrorString());
		goto out;
	}
	while ((de = readdir(dir)) != NULL) {
		if (!is_pid(de->d_name) || strtoull(de->d_name, NULL, 10) == (uint64_t)self)
			continue;
		if (GetProcessAccess(de->d_name, target, n) == 0)
			continue;
		if (!bFound)
			uprintf("WARNING: The following process(es) or service(s) are accessing %s:", HandleName);
		bFound = TRUE;
		uprintf("o Process with PID %s", de->d_name);
	}

out:
	if (dir != NULL)
		closedir(dir);
	free(path);
	return bFound;
}

/**
 * Increase the privileges of the current application.
 * On Linux, Rufus runs as root (through pkexec), which already grants everything
 * that SeDebugPrivilege provides on Windows.
 *
 * \return TRUE if the request was successful.
 */
BOOL EnablePrivileges(void)
{
	if (geteuid() != 0) {
		ubprintf("NOTE: Could not set process privileges: Not running as root");
		return FALSE;
	}
	return TRUE;
}
