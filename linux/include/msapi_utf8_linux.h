/*
 * Rufus for Linux: replacement for src/msapi_utf8.h
 * Copyright © 2026 Rufus Linux port contributors
 *
 * On Windows, msapi_utf8.h converts UTF-8 strings to UTF-16 and calls the W
 * version of each API. On Linux everything is natively UTF-8, so these simply
 * forward to the POSIX based implementations from linux/compat/compat.c.
 * Paths given with Windows separators ('\') are transparently converted.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */
#pragma once
#ifndef RUFUS_LINUX_MSAPI_UTF8_LINUX_H
#define RUFUS_LINUX_MSAPI_UTF8_LINUX_H

#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <sys/stat.h>

#ifdef __cplusplus
extern "C" {
#endif

#define _LTEXT(txt) L##txt
#define LTEXT(txt) _LTEXT(txt)
#define sfree(p) do {if (p != NULL) {free((void*)(p)); p = NULL;}} while(0)

#define isasciiU(c) isascii((unsigned char)(c))
#define iscntrlU(c) iscntrl((unsigned char)(c))
#define isdigitU(c) isdigit((unsigned char)(c))
#define isspaceU(c) isspace((unsigned char)(c))
#define isxdigitU(c) isxdigit((unsigned char)(c))

/* UTF-8 <-> wchar_t (UTF-32 on Linux) */
char* wchar_to_utf8(const wchar_t* wstr);
wchar_t* utf8_to_wchar(const char* str);
#define wchar_to_utf8_no_alloc(wsrc, dest, dest_size) \
	WideCharToMultiByte(CP_UTF8, 0, wsrc, -1, dest, dest_size, NULL, NULL)
#define utf8_to_wchar_no_alloc(src, wdest, wdest_size) \
	MultiByteToWideChar(CP_UTF8, 0, src, -1, wdest, wdest_size)

/*
 * Returns a newly allocated copy of path, translated for Linux: '\' are converted to '/'
 * and virtual drive letters ("E:\...") are replaced with the matching mount point.
 */
char* linux_path(const char* path);
static inline int utf8_to_wchar_get_size(const char* str)
{
	return (str == NULL) ? 0 : MultiByteToWideChar(CP_UTF8, 0, str, -1, NULL, 0);
}

/* Files */
#define CreateFileU CreateFileA
#define CreateDirectoryU CreateDirectoryA
#define CopyFileU CopyFileA
#define DeleteFileU DeleteFileA
#define PathFileExistsU PathFileExistsA
#define MoveFileU MoveFileA
#define MoveFileExU MoveFileExA
#define GetFileAttributesU GetFileAttributesA
#define SetFileAttributesU SetFileAttributesA
#define GetTempPathU GetTempPathA
#define GetTempFileNameU GetTempFileNameA
#define FindFirstFileU FindFirstFileA
#define FindNextFileU FindNextFileA
#define GetFullPathNameU GetFullPathNameA
#define GetModuleFileNameU GetModuleFileNameA
#define GetSystemDirectoryU GetSystemDirectoryA
#define LoadLibraryU LoadLibraryA
#define FormatMessageU FormatMessageA
#define MessageBoxU MessageBoxA
#define SetWindowTextU SetWindowTextA
#define GetWindowTextU GetWindowTextA
#define GetWindowTextLengthU GetWindowTextLengthA
/* The string is UTF-8: use the 'A' version of messages that have one */
#define SendMessageLU(hCtrl, msg, wParam, str) SendMessageA(hCtrl, ((msg) == SB_SETTEXTW) ? SB_SETTEXTA : (msg), wParam, (LPARAM)(str))
BOOLEAN CreateSymbolicLinkU(const char* lpSymlinkFileName, const char* lpTargetFileName, DWORD dwFlags);
DWORD GetTempDirNameU(char* lpPathName, char* lpPrefixString, UINT uUnique, char* lpTempDirName);
int SHCreateDirectoryExU(HWND hwnd, const char* pszPath, SECURITY_ATTRIBUTES* psa);
int SHDeleteDirectoryExU(HWND hwnd, const char* pszPath, DWORD fFlags);
#define FOF_SILENT 0x0004
#define FOF_NOCONFIRMATION 0x0010
#define FOF_NOERRORUI 0x0400
#define FOF_NO_UI (FOF_SILENT | FOF_NOCONFIRMATION | FOF_NOERRORUI)
typedef WORD FILEOP_FLAGS;
BOOL GetVolumeNameForVolumeMountPointU(LPCSTR lpszVolumeMountPoint, LPSTR lpszVolumeName, DWORD cchBufferLength);
char* PathFindFileNameU(const char* szPath);
char* PathCombineU(char* lpDest, char* lpDir, char* lpFile);
DWORD GetFinalPathNameByHandleU(HANDLE hFile, LPSTR lpszFilePath, DWORD cchFilePath, DWORD dwFlags);
BOOL GetVolumeInformationU(LPCSTR lpRootPathName, LPSTR lpVolumeNameBuffer, DWORD nVolumeNameSize,
	LPDWORD lpVolumeSerialNumber, LPDWORD lpMaximumComponentLength, LPDWORD lpFileSystemFlags,
	LPSTR lpFileSystemNameBuffer, DWORD nFileSystemNameSize);

/* CRT */
FILE* fopenU(const char* filename, const char* mode);
int _openU(const char* filename, int oflag, int pmode);
int _unlinkU(const char* path);
int _accessU(const char* path, int mode);
int _chdirU(const char* dirname);
int _mkdirU(const char* dirname);
int _mkdirExU(const char* dirname);
int _rmdirU(const char* dirname);
char* getenvU(const char* varname);
uint64_t _filesizeU(const char* path);
/* NB: st_mtime is a glibc macro for st_mtim.tv_sec */
struct __stat64 { uint64_t st_size; struct timespec st_mtim; mode_t st_mode; };
int _stat64U(const char* path, struct __stat64* buffer);
char* GetAppDirectoryU(void);
BOOL GetUserNameU(LPSTR lpBuffer, LPDWORD pcbBuffer);

#ifdef __cplusplus
}
#endif

#endif
