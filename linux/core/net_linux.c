/*
 * Rufus: The Reliable USB Formatting Utility
 * Networking functionality (web file download, check for update, etc.) - Linux version
 * Copyright © 2012-2026 Pete Batard <pete@akeo.ie>
 * Copyright © 2026 Rufus Linux port contributors
 *
 * This is the Linux counterpart of src/net.c (plus the network related
 * parts of src/stdlg.c: SetUpdateCheck(), SetFidoCheck()). WinINet is
 * replaced with libcurl, and the logic, messages and settings are the same
 * as on Windows, with the following exceptions:
 * - Rufus is updated through the package manager on Linux, so the online
 *   check never offers to download a new application version. It still
 *   performs the UEFI DBX update check, as well as the retrieval of the
 *   latest SBAT/Secure Boot revocation data.
 * - The Fido script, used to download Windows retail ISOs, is run through
 *   PowerShell Core (pwsh) in command line mode, with the selection being
 *   performed through Rufus' own dialogs.
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
#include <stdio.h>
#include <malloc.h>
#include <string.h>
#include <inttypes.h>
#include <assert.h>
#include <time.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <pthread.h>
#include <sys/wait.h>

#include <curl/curl.h>

#include "rufus.h"
#include "missing.h"
#include "resource.h"
#include "msapi_utf8.h"
#include "localization.h"
#include "bled/bled.h"
#include "dbx/dbx_info.h"

#include "settings.h"

/* Maximum download chunk size, in bytes */
#define DOWNLOAD_BUFFER_SIZE    (10*KB)
/* Default delay between update checks (1 day) */
#define DEFAULT_UPDATE_INTERVAL (24*3600)
/* How long we let the download script run for (a single query), in ms */
#define FIDO_TIMEOUT            (5 * 60 * 1000)
/* Maximum amount of script output we keep */
#define FIDO_MAX_OUTPUT         (1 * MB)

#ifndef RUFUS_VERSION_STR
#define RUFUS_VERSION_STR       "0.0"
#endif

/* WinINet error codes that the Windows headers would provide */
#ifndef ERROR_INTERNET_INVALID_URL
#define ERROR_INTERNET_INVALID_URL              12005L
#endif
#ifndef ERROR_INTERNET_ITEM_NOT_FOUND
#define ERROR_INTERNET_ITEM_NOT_FOUND           12028L
#endif
#ifndef ERROR_INTERNET_HTTPS_TO_HTTP_ON_REDIR
#define ERROR_INTERNET_HTTPS_TO_HTTP_ON_REDIR   12040L
#endif
#ifndef ERROR_INTERNET_DISCONNECTED
#define ERROR_INTERNET_DISCONNECTED             12163L
#endif
#ifndef ERROR_HTTP_INVALID_SERVER_RESPONSE
#define ERROR_HTTP_INVALID_SERVER_RESPONSE      12152L
#endif

DWORD DownloadStatus;
BYTE* fido_script = NULL;
HANDLE update_check_thread = NULL;

extern const char* efi_archname[ARCH_MAX];
extern char *sbat_level_txt, *sb_active_txt, *sb_revoked_txt;
static DWORD error_code, fido_len = 0;
static BOOL force_update_check = FALSE;

/*
 * Optional hooks into the front-end. These are weak references, so that this file
 * doesn't force the UI to provide them:
 * - UpdateCallback is the "update policy" dialog (see stdlg.c on Windows).
 * - SetTaskbarProgress* reflect the progress in the desktop's task bar.
 * - linux_set_iso_download_available() is called (from a worker thread) when the
 *   Windows ISO download feature becomes available, so that the front-end can turn
 *   the SELECT button into a SELECT/DOWNLOAD split button. Windows does this by
 *   adding BS_SPLITBUTTON to the IDC_SELECT button style.
 */
#pragma weak UpdateCallback
#pragma weak SetTaskbarProgressState
#pragma weak SetTaskbarProgressValue
void __attribute__((weak)) linux_set_iso_download_available(BOOL available)
{
	(void)available;
}

static void set_taskbar_state(TASKBAR_PROGRESS_FLAGS flags)
{
	if (SetTaskbarProgressState != NULL)
		SetTaskbarProgressState(flags);
}

static void set_taskbar_value(ULONGLONG completed, ULONGLONG total)
{
	if (SetTaskbarProgressValue != NULL)
		SetTaskbarProgressValue(completed, total);
}

static char* GetShortName(const char* url)
{
	static char short_name[128];
	char *p;
	size_t i, len = safe_strlen(url);
	if (len < 5)
		return NULL;

	for (i = len - 2; i > 0; i--) {
		if (url[i] == '/') {
			i++;
			break;
		}
	}
	memset(short_name, 0, sizeof(short_name));
	static_strcpy(short_name, &url[i]);
	// If the URL is followed by a query, remove that part
	// Make sure we detect escaped queries too
	p = strstr(short_name, "%3F");
	if (p != NULL)
		*p = 0;
	p = strstr(short_name, "%3f");
	if (p != NULL)
		*p = 0;
	for (i = 0; i < strlen(short_name); i++) {
		if ((short_name[i] == '?') || (short_name[i] == '#')) {
			short_name[i] = 0;
			break;
		}
	}
	return short_name;
}

/*
 * libcurl session handling (the equivalent of GetInternetSession())
 */
static pthread_once_t curl_init_once = PTHREAD_ONCE_INIT;
static BOOL curl_initialized = FALSE;

static void curl_init(void)
{
	curl_initialized = (curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK);
}

static const char* default_user_agent(void)
{
	return APPLICATION_NAME "/" RUFUS_VERSION_STR " (Linux)";
}

static CURL* GetInternetSession(const char* user_agent, const char* url, char* errbuf)
{
	CURL* curl;
	// Unless the original URL is plain HTTP, refuse to be redirected to anything else than HTTPS
	const char* redir_protocols = (_strnicmp(url, "http://", 7) == 0) ? "http,https" : "https";

	pthread_once(&curl_init_once, curl_init);
	if (!curl_initialized) {
		SetLastError(ERROR_INTERNET_DISCONNECTED);
		return NULL;
	}
	curl = curl_easy_init();
	if (curl == NULL) {
		SetLastError(ERROR_NOT_ENOUGH_MEMORY);
		return NULL;
	}
	errbuf[0] = 0;
	curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);
	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_USERAGENT, (user_agent == NULL) ? default_user_agent() : user_agent);
	curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
	curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, redir_protocols);
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 16L);
	curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
	// Set the timeouts: connection, then no data received during the same period of time
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, (long)NET_SESSION_TIMEOUT);
	curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
	curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, (long)((NET_SESSION_TIMEOUT + 999) / 1000));
	// Enable HTTP/2 protocol support
	curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, (long)CURL_HTTP_VERSION_2TLS);
	// Like WinINet with INTERNET_FLAG_NO_COOKIES/NO_UI, no cookies and no signals
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
	return curl;
}

/* Map a libcurl error to the closest WinINet error and report it */
static void ReportCurlError(CURLcode res, const char* url, const char* errbuf)
{
	const char* msg = (errbuf != NULL && errbuf[0] != 0) ? errbuf : curl_easy_strerror(res);
	DWORD err;

	switch (res) {
	case CURLE_URL_MALFORMAT:
		SetLastError(ERROR_INTERNET_INVALID_URL);
		uprintf("Unable to decode URL: %s", msg);
		return;
	case CURLE_UNSUPPORTED_PROTOCOL:
	case CURLE_TOO_MANY_REDIRECTS:
		if (_strnicmp(url, "https://", 8) == 0 && res == CURLE_UNSUPPORTED_PROTOCOL) {
			uprintf("Refusing to follow a redirect from HTTPS to a non HTTPS URL");
			SetLastError(ERROR_INTERNET_HTTPS_TO_HTTP_ON_REDIR);
			return;
		}
		err = ERROR_INTERNET_INVALID_URL;
		break;
	case CURLE_COULDNT_RESOLVE_PROXY:
	case CURLE_COULDNT_RESOLVE_HOST:
		SetLastError(ERROR_INTERNET_NAME_NOT_RESOLVED);
		uprintf("Could not connect to server for %s: %s", url, msg);
		return;
	case CURLE_COULDNT_CONNECT:
		SetLastError(ERROR_INTERNET_CANNOT_CONNECT);
		uprintf("Could not connect to server for %s: %s", url, msg);
		return;
	case CURLE_OPERATION_TIMEDOUT:
		err = ERROR_INTERNET_TIMEOUT;
		break;
	case CURLE_PEER_FAILED_VERIFICATION:
	case CURLE_SSL_CONNECT_ERROR:
	case CURLE_SSL_CERTPROBLEM:
	case CURLE_SSL_CIPHER:
	case CURLE_SSL_CACERT_BADFILE:
	case CURLE_SSL_ISSUER_ERROR:
		err = ERROR_INTERNET_SEC_CERT_ERRORS;
		break;
	case CURLE_SEND_ERROR:
	case CURLE_RECV_ERROR:
		err = ERROR_INTERNET_CONNECTION_RESET;
		break;
	case CURLE_GOT_NOTHING:
	case CURLE_WEIRD_SERVER_REPLY:
		err = ERROR_HTTP_INVALID_SERVER_RESPONSE;
		break;
	default:
		err = ERROR_INTERNET_CONNECTION_ABORTED;
		break;
	}
	SetLastError(err);
	uprintf("Could not open URL %s: %s", url, msg);
}

/* Context for the download callbacks */
typedef struct {
	CURL* curl;
	const char* url;
	const char* file;
	const char* short_name;
	HANDLE hFile;
	BYTE** buffer;
	HWND hProgressDialog;
	uint64_t size;
	uint64_t total_size;
	BOOL headers_processed;
	BOOL failed;
	BOOL probe_only;	// Only validate the headers (IsDownloadable)
} download_context;

/*
 * Called once the headers of the final (non redirect) response are available:
 * check the status, get the file length, and set up the output.
 */
static BOOL ProcessHeaders(download_context* ctx)
{
	long status = 404;
	curl_off_t length = -1;

	ctx->headers_processed = TRUE;
	curl_easy_getinfo(ctx->curl, CURLINFO_RESPONSE_CODE, &status);
	DownloadStatus = (DWORD)status;
	if (DownloadStatus != 200) {
		if (!ctx->probe_only) {
			error_code = ERROR_INTERNET_ITEM_NOT_FOUND;
			SetLastError(RUFUS_ERROR(error_code));
			uprintf("%s '%s': %d", (DownloadStatus == 404) ? "File not found" : "Unable to access file", ctx->url, DownloadStatus);
		}
		return FALSE;
	}
	if (curl_easy_getinfo(ctx->curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &length) != CURLE_OK || length < 0) {
		if (!ctx->probe_only)
			uprintf("Unable to retrieve file length: %s", "No Content-Length header");
		SetLastError(ERROR_HTTP_INVALID_SERVER_RESPONSE);
		return FALSE;
	}
	ctx->total_size = (uint64_t)length;
	if (ctx->probe_only)
		return TRUE;
	if (ctx->hProgressDialog != NULL) {
		char msg[128];
		uprintf("File length: %s", SizeToHumanReadable(ctx->total_size, FALSE, FALSE));
		if (right_to_left_mode)
			static_sprintf(msg, "(%s) %s", SizeToHumanReadable(ctx->total_size, FALSE, FALSE), GetShortName(ctx->url));
		else
			static_sprintf(msg, "%s (%s)", GetShortName(ctx->url), SizeToHumanReadable(ctx->total_size, FALSE, FALSE));
		PrintStatus(5000, MSG_085, msg);
	}

	if (ctx->file != NULL) {
		ctx->hFile = CreateFileU(ctx->file, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
		if (ctx->hFile == INVALID_HANDLE_VALUE) {
			uprintf("Unable to create file '%s': %s", ctx->short_name, WindowsErrorString());
			return FALSE;
		}
	} else {
		if (ctx->buffer == NULL) {
			uprintf("No buffer pointer provided for download");
			return FALSE;
		}
		// Don't let a bogus Content-Length wrap the allocation size around
		if (ctx->total_size > SIZE_MAX - 2) {
			uprintf("Invalid file length for download: %" PRIu64 " bytes", ctx->total_size);
			return FALSE;
		}
		// Allocate one extra byte, so that caller can rely on NUL-terminated text if needed
		*ctx->buffer = calloc((size_t)ctx->total_size + 2, 1);
		if (*ctx->buffer == NULL) {
			uprintf("Could not allocate buffer for download");
			return FALSE;
		}
	}
	return TRUE;
}

static size_t WriteCallback(char* ptr, size_t size, size_t nmemb, void* userdata)
{
	download_context* ctx = (download_context*)userdata;
	size_t len = size * nmemb;
	DWORD dwWritten;

	if (!ctx->headers_processed) {
		if (!ProcessHeaders(ctx) || ctx->probe_only) {
			ctx->failed = !ctx->probe_only || (DownloadStatus != 200);
			return 0;
		}
	}
	// User may have cancelled the download
	if (IS_ERROR(ErrorStatus))
		return 0;
	if (ctx->hProgressDialog != NULL)
		UpdateProgressWithInfo(OP_NOOP, MSG_241, ctx->size, ctx->total_size);
	if (ctx->file != NULL) {
		if (!WriteFile(ctx->hFile, ptr, (DWORD)len, &dwWritten, NULL)) {
			uprintf("Error writing file '%s': %s", ctx->short_name, WindowsErrorString());
			ctx->failed = TRUE;
			return 0;
		} else if (len != dwWritten) {
			uprintf("Error writing file '%s': Only %d/%d bytes written", ctx->short_name, dwWritten, (DWORD)len);
			ctx->failed = TRUE;
			return 0;
		}
	} else {
		// We must make sure that we don't receive more data than we allocated for.
		if (len > ctx->total_size - ctx->size) {
			uprintf("Received more data than the expected %" PRIu64 " bytes - Aborting", ctx->total_size);
			SetLastError(ERROR_INVALID_DATA);
			ctx->failed = TRUE;
			return 0;
		}
		memcpy(&(*ctx->buffer)[ctx->size], ptr, len);
	}
	ctx->size += len;
	return len;
}

/* Abort as soon as the user cancels, even if the connection is stalled */
static int ProgressCallback(void* clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow)
{
	(void)clientp; (void)dltotal; (void)dlnow; (void)ultotal; (void)ulnow;
	return IS_ERROR(ErrorStatus) ? 1 : 0;
}

/*
 * Download a file or fill a buffer from an URL
 * If file is NULL, a buffer is allocated for the download (that needs to be freed by the caller)
 * If hProgressDialog is not NULL, this function will send INIT and EXIT messages
 * to the dialog in question, with WPARAM being set to nonzero for EXIT on success
 * and also attempt to indicate progress using an IDC_PROGRESS control
 * Note that when a buffer is used, the actual size of the buffer is two more than its reported
 * size (with the extra bytes set to 0) to accommodate for calls that need NUL-terminated data.
 */
uint64_t DownloadToFileOrBufferEx(const char* url, const char* file, const char* user_agent,
	BYTE** buffer, HWND hProgressDialog, BOOL bTaskBarProgress)
{
	char errbuf[CURL_ERROR_SIZE], *bak_file = NULL;
	BOOL r = FALSE;
	CURLcode res;
	struct curl_slist* headers = NULL;
	download_context ctx = { 0 };

	(void)bTaskBarProgress;
	ErrorStatus = 0;
	DownloadStatus = 404;
	if (hProgressDialog != NULL)
		UpdateProgressWithInfoInit(hProgressDialog, FALSE);

	assert(url != NULL);
	if (buffer != NULL)
		*buffer = NULL;

	ctx.url = url;
	ctx.file = file;
	ctx.buffer = buffer;
	ctx.hFile = INVALID_HANDLE_VALUE;
	ctx.hProgressDialog = hProgressDialog;
	ctx.short_name = (file != NULL) ? PathFindFileNameU(file) : PathFindFileNameU(url);

	if (hProgressDialog != NULL) {
		PrintInfo(5000, MSG_085, ctx.short_name);
		uprintf("Downloading %s", url);
	}

	// If an old file with the same name exists, rename it so we can restore it in case of failure
	if (file != NULL && PathFileExistsU((char*)file) && (bak_file = calloc(1, strlen(file) + 6)) != NULL) {
		strcpy(bak_file, file);
		strcat(bak_file, ".bak");
		MoveFileU(file, bak_file);
	}

	ctx.curl = GetInternetSession(user_agent, url, errbuf);
	if (ctx.curl == NULL) {
		uprintf("Could not open Internet session: %s", WindowsErrorString());
		goto out;
	}

	// If we are querying the GitHub API, we need to enable raw content
	if (strstr(url, "api.github.com") != NULL)
		headers = curl_slist_append(headers, "Accept: application/vnd.github.v3.raw");
	// Must use "Accept-Encoding: identity" to get the file size
	headers = curl_slist_append(headers, "Accept-Encoding: identity");
	headers = curl_slist_append(headers, "Accept: */*");
	curl_easy_setopt(ctx.curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(ctx.curl, CURLOPT_WRITEFUNCTION, WriteCallback);
	curl_easy_setopt(ctx.curl, CURLOPT_WRITEDATA, &ctx);
	curl_easy_setopt(ctx.curl, CURLOPT_XFERINFOFUNCTION, ProgressCallback);
	curl_easy_setopt(ctx.curl, CURLOPT_NOPROGRESS, 0L);

	res = curl_easy_perform(ctx.curl);
	if (ctx.failed)
		goto out;
	if (res == CURLE_ABORTED_BY_CALLBACK || (res == CURLE_WRITE_ERROR && IS_ERROR(ErrorStatus)))
		goto out;
	if (res != CURLE_OK) {
		ReportCurlError(res, url, errbuf);
		goto out;
	}
	// Empty files don't go through the write callback
	if (!ctx.headers_processed && !ProcessHeaders(&ctx))
		goto out;

	if (ctx.size != ctx.total_size) {
		uprintf("Could not download complete file - read: %" PRIu64 " bytes, expected: %" PRIu64 " bytes", ctx.size, ctx.total_size);
		ErrorStatus = RUFUS_ERROR(ERROR_WRITE_FAULT);
		goto out;
	} else {
		DownloadStatus = 200;
		r = TRUE;
		if (hProgressDialog != NULL) {
			UpdateProgressWithInfo(OP_NOOP, MSG_241, ctx.total_size, ctx.total_size);
			uprintf("Successfully downloaded '%s'", ctx.short_name);
		}
	}

out:
	error_code = GetLastError();
	if (ctx.hFile != INVALID_HANDLE_VALUE) {
		// Force a flush - May help with processing downloaded files too early...
		FlushFileBuffers(ctx.hFile);
		CloseHandle(ctx.hFile);
	}
	if (!r) {
		if (file != NULL)
			DeleteFileU(file);
		if (bak_file != NULL)
			MoveFileU(bak_file, file);
		if (buffer != NULL)
			safe_free(*buffer);
	} else if (bak_file != NULL) {
		DeleteFileU(bak_file);
	}
	free(bak_file);
	curl_slist_free_all(headers);
	if (ctx.curl != NULL)
		curl_easy_cleanup(ctx.curl);

	SetLastError(error_code);
	return r ? ctx.size : 0;
}

// Download and validate a signed file. The file must have a corresponding '.sig' on the server.
DWORD DownloadSignedFile(const char* url, const char* file, HWND hProgressDialog, BOOL bPromptOnError, BOOL bSaveSig)
{
	char* sig_path = NULL;
	BYTE *buf = NULL, *sig = NULL;
	DWORD buf_len = 0, sig_len = 0;
	DWORD ret = 0;
	HANDLE hFile = INVALID_HANDLE_VALUE;

	if_assert_fails(url != NULL && file != NULL)
		goto out;

	sig_path = malloc(strlen(url) + 5);
	if (sig_path == NULL) {
		uprintf("Could not allocate signature URL");
		goto out;
	}
	strcpy(sig_path, url);
	strcat(sig_path, ".sig");

	buf_len = (DWORD)DownloadToFileOrBuffer(url, NULL, &buf, hProgressDialog, FALSE);
	if (buf_len == 0)
		goto out;
	sig_len = (DWORD)DownloadToFileOrBuffer(sig_path, NULL, &sig, NULL, FALSE);
	if ((sig_len != RSA_SIGNATURE_SIZE) || (!ValidateOpensslSignature(buf, buf_len, sig, sig_len))) {
		uprintf("FATAL: Download signature is invalid ✗");
		DownloadStatus = 403;	// Forbidden
		ErrorStatus = RUFUS_ERROR(APPERR(ERROR_BAD_SIGNATURE));
		SendMessage(GetDlgItem(hProgressDialog, IDC_PROGRESS), PBM_SETSTATE, (WPARAM)PBST_ERROR, 0);
		set_taskbar_state(TASKBAR_ERROR);
		goto out;
	}

	uprintf("Download signature is valid ✓");

	// Save the signature if requested
	if (bSaveSig) {
		free(sig_path);
		sig_path = malloc(strlen(file) + 5);
		if (sig_path == NULL) {
			uprintf("Could not allocate signature path");
			goto out;
		}
		strcpy(sig_path, file);
		strcat(sig_path, ".sig");
		if (write_file(sig_path, sig, sig_len) != sig_len)
			goto out;
	}

	DownloadStatus = 206;	// Partial content
	hFile = CreateFileU(file, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (hFile == INVALID_HANDLE_VALUE) {
		uprintf("Unable to create file '%s': %s", PathFindFileNameU(file), WindowsErrorString());
		goto out;
	}
	if (!WriteFile(hFile, buf, buf_len, &ret, NULL)) {
		uprintf("Error writing file '%s': %s", PathFindFileNameU(file), WindowsErrorString());
		ret = 0;
		goto out;
	} else if (ret != buf_len) {
		uprintf("Error writing file '%s': Only %d/%d bytes written", PathFindFileNameU(file), ret, buf_len);
		ret = 0;
		goto out;
	}
	DownloadStatus = 200;	// Full content

out:
	if (hProgressDialog != NULL)
		SendMessage(hProgressDialog, UM_PROGRESS_EXIT, (WPARAM)ret, 0);
	if ((bPromptOnError) && (DownloadStatus != 200)) {
		PrintInfo(0, MSG_242);
		SetLastError(error_code);
		Notification(MB_OK | MB_ICONERROR, lmprintf(MSG_044), IS_ERROR(ErrorStatus) ? StrError(ErrorStatus, FALSE) : WindowsErrorString());
	}
	safe_closehandle(hFile);
	free(sig_path);
	free(buf);
	free(sig);
	return ret;
}

/* Threaded download */
typedef struct {
	const char* url;
	const char* file;
	HWND hProgressDialog;
	BOOL bPromptOnError;
	BOOL bSaveSig;
} DownloadSignedFileThreadArgs;

static DWORD WINAPI DownloadSignedFileThread(LPVOID param)
{
	DownloadSignedFileThreadArgs* args = (DownloadSignedFileThreadArgs*)param;
	ExitThread(DownloadSignedFile(args->url, args->file, args->hProgressDialog, args->bPromptOnError, args->bSaveSig));
}

HANDLE DownloadSignedFileThreaded(const char* url, const char* file, HWND hProgressDialog, BOOL bPromptOnError, BOOL bSaveSig)
{
	static DownloadSignedFileThreadArgs args;
	args.url = url;
	args.file = file;
	args.hProgressDialog = hProgressDialog;
	args.bPromptOnError = bPromptOnError;
	args.bSaveSig = bSaveSig;
	return CreateThread(NULL, 0, DownloadSignedFileThread, &args, 0, NULL);
}

BOOL UseLocalDbx(int arch)
{
	char reg_name[32];
	static_sprintf(reg_name, "DBXTimestamp_%s", efi_archname[arch]);
	return (uint64_t)ReadSetting64(reg_name) > dbx_info[arch - 1].timestamp;
}

static void CheckForDBXUpdates(int verbose)
{
	int i, r;
	char reg_name[32], timestamp_url[256], path[MAX_PATH];
	char *p, *c, *rep, *buf = NULL;
	struct tm t = { 0 };
	uint64_t size, timestamp;
	BOOL already_prompted = FALSE;

	for (i = 0; i < ARRAYSIZE(dbx_info); i++) {
		// Get the epoch of the last commit
		timestamp = 0;
		static_strcpy(timestamp_url, dbx_info[i].url);
		p = strstr(timestamp_url, "contents/");
		if (p == NULL)
			continue;
		*p = 0;
		rep = replace_char(&p[9], '/', "%2F");
		static_strcat(timestamp_url, "commits?path=");
		static_strcat(timestamp_url, rep);
		free(rep);
		static_strcat(timestamp_url, "&page=1&per_page=1");
		vuprintf("Querying %s for DBX update timestamp", timestamp_url);
		size = DownloadToFileOrBuffer(timestamp_url, NULL, (BYTE**)&buf, NULL, FALSE);
		if (size == 0)
			continue;
		// Assumes that the GitHub JSON commit dates are of the form:
		// "date":[ ]*"2025-02-24T20:20:22Z"
		p = strstr(buf, "\"date\":");
		if (p == NULL) {
			safe_free(buf);
			continue;
		}
		c = &p[7];
		while (*c == ' ' || *c == '"')
			c++;
		p = c;
		while (*c != '"' && *c != '\0')
			c++;
		*c = 0;
		r = sscanf(p, "%d-%d-%dT%d:%d:%dZ", &t.tm_year, &t.tm_mon, &t.tm_mday, &t.tm_hour, &t.tm_min, &t.tm_sec);
		safe_free(buf);
		if (r != 6)
			continue;
		t.tm_year -= 1900;
		t.tm_mon -= 1;
		timestamp = (uint64_t)timegm(&t);
		vuprintf("DBX update timestamp is %" PRId64, timestamp);
		static_sprintf(reg_name, "DBXTimestamp_%s", efi_archname[i + 1]);
		// Check if we have an external DBX that is newer than embedded/last downloaded
		if (timestamp <= dbx_info[i].timestamp)
			continue;
		static_sprintf(path, "%s\\%s\\dbx_%s.bin", app_data_dir, FILES_DIR, efi_archname[i + 1]);
		if (PathFileExistsU(path) && timestamp <= (uint64_t)ReadSetting64(reg_name))
			continue;
		if (!already_prompted) {
			r = Notification(MB_YESNO | MB_ICONWARNING, lmprintf(MSG_353), lmprintf(MSG_354));
			already_prompted = TRUE;
			if (r != IDYES)
				break;
			IGNORE_RETVAL(_chdirU(app_data_dir));
			IGNORE_RETVAL(_mkdir(FILES_DIR));
			IGNORE_RETVAL(_chdir(FILES_DIR));
		}
		if (DownloadToFileOrBuffer(dbx_info[i].url, path, NULL, NULL, FALSE) != 0) {
			WriteSetting64(reg_name, timestamp);
			uprintf("Saved %s as 'dbx_%s.bin'", dbx_info[i].url, efi_archname[i + 1]);
		} else
			uprintf("WARNING: Failed to download %s", dbx_info[i].url);
	}
}

/* Current time, in seconds since 1601.01.01 (the same reference as the Windows version) */
static int64_t GetCurrentTimeSince1601(void)
{
	SYSTEMTIME SystemTime;
	FILETIME FileTime;

	GetSystemTime(&SystemTime);
	if (!SystemTimeToFileTime(&SystemTime, &FileTime))
		return 0;
	return ((((int64_t)FileTime.dwHighDateTime) << 32) + FileTime.dwLowDateTime) / 10000000;
}

/*
 * Background thread to check for updates (including UEFI DBX updates)
 * On Linux, the application itself is updated by the package manager, so the
 * check is limited to the UEFI DBX revocation data.
 */
static DWORD WINAPI CheckForUpdatesThread(LPVOID param)
{
	int i, verbose = 0;
	int64_t local_time = 0, reg_time, update_interval;

	(void)param;
	verbose = ReadSetting32(SETTING_VERBOSE_UPDATES);
	// Unless the update was forced, wait a while before performing the update check
	if (!force_update_check) {
		// It would of course be a lot nicer to use a timer and wake the thread, but my
		// development time is limited and this is FASTER to implement.
		do {
			for (i = 0; (i < 30) && (!force_update_check); i++)
				Sleep(500);
		} while ((!force_update_check) && ((op_in_progress || (dialog_showing > 0))));
		if (!force_update_check) {
			if ((ReadSetting32(SETTING_UPDATE_INTERVAL) == -1)) {
				vuprintf("Check for updates disabled, as per settings.");
				goto out;
			}
			reg_time = ReadSetting64(SETTING_LAST_UPDATE);
			update_interval = (int64_t)ReadSetting32(SETTING_UPDATE_INTERVAL);
			if (update_interval == 0) {
				WriteSetting32(SETTING_UPDATE_INTERVAL, DEFAULT_UPDATE_INTERVAL);
				update_interval = DEFAULT_UPDATE_INTERVAL;
			}
			local_time = GetCurrentTimeSince1601();
			if (local_time == 0)
				goto out;
			vvuprintf("Local time: %" PRId64, local_time);
			if (local_time < reg_time + update_interval) {
				vuprintf("Next update check in %" PRId64 " seconds.", reg_time + update_interval - local_time);
				goto out;
			}
		}
	}

	// Perform the DBX Update check
	PrintInfoDebug(3000, MSG_352);
	CheckForDBXUpdates(verbose);

	// On Windows, the time of the last check comes from the update server's answer.
	// Since we don't query the application update server, use the local time.
	local_time = GetCurrentTimeSince1601();
	if (local_time != 0)
		WriteSetting64(SETTING_LAST_UPDATE, local_time);
	vuprintf("Application updates are managed by the system's package manager - Skipping %s version check.", APPLICATION_NAME);

out:
	// No application update can be found on Linux
	if (force_update_check)
		PostMessage(hMainDialog, UM_NO_UPDATE, 0, 0);
	force_update_check = FALSE;
	update_check_thread = NULL;
	ExitThread(0);
}

/*
 * Initiate a check for updates. If force is true, ignore the wait period
 */
BOOL CheckForUpdates(BOOL force)
{
	force_update_check = force;
	if (update_check_thread != NULL)
		return FALSE;

	update_check_thread = CreateThread(NULL, 0, CheckForUpdatesThread, NULL, 0, NULL);
	if (update_check_thread == NULL) {
		uprintf("Unable to start update check thread");
		return FALSE;
	}
	return TRUE;
}

/*
 * On Windows, this downloads and launches a new version of the application.
 * On Linux, new versions are provided by the package manager.
 */
void DownloadNewVersion(void)
{
	uprintf("Application updates are managed by the system's package manager (e.g. 'sudo apt upgrade')");
}

/*
 * PowerShell Core detection
 * NB: When running elevated through pkexec, PATH is reset to a minimal value,
 * so also look in the locations used by the snap and the Microsoft packages.
 */
static const char* GetPowerShellPath(void)
{
	static char path[MAX_PATH];
	const char* fallback[] = { "/snap/bin/pwsh", "/usr/bin/pwsh", "/usr/local/bin/pwsh",
		"/opt/microsoft/powershell/7/pwsh", "/opt/microsoft/powershell/7-preview/pwsh" };
	const char* env = getenv("PATH");
	char *dirs, *dir, *save = NULL;
	size_t i;

	if (env != NULL && (dirs = strdup(env)) != NULL) {
		for (dir = strtok_r(dirs, ":", &save); dir != NULL; dir = strtok_r(NULL, ":", &save)) {
			if (dir[0] != '/')	// Ignore relative PATH entries
				continue;
			static_sprintf(path, "%s/pwsh", dir);
			if (access(path, X_OK) == 0) {
				free(dirs);
				return path;
			}
		}
		free(dirs);
	}
	for (i = 0; i < ARRAYSIZE(fallback); i++) {
		if (access(fallback[i], X_OK) == 0) {
			static_strcpy(path, fallback[i]);
			return path;
		}
	}
	return NULL;
}

/*
 * Use a thread to enable the download button as this may be a lengthy
 * operation due to the external download check.
 */
static DWORD WINAPI CheckForFidoThread(LPVOID param)
{
	static BOOL is_active = FALSE;
	BOOL has_powershell = (BOOL)(uintptr_t)param;
	char* loc = NULL;
	uint32_t i;
	uint64_t len;

	// Because a user may switch language before this thread has completed,
	// we need to detect concurrency.
	// Checking on a static boolean is more than good enough for our purpose.
	if (is_active)
		return (DWORD)-1;
	is_active = TRUE;
	safe_free(fido_url);
	safe_free(sbat_entries);
	safe_free(sbat_level_txt);
	safe_free(sb_active_txt);
	safe_free(sb_revoked_txt);

	// Get the latest sbat_level.txt data while we're poking the network for Fido.
	len = DownloadToFileOrBuffer(RUFUS_URL "/sbat_level.txt", NULL, (BYTE**)&sbat_level_txt, NULL, FALSE);
	if (len != 0 && len < 1 * KB) {
		sbat_entries = GetSbatEntries(sbat_level_txt);
		if (sbat_entries != NULL) {
			for (i = 0; sbat_entries[i].product != NULL; i++);
			if (i > 0)
				uprintf("Found %d additional UEFI revocation filters from remote SBAT", i);
		}
	}

	// Get the active Secure Boot certificate thumbprints
	len = DownloadToFileOrBuffer(RUFUS_URL "/sb_active.txt", NULL, (BYTE**)&sb_active_txt, NULL, FALSE);
	if (len != 0 && len < 1 * KB) {
		sb_active_certs = GetThumbprintEntries(sb_active_txt);
		if (sb_active_certs != NULL) {
			uprintf("Found %d active Secure Boot certificate entries from remote", sb_active_certs->count);
		}
	}

	// Get the revoked Secure Boot certificate thumbprints
	len = DownloadToFileOrBuffer(RUFUS_URL "/sb_revoked.txt", NULL, (BYTE**)&sb_revoked_txt, NULL, FALSE);
	if (len != 0 && len < 1 * KB) {
		sb_revoked_certs = GetThumbprintEntries(sb_revoked_txt);
		if (sb_revoked_certs != NULL) {
			uprintf("Found %d revoked Secure Boot certificate entries from remote", sb_revoked_certs->count);
		}
	}

	// The ISO download feature requires PowerShell
	if (!has_powershell)
		goto out;

	// Get the Fido URL from parsing a 'Fido.ver' on our server. This enables the use of different
	// Fido versions from different versions of Rufus, if needed, as opposed to always downloading
	// the latest release from GitHub, which may contain incompatible changes...
	len = DownloadToFileOrBuffer(RUFUS_URL "/Fido.ver", NULL, (BYTE**)&loc, NULL, FALSE);
	if ((len == 0) || (len >= 4 * KB))
		goto out;

	len++;	// DownloadToFileOrBuffer allocated an extra NUL character if needed
	fido_url = get_token_data_buffer(FIDO_VERSION, 1, loc, (size_t)len);
	if (safe_strncmp(fido_url, "https://github.com/pbatard/Fido", 31) != 0) {
		uprintf("WARNING: Download script URL %s is invalid ✗", fido_url);
		safe_free(fido_url);
		goto out;
	}
	if (IsDownloadable(fido_url))
		linux_set_iso_download_available(TRUE);

out:
	safe_free(loc);
	is_active = FALSE;
	return 0;
}

void SetFidoCheck(void)
{
	// Detect if we can use Fido, which depends on:
	// - Powershell being installed
	// - Update check being enabled
	// - URL for the script being reachable
	// NB: Unlike Windows, where PowerShell is always installed, the remote UEFI revocation
	// data (which is retrieved alongside the Fido URL) must still be fetched without it.
	BOOL has_powershell = (GetPowerShellPath() != NULL);

	if (ReadSetting32(SETTING_UPDATE_INTERVAL) <= 0) {
		ubprintf("Notice: The ISO download feature has been deactivated because "
			"'Check for updates' is disabled in your settings.");
		return;
	}

	if (!has_powershell)
		ubprintf("Notice: The ISO download feature has been deactivated because "
			"a compatible PowerShell version was not detected on this system.");

	CreateThread(NULL, 0, CheckForFidoThread, (LPVOID)(uintptr_t)has_powershell, 0, NULL);
}

/*
 * Initial update check setup
 */
BOOL SetUpdateCheck(void)
{
	BOOL enable_updates;
	uint64_t commcheck = GetTickCount64();

	// Test if we can read and write settings. If not, forget it.
	WriteSetting64(SETTING_COMM_CHECK, commcheck);
	if (ReadSetting64(SETTING_COMM_CHECK) != commcheck)
		return FALSE;

	// If the update interval is not set, this is the first time we run so prompt the user
	// NB: The Windows version doesn't prompt when the executable is named "rufus.exe"
	// (without a version). This doesn't apply here, since the name is set by the package.
	if (ReadSetting32(SETTING_UPDATE_INTERVAL) == 0) {
		notification_info more_info;

		more_info.id = IDD_UPDATE_POLICY;
		more_info.callback = UpdateCallback;
		enable_updates = (NotificationEx(MB_ICONQUESTION | MB_YESNO, NULL,
			(UpdateCallback != NULL) ? &more_info : NULL, lmprintf(MSG_004), lmprintf(MSG_005)) == IDYES);
		if (!enable_updates) {
			WriteSetting32(SETTING_UPDATE_INTERVAL, -1);
			return FALSE;
		}
		// If the user hasn't set the interval in the dialog, set to default
		if ( (ReadSetting32(SETTING_UPDATE_INTERVAL) == 0) ||
			 (ReadSetting32(SETTING_UPDATE_INTERVAL) == -1) )
			WriteSetting32(SETTING_UPDATE_INTERVAL, 86400);
	}
	SetFidoCheck();
	return TRUE;
}

/*
 * Run a program (no shell involved) and capture its standard output and error.
 * The program is terminated if the user cancels or if it runs for more than
 * timeout ms. Returns the exit code, or -1 if the program couldn't be run.
 */
extern char** environ;
static int RunAndCapture(const char* const argv[], char** output, DWORD timeout)
{
	static const char* const extra_env[] = {
		// Don't send telemetry, don't check for updates and don't colorize the output
		"POWERSHELL_TELEMETRY_OPTOUT=1", "DOTNET_CLI_TELEMETRY_OPTOUT=1", "POWERSHELL_UPDATECHECK=Off",
		"DOTNET_NOLOGO=1", "NO_COLOR=1", "TERM=dumb",
	};
	posix_spawn_file_actions_t actions;
	char **envp = NULL, *buf = NULL;
	size_t i, j, n, len = 0, size = 0;
	int fds[2] = { -1, -1 }, status, ret = -1;
	pid_t pid = -1;
	ssize_t r;
	struct pollfd pfd;
	ULONGLONG start = GetTickCount64();
	BOOL has_home = FALSE;

	*output = NULL;
	if (pipe2(fds, O_CLOEXEC) != 0)
		return -1;

	// Build the environment
	for (n = 0; environ[n] != NULL; n++);
	envp = calloc(n + ARRAYSIZE(extra_env) + 2, sizeof(char*));
	if (envp == NULL)
		goto out;
	for (i = 0, j = 0; i < n; i++) {
		size_t k;
		for (k = 0; k < ARRAYSIZE(extra_env); k++) {
			const char* eq = strchr(extra_env[k], '=');
			if (strncmp(environ[i], extra_env[k], eq - extra_env[k] + 1) == 0)
				break;
		}
		if (k < ARRAYSIZE(extra_env))
			continue;
		if (strncmp(environ[i], "HOME=", 5) == 0)
			has_home = TRUE;
		envp[j++] = environ[i];
	}
	for (i = 0; i < ARRAYSIZE(extra_env); i++)
		envp[j++] = (char*)extra_env[i];
	// PowerShell needs a home directory for its configuration
	if (!has_home)
		envp[j++] = (geteuid() == 0) ? "HOME=/root" : "HOME=/tmp";
	envp[j] = NULL;

	posix_spawn_file_actions_init(&actions);
	posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
	posix_spawn_file_actions_adddup2(&actions, fds[1], STDOUT_FILENO);
	posix_spawn_file_actions_adddup2(&actions, fds[1], STDERR_FILENO);
	status = posix_spawn(&pid, argv[0], &actions, NULL, (char* const*)argv, envp);
	posix_spawn_file_actions_destroy(&actions);
	close(fds[1]);
	fds[1] = -1;
	if (status != 0) {
		errno = status;
		uprintf("Could not run '%s': %s", argv[0], strerror(status));
		pid = -1;
		goto out;
	}

	pfd.fd = fds[0];
	pfd.events = POLLIN;
	for (;;) {
		if (IS_ERROR(ErrorStatus) || (GetTickCount64() - start > timeout)) {
			if (!IS_ERROR(ErrorStatus))
				uprintf("'%s' took too long to complete - Aborting", argv[0]);
			kill(pid, SIGKILL);
			break;
		}
		r = poll(&pfd, 1, 250);
		if (r < 0 && errno != EINTR)
			break;
		if (r <= 0)
			continue;
		if (size - len < 4096) {
			char* nbuf;
			if (size >= FIDO_MAX_OUTPUT)
				break;
			nbuf = realloc(buf, size + 64 * KB);
			if (nbuf == NULL)
				break;
			buf = nbuf;
			size += 64 * KB;
		}
		r = read(fds[0], &buf[len], size - len - 1);
		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0)
			break;
		len += (size_t)r;
		buf[len] = 0;
	}
	if (waitpid(pid, &status, 0) == pid)
		ret = WIFEXITED(status) ? WEXITSTATUS(status) : -1;

out:
	if (fds[0] >= 0)
		close(fds[0]);
	if (fds[1] >= 0)
		close(fds[1]);
	free(envp);
	if (buf == NULL)
		buf = calloc(1, 1);
	*output = buf;
	return ret;
}

/* Remove ANSI escape sequences and carriage returns from a line, in place */
static void SanitizeLine(char* line)
{
	char *s = line, *d = line;
	while (*s != 0) {
		if (*s == '\x1b') {
			s++;
			if (*s == '[') {
				s++;
				while (*s != 0 && !(*s >= 0x40 && *s <= 0x7e))
					s++;
				if (*s != 0)
					s++;
			} else if (*s != 0) {
				s++;
			}
			continue;
		}
		if (*s != '\r')
			*d++ = *s;
		s++;
	}
	*d = 0;
	trim(line);
}

/*
 * Parse the output of a Fido command line query:
 * - list entries are reported as " - <entry>" lines
 * - the download URL is reported on its own line
 * The last non empty line is also returned, for error reporting.
 */
static void ParseFidoOutput(char* output, StrArray* items, char** url, char* last_line, size_t last_line_size)
{
	char *line, *save = NULL;

	if (url != NULL)
		*url = NULL;
	last_line[0] = 0;
	for (line = strtok_r(output, "\n", &save); line != NULL; line = strtok_r(NULL, "\n", &save)) {
		SanitizeLine(line);
		if (line[0] == 0)
			continue;
		snprintf(last_line, last_line_size, "%s", line);
		uprintf("  %s", line);
		if (line[0] == '-' && line[1] == ' ') {
			char* item = &line[2];
			trim(item);
			if (items != NULL && item[0] != 0)
				StrArrayAdd(items, item, TRUE);
		} else if (url != NULL && *url == NULL && _strnicmp(line, "https://", 8) == 0) {
			*url = strdup(line);
		}
	}
}

/*
 * Fido matches the values it is given as follows (see Fido.ps1's command line mode):
 * - Version (-Win), Edition (-Ed) and Architecture (-Arch): PowerShell -match (regexp)
 * - Release (-Rel): StartsWith()
 * - Language (-Lang): -match, after Fido escaped the parentheses itself
 * We pass the values as they were listed, encoded accordingly, and should a query
 * still fail (e.g. if a later Fido changes these), retry with the part of the value
 * that precedes any regular expression special character, which suits both operators.
 */
enum fido_value_encoding {
	FVE_RAW = 0,
	FVE_ESCAPE,				// Escape all regexp special characters
	FVE_ESCAPE_NO_PARENS,	// Same, except for parentheses (that Fido escapes for -Lang)
	FVE_PREFIX,				// Part of the value that precedes any special character
};
static const char* fido_regex_special = "\\^$|?*+()[]{}";
#define FIDO_NB_ENCODINGS 2
static const int fido_encoding[5][FIDO_NB_ENCODINGS] = {
	{ FVE_ESCAPE, FVE_PREFIX },				// -Win
	{ FVE_RAW, FVE_PREFIX },				// -Rel
	{ FVE_ESCAPE, FVE_PREFIX },				// -Ed
	{ FVE_ESCAPE_NO_PARENS, FVE_PREFIX },	// -Lang
	{ FVE_ESCAPE, FVE_PREFIX },				// -Arch
};

static char* GetFidoValue(const char* value, int encoding)
{
	char* r;
	size_t i, j, len = strlen(value);

	switch (encoding) {
	case FVE_ESCAPE:
	case FVE_ESCAPE_NO_PARENS:
		r = calloc(2 * len + 1, 1);
		if (r == NULL)
			return NULL;
		for (i = 0, j = 0; i < len; i++) {
			if ((strchr(fido_regex_special, value[i]) != NULL || value[i] == '.') &&
				!(encoding == FVE_ESCAPE_NO_PARENS && (value[i] == '(' || value[i] == ')')))
				r[j++] = '\\';
			r[j++] = value[i];
		}
		return r;
	case FVE_PREFIX:
		r = strdup(value);
		if (r != NULL) {
			r[strcspn(r, fido_regex_special)] = 0;
			trim(r);
		}
		return r;
	default:
		return strdup(value);
	}
}

/*
 * Fido's command line mode refuses to run on platforms other than Windows 8 or later
 * ("Windows 7 and non Windows platforms are too much of a liability"), on account of
 * the platform version it gets from the .NET runtime. Nothing in that mode actually
 * depends on Windows (it only issues web requests), so we report a Windows 10 platform.
 * NB: This is applied, as a constant replacement, to the script we validated.
 */
static const char fido_platform_check[] = "$winver = Get-Platform-Version";
static const char fido_platform_patch[] = "$winver = 10.0	# Rufus for Linux: Platform check doesn't apply to PowerShell Core";

static BOOL WriteFidoScript(int fd, const BYTE* script, size_t len)
{
	const BYTE* p = memmem(script, len, fido_platform_check, sizeof(fido_platform_check) - 1);
	size_t offset;

	if (p == NULL) {
		uprintf("WARNING: Could not locate the platform check of the download script");
		return (write(fd, script, len) == (ssize_t)len);
	}
	offset = (size_t)(p - script);
	return (write(fd, script, offset) == (ssize_t)offset) &&
		(write(fd, fido_platform_patch, sizeof(fido_platform_patch) - 1) == (ssize_t)(sizeof(fido_platform_patch) - 1)) &&
		(write(fd, &script[offset + sizeof(fido_platform_check) - 1], len - offset - (sizeof(fido_platform_check) - 1)) ==
			(ssize_t)(len - offset - (sizeof(fido_platform_check) - 1)));
}

/*
 * Present a list of choices to the user. Since the selection dialog can only
 * display a limited number of entries, long lists (e.g. languages) are paged,
 * with a last "…" entry used to display the next page.
 * Returns the index of the selected entry, or -1 if the user cancelled.
 */
static int SelectFidoEntry(const char* message, StrArray* items)
{
	const uint32_t max_entries = IDC_SELECTION_CHOICEMAX - IDC_SELECTION_CHOICE1;
	uint32_t start = 0, count, i;
	BOOL paged = (items->Index > max_entries);
	int r;

	if (items->Index == 0)
		return -1;
	// Nothing to choose from
	if (items->Index == 1)
		return 0;
	for (;;) {
		selection_dialog_options_t selection = { 0 };
		count = paged ? min(max_entries - 1, items->Index - start) : items->Index;
		StrArrayCreate(&selection.choices, max_entries);
		for (i = 0; i < count; i++)
			StrArrayAdd(&selection.choices, items->String[start + i], TRUE);
		if (paged)
			StrArrayAdd(&selection.choices, "…", TRUE);
		r = SelectionDialog(lmprintf(MSG_149), (char*)message, &selection);
		StrArrayDestroy(&selection.choices);
		if (r <= 0)
			return -1;
		r = _log2(r);
		if (r < 0)
			return -1;
		if (paged && (uint32_t)r == count) {
			// Next page (or back to the first one)
			start += count;
			if (start >= items->Index)
				start = 0;
			continue;
		}
		return (int)(start + (uint32_t)r);
	}
}

#define FIDO_NB_PARAMS 5

/*
 * Run a Fido command line query, with the values selected so far.
 * If 'list_param' is a valid index, the values for that parameter are listed in
 * 'items'. Otherwise the download URL is requested and returned in 'url'.
 */
static BOOL QueryFido(const char* pwsh, const char* script, char** selected, int* mode, int nb_selected,
	int list_param, StrArray* items, char** url, char* last_line, size_t last_line_size)
{
	static const char* param_name[FIDO_NB_PARAMS] = { "-Win", "-Rel", "-Ed", "-Lang", "-Arch" };
	const char* argv[32];
	char *values[FIDO_NB_PARAMS] = { 0 }, *output = NULL;
	int i, n = 0, exit_code;
	BOOL r = FALSE;

	argv[n++] = pwsh;
	argv[n++] = "-NonInteractive";
	argv[n++] = "-NoProfile";
	argv[n++] = "-ExecutionPolicy";
	argv[n++] = "Bypass";
	argv[n++] = "-File";
	argv[n++] = script;
	for (i = 0; i < nb_selected; i++) {
		values[i] = GetFidoValue(selected[i], fido_encoding[i][mode[i]]);
		if (values[i] == NULL)
			goto out;
		argv[n++] = param_name[i];
		argv[n++] = values[i];
	}
	if (list_param >= 0 && list_param < FIDO_NB_PARAMS) {
		argv[n++] = param_name[list_param];
		argv[n++] = "List";
	} else {
		argv[n++] = "-GetUrl";
	}
	argv[n++] = "-PlatformArch";
	argv[n++] = GetArchName(GetApplicationArch());
	argv[n] = NULL;

	PrintInfo(0, MSG_148);
	exit_code = RunAndCapture(argv, &output, FIDO_TIMEOUT);
	uprintf("Exited download script with code: %d", exit_code);
	if (exit_code == -1 && (output == NULL || output[0] == 0))
		goto out;
	ParseFidoOutput(output, items, url, last_line, last_line_size);
	r = (items != NULL) ? (items->Index > 0) : (url != NULL && *url != NULL);

out:
	for (i = 0; i < FIDO_NB_PARAMS; i++)
		free(values[i]);
	free(output);
	return r;
}

/*
 * Download an ISO through Fido
 */
static DWORD WINAPI DownloadISOThread(LPVOID param)
{
	static const int param_msg[FIDO_NB_PARAMS] = { MSG_135, MSG_136, MSG_137, MSG_138, MSG_139 };
	char tmp_dir[MAX_PATH] = { 0 }, script_path[MAX_PATH] = { 0 }, sig_url[256], last_line[256] = "";
	char *url = NULL, *image = NULL, *selected[FIDO_NB_PARAMS] = { 0 };
	const char* pwsh;
	int i, step, idx, mode[FIDO_NB_PARAMS] = { 0 }, fd = -1;
	uint64_t uncompressed_size;
	int64_t size = -1;
	BYTE *compressed = NULL, *sig = NULL;
	DWORD dwExitCode = 99, dwCompressedSize, dwSize;
	StrArray items;
	BOOL r;

	(void)param;
	dialog_showing++;
	StrArrayCreate(&items, 32);

	pwsh = GetPowerShellPath();
	if (pwsh == NULL) {
		uprintf("Could not find PowerShell (pwsh), which is required to download Windows ISOs");
		Notification(MB_OK | MB_ICONERROR, lmprintf(MSG_149), "%s\n\n%s", lmprintf(MSG_145),
			"sudo snap install powershell --classic");
		goto out;
	}

	// If we don't have the script, download it
	if (fido_script == NULL) {
		dwCompressedSize = (DWORD)DownloadToFileOrBuffer(fido_url, NULL, &compressed, hMainDialog, FALSE);
		if (dwCompressedSize == 0)
			goto out;
		static_sprintf(sig_url, "%s.sig", fido_url);
		dwSize = (DWORD)DownloadToFileOrBuffer(sig_url, NULL, &sig, NULL, FALSE);
		if ((dwSize != RSA_SIGNATURE_SIZE) || (!ValidateOpensslSignature(compressed, dwCompressedSize, sig, dwSize))) {
			uprintf("FATAL: Download signature is invalid ✗");
			ErrorStatus = RUFUS_ERROR(APPERR(ERROR_BAD_SIGNATURE));
			SendMessage(hProgress, PBM_SETSTATE, (WPARAM)PBST_ERROR, 0);
			set_taskbar_state(TASKBAR_ERROR);
			safe_free(compressed);
			free(sig);
			goto out;
		}
		free(sig);
		uprintf("Download signature is valid ✓");
		// LZMA alone header: properties (1 byte), dictionary size (4 bytes), uncompressed size (8 bytes)
		uncompressed_size = (dwCompressedSize > 13) ? *((uint64_t*)&compressed[5]) : (uint64_t)-1;
		if ((uncompressed_size < 1 * MB) && (bled_init(0, uprintf, NULL, NULL, NULL, NULL, &ErrorStatus) >= 0)) {
			fido_script = malloc((size_t)uncompressed_size);
			if (fido_script != NULL)
				size = bled_uncompress_from_buffer_to_buffer((const char*)compressed, dwCompressedSize,
					(char*)fido_script, (size_t)uncompressed_size, BLED_COMPRESSION_LZMA);
			bled_exit();
		}
		safe_free(compressed);
		if (size != (int64_t)uncompressed_size) {
			uprintf("FATAL: Could not uncompressed download script");
			safe_free(fido_script);
			ErrorStatus = RUFUS_ERROR(ERROR_INVALID_DATA);
			SendMessage(hProgress, PBM_SETSTATE, (WPARAM)PBST_ERROR, 0);
			set_taskbar_state(TASKBAR_ERROR);
			goto out;
		}
		fido_len = (DWORD)size;
		SendMessage(hProgress, PBM_SETSTATE, (WPARAM)PBST_NORMAL, 0);
		set_taskbar_state(TASKBAR_NORMAL);
		set_taskbar_value(0, MAX_PROGRESS);
		SendMessage(hProgress, PBM_SETPOS, 0, 0);
	}
	PrintInfo(0, MSG_148);

	if_assert_fails((fido_script != NULL) && (fido_len != 0))
		goto out;

	// Write the script in a private directory (only accessible by the user we run as),
	// using a non predictable name, so that it can't be swapped before PowerShell runs it.
	static_sprintf(tmp_dir, "%s/rufus-fido-XXXXXX", "/tmp");
	if (mkdtemp(tmp_dir) == NULL) {
		uprintf("Unable to create directory for download script: %s", strerror(errno));
		tmp_dir[0] = 0;
		goto out;
	}
	static_sprintf(script_path, "%s/Fido.ps1", tmp_dir);
	fd = open(script_path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0400);
	if (fd < 0) {
		uprintf("Unable to create download script '%s': %s", script_path, strerror(errno));
		script_path[0] = 0;
		goto out;
	}
	if (!WriteFidoScript(fd, fido_script, fido_len)) {
		uprintf("Unable to write download script '%s': %s", script_path, strerror(errno));
		goto out;
	}
	close(fd);
	fd = -1;

	// Have the user select the version, release, edition, language and architecture,
	// one after the other, and then retrieve the download URL.
	ErrorStatus = 0;
	for (step = 0; step <= FIDO_NB_PARAMS; step++) {
		for (;;) {
			StrArrayClear(&items);
			safe_free(url);
			r = QueryFido(pwsh, script_path, selected, mode, step, (step < FIDO_NB_PARAMS) ? step : -1,
				(step < FIDO_NB_PARAMS) ? &items : NULL, &url, last_line, sizeof(last_line));
			if (r || IS_ERROR(ErrorStatus))
				break;
			// If the value we last selected contains regexp special characters, try the next way of passing it
			if (step > 0 && strpbrk(selected[step - 1], fido_regex_special) != NULL && mode[step - 1] < FIDO_NB_ENCODINGS - 1) {
				mode[step - 1]++;
				continue;
			}
			break;
		}
		if (IS_ERROR(ErrorStatus))
			goto out;
		if (!r) {
			uprintf("Download script failed: %s", (last_line[0] != 0) ? last_line : "No output");
			Notification(MB_OK | MB_ICONERROR, lmprintf(MSG_149), "%s",
				(last_line[0] != 0) ? last_line : lmprintf(MSG_144));
			goto out;
		}
		if (step == FIDO_NB_PARAMS)
			break;
		idx = SelectFidoEntry(lmprintf(param_msg[step]), &items);
		if (idx < 0)
			goto out;
		selected[step] = strdup(items.String[idx]);
		if (selected[step] == NULL)
			goto out;
		mode[step] = 0;
		uprintf("%s: %s", lmprintf(param_msg[step]), selected[step]);
	}
	dwExitCode = 0;

	{
		EXT_DECL(img_ext, GetShortName(url), __VA_GROUP__("*.iso"), __VA_GROUP__(lmprintf(MSG_036)));
		image = FileDialog(TRUE, NULL, &img_ext, NULL);
		if (image == NULL)
			goto out;
		// Download the ISO and report errors if any
		SendMessage(hMainDialog, UM_PROGRESS_INIT, 0, 0);
		ErrorStatus = 0;
		SendMessage(hMainDialog, UM_TIMER_START, 0, 0);
		if (DownloadToFileOrBuffer(url, image, NULL, hMainDialog, TRUE) == 0) {
			SendMessage(hMainDialog, UM_PROGRESS_EXIT, 0, 0);
			if (SCODE_CODE(ErrorStatus) == ERROR_CANCELLED) {
				uprintf("Download cancelled by user");
				Notification(MB_ICONINFORMATION | MB_CLOSE, lmprintf(MSG_211), lmprintf(MSG_041));
				PrintInfo(0, MSG_211);
			} else {
				Notification(MB_ICONERROR | MB_CLOSE, lmprintf(MSG_194, GetShortName(url)), lmprintf(MSG_043, WindowsErrorString()));
				PrintInfo(0, MSG_212);
			}
		} else {
			// Download was successful => Select and scan the ISO
			image_path = safe_strdup(image);
			PostMessage(hMainDialog, UM_SELECT_ISO, 0, 0);
		}
	}

out:
	if (fd >= 0)
		close(fd);
	if (script_path[0] != '\0')
		unlink(script_path);
	if (tmp_dir[0] != '\0')
		rmdir(tmp_dir);
	for (i = 0; i < FIDO_NB_PARAMS; i++)
		free(selected[i]);
	StrArrayDestroy(&items);
	free(image);
	free(url);
	SendMessage(hMainDialog, UM_ENABLE_CONTROLS, 0, 0);
	dialog_showing--;
	ExitThread(dwExitCode);
}

BOOL DownloadISO(void)
{
	if (CreateThread(NULL, 0, DownloadISOThread, NULL, 0, NULL) == NULL) {
		uprintf("Unable to start Windows ISO download thread");
		ErrorStatus = RUFUS_ERROR(APPERR(ERROR_CANT_START_THREAD));
		SendMessage(hMainDialog, UM_ENABLE_CONTROLS, 0, 0);
		return FALSE;
	}
	return TRUE;
}

/*
 * Check that a URL can be downloaded (status 200 with a non zero Content-Length),
 * without downloading its content.
 * NB: Like on Windows, this issues a GET (rather than a HEAD, which some servers,
 * such as the ones GitHub redirects release downloads to, reject) and stops as
 * soon as the headers have been received.
 */
BOOL IsDownloadable(const char* url)
{
	char errbuf[CURL_ERROR_SIZE];
	struct curl_slist* headers = NULL;
	download_context ctx = { 0 };
	CURLcode res;

	if (url == NULL)
		return FALSE;

	ErrorStatus = 0;
	DownloadStatus = 404;

	ctx.url = url;
	ctx.hFile = INVALID_HANDLE_VALUE;
	ctx.probe_only = TRUE;
	ctx.curl = GetInternetSession(NULL, url, errbuf);
	if (ctx.curl == NULL)
		goto out;

	// Must use "Accept-Encoding: identity" to get the file size
	headers = curl_slist_append(headers, "Accept-Encoding: identity");
	headers = curl_slist_append(headers, "Accept: */*");
	curl_easy_setopt(ctx.curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(ctx.curl, CURLOPT_WRITEFUNCTION, WriteCallback);
	curl_easy_setopt(ctx.curl, CURLOPT_WRITEDATA, &ctx);
	res = curl_easy_perform(ctx.curl);
	if (!ctx.headers_processed) {
		if (res != CURLE_OK) {
			if (res == CURLE_UNSUPPORTED_PROTOCOL && _strnicmp(url, "https://", 8) == 0)
				uprintf("Refusing to follow a redirect from HTTPS to a non HTTPS URL");
			goto out;
		}
		ProcessHeaders(&ctx);
	}

out:
	curl_slist_free_all(headers);
	if (ctx.curl != NULL)
		curl_easy_cleanup(ctx.curl);
	return (DownloadStatus == 200) && (ctx.total_size > 0);
}
