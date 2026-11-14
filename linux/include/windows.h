/*
 * Rufus for Linux: minimal Win32 compatibility layer
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

/*
 * This header lets the platform independent parts of Rufus (ISO/UDF scanning and
 * extraction, hashing, bad blocks, boot records, decompression, localization...)
 * be compiled unmodified on Linux. Only what Rufus actually uses is provided, and
 * the semantics follow the Win32 documentation as closely as practical:
 * - HANDLEs are reference objects wrapping a file descriptor, a thread or an event.
 * - All strings are UTF-8 (which is what Rufus uses internally anyway).
 * - GetLastError() maps errno values to the closest Win32 error codes.
 */
#pragma once
#ifndef RUFUS_LINUX_WINDOWS_H
#define RUFUS_LINUX_WINDOWS_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <wchar.h>
#include <wctype.h>
#include <errno.h>
#include <limits.h>
#include <ctype.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <alloca.h>
#include <malloc.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include "wstdio_compat.h"
#define fopen rufus_fopen

#ifdef __cplusplus
extern "C" {
#endif

/* MSVC architecture identifiers (Rufus uses them to select arch specific code) */
#if defined(__x86_64__)
#define _M_AMD64 100
#define _M_X64 100
#elif defined(__i386__)
#define _M_IX86 600
#elif defined(__aarch64__)
#define _M_ARM64 1
#elif defined(__arm__)
#define _M_ARM 7
#endif
#define _STATIC_ASSERT(expr) _Static_assert((expr), #expr)
#define C_ASSERT(expr) _Static_assert((expr), #expr)

/* ------------------------------------------------------------------------- */
/* Calling conventions and compiler-isms                                     */
/* ------------------------------------------------------------------------- */
#define WINAPI
#define APIENTRY
#define CALLBACK
#define WINAPIV
#define STDAPI
#define NTAPI
#define __stdcall
#define __cdecl
#define _stdcall
#define __fastcall
#define DECLSPEC_IMPORT
#define DECLSPEC_NORETURN __attribute__((noreturn))
#define DECLSPEC_ALIGN(x) __attribute__((aligned(x)))
#define FORCEINLINE static inline __attribute__((always_inline))
#ifndef __inline
#define __inline inline
#endif
#define __forceinline inline __attribute__((always_inline))
#define __declspec(x)
#define IN
#define OUT
#define OPTIONAL
#define CONST const
#define VOID void
#define UNREFERENCED_PARAMETER(P) (void)(P)
#define _Inout_
#define _In_
#define _Out_
#define _In_opt_
#define _Out_opt_
#define _Printf_format_string_
#define __analysis_assume(x)
#define __pragma(x)

/* ------------------------------------------------------------------------- */
/* Base types (LP64 Linux vs LLP64 Windows: LONG/ULONG/DWORD are 32-bit)     */
/* ------------------------------------------------------------------------- */
typedef int                 BOOL, *PBOOL, *LPBOOL;
typedef uint8_t             BYTE, *PBYTE, *LPBYTE, UCHAR, *PUCHAR, BOOLEAN, *PBOOLEAN, UINT8, *PUINT8;
typedef char                CHAR, *PCHAR, *LPCH, CCHAR, TCHAR;
typedef wchar_t             WCHAR, *PWCHAR, *LPWCH;
typedef int16_t             SHORT, *PSHORT, INT16;
typedef uint16_t            USHORT, *PUSHORT, WORD, *PWORD, *LPWORD, UINT16, ATOM, LANGID;
typedef int32_t             INT, *PINT, *LPINT, LONG, *PLONG, *LPLONG, INT32, LONG32, HRESULT, NTSTATUS, SCODE;
typedef uint32_t            UINT, *PUINT, ULONG, *PULONG, DWORD, *PDWORD, *LPDWORD, UINT32, ULONG32, DWORD32, COLORREF, LCID, LCTYPE;
typedef int64_t             LONGLONG, *PLONGLONG, INT64, LONG64, __int64;
typedef uint64_t            ULONGLONG, *PULONGLONG, UINT64, ULONG64, DWORD64, *PDWORD64, DWORDLONG;
typedef intptr_t            INT_PTR, *PINT_PTR, LONG_PTR, *PLONG_PTR, SSIZE_T, LPARAM, LRESULT;
typedef uintptr_t           UINT_PTR, *PUINT_PTR, ULONG_PTR, *PULONG_PTR, DWORD_PTR, *PDWORD_PTR, SIZE_T, *PSIZE_T, WPARAM;
typedef float               FLOAT;
typedef double              DOUBLE;
typedef void                *PVOID, *LPVOID;
typedef const void          *LPCVOID, *PCVOID;
typedef char                *LPSTR, *PSTR, *LPTSTR;
typedef const char          *LPCSTR, *PCSTR, *LPCTSTR, *PCTSTR;
typedef wchar_t             *LPWSTR, *PWSTR;
typedef const wchar_t       *LPCWSTR, *PCWSTR;
typedef LONG                *PHRESULT;

/* Opaque handles */
#if defined(RUFUS_COMPAT_INTERNAL)
typedef struct _RUFUS_HANDLE *HANDLE, **PHANDLE, **LPHANDLE;
#else
/* Like on Windows, HANDLEs are opaque pointers (see linux/compat/compat.c) */
typedef void *HANDLE, **PHANDLE, **LPHANDLE;
#endif
typedef void *HMODULE, *HINSTANCE, *HWND, *HDC, *HFONT, *HICON, *HMENU, *HBITMAP, *HBRUSH,
	*HPEN, *HGDIOBJ, *HCURSOR, *HIMAGELIST, *HGLOBAL, *HLOCAL, *HRSRC, *HKEY, *PHKEY,
	*HMONITOR, *HTHEME, *HDEVINFO, *HRGN, *HHOOK, *HDROP, *HWINEVENTHOOK, *HINTERNET,
	*HCRYPTPROV, *HCERTSTORE, *HCRYPTMSG, *HPOWERNOTIFY, *HDEVNOTIFY, *HACCEL, *HPALETTE,
	*HKL, *HDESK, *HWINSTA, *HTASK, *HMETAFILE, *HENHMETAFILE, *HPROPSHEETPAGE, *HTREEITEM,
	*HCONV, *HSZ, *HDDEDATA, *HGLRC, *HCOLORSPACE, *HUMPD, *HDWP, *HSTRING;
typedef HMODULE HINST;

typedef union _LARGE_INTEGER {
	struct { DWORD LowPart; LONG HighPart; };
	struct { DWORD LowPart; LONG HighPart; } u;
	LONGLONG QuadPart;
} LARGE_INTEGER, *PLARGE_INTEGER;

typedef union _ULARGE_INTEGER {
	struct { DWORD LowPart; DWORD HighPart; };
	struct { DWORD LowPart; DWORD HighPart; } u;
	ULONGLONG QuadPart;
} ULARGE_INTEGER, *PULARGE_INTEGER;

typedef struct _FILETIME {
	DWORD dwLowDateTime;
	DWORD dwHighDateTime;
} FILETIME, *PFILETIME, *LPFILETIME;

typedef struct _SYSTEMTIME {
	WORD wYear;
	WORD wMonth;
	WORD wDayOfWeek;
	WORD wDay;
	WORD wHour;
	WORD wMinute;
	WORD wSecond;
	WORD wMilliseconds;
} SYSTEMTIME, *PSYSTEMTIME, *LPSYSTEMTIME;

typedef struct _GUID {
	uint32_t Data1;
	uint16_t Data2;
	uint16_t Data3;
	uint8_t  Data4[8];
} GUID, *PGUID, *LPGUID, IID, CLSID, UUID, FMTID;
typedef const GUID *LPCGUID, *REFGUID, *REFIID, *REFCLSID;
#ifndef DEFINE_GUID
#define DEFINE_GUID(name, l, w1, w2, b1, b2, b3, b4, b5, b6, b7, b8) \
	static const GUID name __attribute__((unused)) = { l, w1, w2, { b1, b2, b3, b4, b5, b6, b7, b8 } }
#endif
#define IsEqualGUID(a, b) (memcmp((a), (b), sizeof(GUID)) == 0)

typedef struct tagRECT { LONG left, top, right, bottom; } RECT, *PRECT, *LPRECT;
typedef const RECT *LPCRECT;
typedef struct tagPOINT { LONG x, y; } POINT, *PPOINT, *LPPOINT;
typedef struct tagSIZE { LONG cx, cy; } SIZE, *PSIZE, *LPSIZE;

typedef struct _SECURITY_ATTRIBUTES {
	DWORD nLength;
	LPVOID lpSecurityDescriptor;
	BOOL bInheritHandle;
} SECURITY_ATTRIBUTES, *PSECURITY_ATTRIBUTES, *LPSECURITY_ATTRIBUTES;

typedef struct _OVERLAPPED {
	ULONG_PTR Internal;
	ULONG_PTR InternalHigh;
	union {
		struct { DWORD Offset; DWORD OffsetHigh; };
		PVOID Pointer;
	};
	HANDLE hEvent;
} OVERLAPPED, *LPOVERLAPPED;

typedef DWORD (WINAPI *PTHREAD_START_ROUTINE)(LPVOID lpThreadParameter);
typedef PTHREAD_START_ROUTINE LPTHREAD_START_ROUTINE;
typedef INT_PTR (CALLBACK *DLGPROC)(HWND, UINT, WPARAM, LPARAM);
typedef LRESULT (CALLBACK *WNDPROC)(HWND, UINT, WPARAM, LPARAM);
typedef INT_PTR (WINAPI *FARPROC)(void);
typedef void (CALLBACK *TIMERPROC)(HWND, UINT, UINT_PTR, DWORD);

typedef struct _CRITICAL_SECTION { void *opaque; } CRITICAL_SECTION, *LPCRITICAL_SECTION;

/* ------------------------------------------------------------------------- */
/* Base constants and macros                                                 */
/* ------------------------------------------------------------------------- */
#ifndef TRUE
#define TRUE  1
#endif
#ifndef FALSE
#define FALSE 0
#endif
#ifndef NULL
#define NULL ((void*)0)
#endif
/* Linux paths can be much longer than Windows' 260 characters */
#define MAX_PATH            4096
#define _MAX_PATH           MAX_PATH
#define _MAX_FNAME          256
#define _MAX_EXT            256
#define _MAX_DIR            MAX_PATH
#define _MAX_DRIVE          3
#define INFINITE            0xFFFFFFFF
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
#define INVALID_FILE_SIZE   ((DWORD)0xFFFFFFFF)
#define INVALID_SET_FILE_POINTER ((DWORD)-1)
#define INVALID_FILE_ATTRIBUTES ((DWORD)-1)
#define MAXDWORD            0xFFFFFFFF
#define MAXLONGLONG         (0x7fffffffffffffffLL)
#define MAXWORD             0xFFFF
#define MAXBYTE             0xFF
#define MAXUINT32           UINT32_MAX
#define MAXUINT64           UINT64_MAX
#define MAXULONG32          UINT32_MAX
#define MAXLONG             0x7FFFFFFF
#define LOWORD(l)           ((WORD)(((DWORD_PTR)(l)) & 0xffff))
#define HIWORD(l)           ((WORD)((((DWORD_PTR)(l)) >> 16) & 0xffff))
#define LOBYTE(w)           ((BYTE)(((DWORD_PTR)(w)) & 0xff))
#define HIBYTE(w)           ((BYTE)((((DWORD_PTR)(w)) >> 8) & 0xff))
#define MAKEWORD(a, b)      ((WORD)(((BYTE)(((DWORD_PTR)(a)) & 0xff)) | ((WORD)((BYTE)(((DWORD_PTR)(b)) & 0xff))) << 8))
#define MAKELONG(a, b)      ((LONG)(((WORD)(((DWORD_PTR)(a)) & 0xffff)) | ((DWORD)((WORD)(((DWORD_PTR)(b)) & 0xffff))) << 16))
#define MAKEWPARAM(l, h)    ((WPARAM)(DWORD)MAKELONG(l, h))
#define MAKELPARAM(l, h)    ((LPARAM)(DWORD)MAKELONG(l, h))
#define MAKELANGID(p, s)    ((((WORD)(s)) << 10) | (WORD)(p))
#define PRIMARYLANGID(lgid) ((WORD)(lgid) & 0x3ff)
#define SUBLANGID(lgid)     ((WORD)(lgid) >> 10)
#define MAKELCID(lgid, srtid) ((DWORD)((((DWORD)((WORD)(srtid))) << 16) | ((DWORD)((WORD)(lgid)))))
#define LANGIDFROMLCID(lcid) ((WORD)(lcid))
#define MAKEINTRESOURCEA(i) ((LPSTR)((ULONG_PTR)((WORD)(i))))
#define MAKEINTRESOURCEW(i) ((LPWSTR)((ULONG_PTR)((WORD)(i))))
#define MAKEINTRESOURCE     MAKEINTRESOURCEA
#define IS_INTRESOURCE(_r)  ((((ULONG_PTR)(_r)) >> 16) == 0)
#define LANG_NEUTRAL        0x00
#define LANG_ENGLISH        0x09
#define SUBLANG_NEUTRAL     0x00
#define SUBLANG_DEFAULT     0x01
#define SUBLANG_ENGLISH_US  0x01
#define LOCALE_USER_DEFAULT 0x0400
#define LOCALE_SYSTEM_DEFAULT 0x0800
#define CP_ACP              0
#define CP_OEMCP            1
#define CP_UTF8             65001
#define MB_ERR_INVALID_CHARS 0x08
#define WC_ERR_INVALID_CHARS 0x80
#define _TRUNCATE           ((size_t)-1)
#define TEXT(s)             s
#define _T(s)               s
#define ANYSIZE_ARRAY       1

#ifndef min
#define min(a, b)           (((a) < (b)) ? (a) : (b))
#endif
#ifndef max
#define max(a, b)           (((a) > (b)) ? (a) : (b))
#endif

/* ------------------------------------------------------------------------- */
/* Error codes                                                               */
/* ------------------------------------------------------------------------- */
#define NO_ERROR                        0L
#define ERROR_SUCCESS                   0L
#define ERROR_INVALID_FUNCTION          1L
#define ERROR_FILE_NOT_FOUND            2L
#define ERROR_PATH_NOT_FOUND            3L
#define ERROR_TOO_MANY_OPEN_FILES       4L
#define ERROR_ACCESS_DENIED             5L
#define ERROR_INVALID_HANDLE            6L
#define ERROR_NOT_ENOUGH_MEMORY         8L
#define ERROR_BAD_FORMAT                11L
#define ERROR_INVALID_ACCESS            12L
#define ERROR_INVALID_DATA              13L
#define ERROR_OUTOFMEMORY               14L
#define ERROR_INVALID_DRIVE             15L
#define ERROR_NO_MORE_FILES             18L
#define ERROR_WRITE_PROTECT             19L
#define ERROR_BAD_UNIT                  20L
#define ERROR_NOT_READY                 21L
#define ERROR_BAD_COMMAND               22L
#define ERROR_CRC                       23L
#define ERROR_BAD_LENGTH                24L
#define ERROR_SEEK                      25L
#define ERROR_NOT_DOS_DISK              26L
#define ERROR_SECTOR_NOT_FOUND          27L
#define ERROR_WRITE_FAULT               29L
#define ERROR_READ_FAULT                30L
#define ERROR_GEN_FAILURE               31L
#define ERROR_SHARING_VIOLATION         32L
#define ERROR_LOCK_VIOLATION            33L
#define ERROR_WRONG_DISK                34L
#define ERROR_HANDLE_EOF                38L
#define ERROR_HANDLE_DISK_FULL          39L
#define ERROR_NOT_SUPPORTED             50L
#define ERROR_DEV_NOT_EXIST             55L
#define ERROR_FILE_EXISTS               80L
#define ERROR_CANNOT_MAKE               82L
#define ERROR_INVALID_PARAMETER         87L
#define ERROR_DISK_CHANGE               107L
#define ERROR_BROKEN_PIPE               109L
#define ERROR_OPEN_FAILED               110L
#define ERROR_BUFFER_OVERFLOW           111L
#define ERROR_DISK_FULL                 112L
#define ERROR_CALL_NOT_IMPLEMENTED      120L
#define ERROR_INSUFFICIENT_BUFFER       122L
#define ERROR_INVALID_NAME              123L
#define ERROR_INVALID_LEVEL             124L
#define ERROR_NO_VOLUME_LABEL           125L
#define ERROR_MOD_NOT_FOUND             126L
#define ERROR_PROC_NOT_FOUND            127L
#define ERROR_NEGATIVE_SEEK             131L
#define ERROR_DIR_NOT_EMPTY             145L
#define ERROR_LABEL_TOO_LONG            154L
#define ERROR_BAD_PATHNAME              161L
#define ERROR_BUSY                      170L
#define ERROR_ALREADY_EXISTS            183L
#define ERROR_BAD_EXE_FORMAT            193L
#define ERROR_ENVVAR_NOT_FOUND          203L
#define ERROR_FILENAME_EXCED_RANGE      206L
#define ERROR_FILE_TOO_LARGE            223L
#define ERROR_BAD_PIPE                  230L
#define ERROR_NO_DATA                   232L
#define ERROR_MORE_DATA                 234L
#define WAIT_TIMEOUT                    258L
#define ERROR_NO_MORE_ITEMS             259L
#define ERROR_DIRECTORY                 267L
#define ERROR_PARTIAL_COPY              299L
#define ERROR_INVALID_ADDRESS           487L
#define ERROR_ARITHMETIC_OVERFLOW       534L
#define ERROR_ELEVATION_REQUIRED        740L
#define ERROR_OPERATION_ABORTED         995L
#define ERROR_IO_INCOMPLETE             996L
#define ERROR_IO_PENDING                997L
#define ERROR_NOACCESS                  998L
#define ERROR_CANTOPEN                  1011L
#define ERROR_CANTREAD                  1012L
#define ERROR_CANTWRITE                 1013L
#define ERROR_NOT_FOUND                 1168L
#define ERROR_IO_DEVICE                 1117L
#define ERROR_MEDIA_CHANGED             1110L
#define ERROR_NO_MEDIA_IN_DRIVE         1112L
#define ERROR_INVALID_BLOCK_LENGTH      1106L
#define ERROR_DEVICE_NOT_CONNECTED      1167L
#define ERROR_CANCELLED                 1223L
#define ERROR_PRIVILEGE_NOT_HELD        1314L
#define ERROR_INVALID_SECURITY_DESCR    1338L
#define ERROR_NO_SYSTEM_RESOURCES       1450L
#define ERROR_TIMEOUT                   1460L
#define ERROR_INCORRECT_SIZE            1462L
#define ERROR_DEVICE_REMOVED            1617L
#define ERROR_UNRECOGNIZED_MEDIA        1785L
#define ERROR_RESOURCE_DATA_NOT_FOUND   1812L
#define ERROR_RESOURCE_TYPE_NOT_FOUND   1813L
#define ERROR_RESOURCE_NAME_NOT_FOUND   1814L
#define ERROR_NOT_ENOUGH_QUOTA          1816L
#define ERROR_CANT_ACCESS_FILE          1920L
#define ERROR_CANT_RESOLVE_FILENAME     1921L
#define ERROR_INVALID_STATE             5023L
#define ERROR_DEVICE_NOT_AVAILABLE      4319L
#define ERROR_INVALID_DRIVE_OBJECT      4321L
#define ERROR_DEVICE_IN_USE             2404L
#define ERROR_UNHANDLED_EXCEPTION       574L
#define ERROR_INVALID_USER_BUFFER       1784L
#define ERROR_INTERNET_TIMEOUT          12002L
#define ERROR_INTERNET_NAME_NOT_RESOLVED 12007L
#define ERROR_INTERNET_CANNOT_CONNECT   12029L
#define ERROR_INTERNET_CONNECTION_ABORTED 12030L
#define ERROR_INTERNET_CONNECTION_RESET 12031L
#define ERROR_INTERNET_SEC_CERT_ERRORS  12055L
#define ERROR_HTTP_INVALID_SERVER_RESPONSE 12152L

#define S_OK                            ((HRESULT)0L)
#define S_FALSE                         ((HRESULT)1L)
#define E_FAIL                          ((HRESULT)0x80004005L)
#define E_NOTIMPL                       ((HRESULT)0x80004001L)
#define E_OUTOFMEMORY                   ((HRESULT)0x8007000EL)
#define E_INVALIDARG                    ((HRESULT)0x80070057L)
#define E_ACCESSDENIED                  ((HRESULT)0x80070005L)
#define E_POINTER                       ((HRESULT)0x80004003L)
#define E_UNEXPECTED                    ((HRESULT)0x8000FFFFL)
#define SUCCEEDED(hr)                   (((HRESULT)(hr)) >= 0)
#define FAILED(hr)                      (((HRESULT)(hr)) < 0)
#define FACILITY_WIN32                  7
#define FACILITY_STORAGE                3
#define FACILITY_ITF                    4
#define HRESULT_FROM_WIN32(x)           ((HRESULT)(x) <= 0 ? ((HRESULT)(x)) : ((HRESULT) (((x) & 0x0000FFFF) | (FACILITY_WIN32 << 16) | 0x80000000)))
#define HRESULT_CODE(hr)                ((hr) & 0xFFFF)
#define SCODE_CODE(sc)                  ((sc) & 0xFFFF)
#define SCODE_FACILITY(sc)              (((sc) >> 16) & 0x1fff)
#define SCODE_SEVERITY(sc)              (((sc) >> 31) & 0x1)
#define HRESULT_FACILITY(hr)            (((hr) >> 16) & 0x1fff)
#define IS_ERROR(Status)                (((unsigned long)(Status)) >> 31 == 1)
#define MAKE_HRESULT(sev, fac, code)    ((HRESULT) (((unsigned long)(sev)<<31) | ((unsigned long)(fac)<<16) | ((unsigned long)(code))))
#define APPLICATION_ERROR_MASK          0x20000000
#define ERROR_SEVERITY_SUCCESS          0x00000000
#define ERROR_SEVERITY_INFORMATIONAL    0x40000000
#define ERROR_SEVERITY_WARNING          0x80000000
#define ERROR_SEVERITY_ERROR            0xC0000000
#define NT_SUCCESS(Status)              (((NTSTATUS)(Status)) >= 0)
#define STATUS_SUCCESS                  ((NTSTATUS)0x00000000L)
#define STATUS_UNSUCCESSFUL             ((NTSTATUS)0xC0000001L)
#define STATUS_NOT_IMPLEMENTED          ((NTSTATUS)0xC0000002L)
#define STATUS_PROCEDURE_NOT_FOUND      ((NTSTATUS)0xC000007AL)
#define STATUS_INFO_LENGTH_MISMATCH     ((NTSTATUS)0xC0000004L)
#define STATUS_NO_MEMORY                ((NTSTATUS)0xC0000017L)
#define STATUS_ACCESS_VIOLATION         ((NTSTATUS)0xC0000005L)
#define STATUS_IN_PAGE_ERROR            ((NTSTATUS)0xC0000006L)
#define STATUS_TIMEOUT                  ((DWORD)0x00000102L)
#define EXCEPTION_IN_PAGE_ERROR         STATUS_IN_PAGE_ERROR

/* ------------------------------------------------------------------------- */
/* Files                                                                     */
/* ------------------------------------------------------------------------- */
#define GENERIC_READ                    0x80000000
#define GENERIC_WRITE                   0x40000000
#define GENERIC_EXECUTE                 0x20000000
#define GENERIC_ALL                     0x10000000
#define FILE_READ_DATA                  0x0001
#define FILE_WRITE_DATA                 0x0002
#define FILE_APPEND_DATA                0x0004
#define FILE_READ_ATTRIBUTES            0x0080
#define FILE_WRITE_ATTRIBUTES           0x0100
#define FILE_GENERIC_READ               0x00120089
#define FILE_GENERIC_WRITE              0x00120116
#define FILE_ALL_ACCESS                 0x001F01FF
#define SYNCHRONIZE                     0x00100000
#define DELETE                          0x00010000
#define FILE_SHARE_READ                 0x00000001
#define FILE_SHARE_WRITE                0x00000002
#define FILE_SHARE_DELETE               0x00000004
#define CREATE_NEW                      1
#define CREATE_ALWAYS                   2
#define OPEN_EXISTING                   3
#define OPEN_ALWAYS                     4
#define TRUNCATE_EXISTING               5
#define FILE_ATTRIBUTE_READONLY         0x00000001
#define FILE_ATTRIBUTE_HIDDEN           0x00000002
#define FILE_ATTRIBUTE_SYSTEM           0x00000004
#define FILE_ATTRIBUTE_DIRECTORY        0x00000010
#define FILE_ATTRIBUTE_ARCHIVE          0x00000020
#define FILE_ATTRIBUTE_DEVICE           0x00000040
#define FILE_ATTRIBUTE_NORMAL           0x00000080
#define FILE_ATTRIBUTE_TEMPORARY        0x00000100
#define FILE_ATTRIBUTE_SPARSE_FILE      0x00000200
#define FILE_ATTRIBUTE_REPARSE_POINT    0x00000400
#define FILE_ATTRIBUTE_COMPRESSED       0x00000800
#define FILE_ATTRIBUTE_OFFLINE          0x00001000
#define FILE_ATTRIBUTE_NOT_CONTENT_INDEXED 0x00002000
#define FILE_ATTRIBUTE_ENCRYPTED        0x00004000
#define FILE_FLAG_WRITE_THROUGH         0x80000000
#define FILE_FLAG_OVERLAPPED            0x40000000
#define FILE_FLAG_NO_BUFFERING          0x20000000
#define FILE_FLAG_RANDOM_ACCESS         0x10000000
#define FILE_FLAG_SEQUENTIAL_SCAN       0x08000000
#define FILE_FLAG_DELETE_ON_CLOSE       0x04000000
#define FILE_FLAG_BACKUP_SEMANTICS      0x02000000
#define FILE_FLAG_POSIX_SEMANTICS       0x01000000
#define FILE_FLAG_OPEN_REPARSE_POINT    0x00200000
#define FILE_BEGIN                      0
#define FILE_CURRENT                    1
#define FILE_END                        2
#define MOVEFILE_REPLACE_EXISTING       0x00000001
#define MOVEFILE_COPY_ALLOWED           0x00000002
#define MOVEFILE_DELAY_UNTIL_REBOOT     0x00000004
#define MOVEFILE_WRITE_THROUGH          0x00000008
#define SYMBOLIC_LINK_FLAG_DIRECTORY    0x1
#define SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE 0x2
#define FILE_TYPE_UNKNOWN               0x0000
#define FILE_TYPE_DISK                  0x0001
#define FILE_TYPE_CHAR                  0x0002
#define FILE_TYPE_PIPE                  0x0003
#define DRIVE_UNKNOWN                   0
#define DRIVE_NO_ROOT_DIR               1
#define DRIVE_REMOVABLE                 2
#define DRIVE_FIXED                     3
#define DRIVE_REMOTE                    4
#define DRIVE_CDROM                     5
#define DRIVE_RAMDISK                   6

typedef struct _BY_HANDLE_FILE_INFORMATION {
	DWORD dwFileAttributes;
	FILETIME ftCreationTime;
	FILETIME ftLastAccessTime;
	FILETIME ftLastWriteTime;
	DWORD dwVolumeSerialNumber;
	DWORD nFileSizeHigh;
	DWORD nFileSizeLow;
	DWORD nNumberOfLinks;
	DWORD nFileIndexHigh;
	DWORD nFileIndexLow;
} BY_HANDLE_FILE_INFORMATION, *PBY_HANDLE_FILE_INFORMATION, *LPBY_HANDLE_FILE_INFORMATION;

typedef struct _WIN32_FIND_DATAA {
	DWORD dwFileAttributes;
	FILETIME ftCreationTime;
	FILETIME ftLastAccessTime;
	FILETIME ftLastWriteTime;
	DWORD nFileSizeHigh;
	DWORD nFileSizeLow;
	DWORD dwReserved0;
	DWORD dwReserved1;
	CHAR cFileName[MAX_PATH];
	CHAR cAlternateFileName[14];
} WIN32_FIND_DATAA, *PWIN32_FIND_DATAA, *LPWIN32_FIND_DATAA, WIN32_FIND_DATA, *LPWIN32_FIND_DATA;

/* ------------------------------------------------------------------------- */
/* Synchronization and threads                                               */
/* ------------------------------------------------------------------------- */
#define WAIT_OBJECT_0                   0x00000000L
#define WAIT_ABANDONED                  0x00000080L
#define WAIT_ABANDONED_0                WAIT_ABANDONED
#define WAIT_FAILED                     ((DWORD)0xFFFFFFFF)
#define STILL_ACTIVE                    259
#define THREAD_PRIORITY_LOWEST          (-2)
#define THREAD_PRIORITY_BELOW_NORMAL    (-1)
#define THREAD_PRIORITY_NORMAL          0
#define THREAD_PRIORITY_ABOVE_NORMAL    1
#define THREAD_PRIORITY_HIGHEST         2
#define THREAD_PRIORITY_TIME_CRITICAL   15
#define THREAD_PRIORITY_IDLE            (-15)
#define CREATE_SUSPENDED                0x00000004
#define STATUS_PENDING                  ((DWORD)0x00000103L)

/* ------------------------------------------------------------------------- */
/* Windowing constants that the portable code references                     */
/* ------------------------------------------------------------------------- */
#define WM_NULL                         0x0000
#define WM_CREATE                       0x0001
#define WM_DESTROY                      0x0002
#define WM_SIZE                         0x0005
#define WM_SETFOCUS                     0x0007
#define WM_SETTEXT                      0x000C
#define WM_GETTEXT                      0x000D
#define WM_PAINT                        0x000F
#define WM_CLOSE                        0x0010
#define WM_QUIT                         0x0012
#define WM_SETFONT                      0x0030
#define WM_NOTIFY                       0x004E
#define WM_INITDIALOG                   0x0110
#define WM_COMMAND                      0x0111
#define WM_TIMER                        0x0113
#define WM_DEVICECHANGE                 0x0219
#define WM_USER                         0x0400
#define WM_APP                          0x8000
#define BM_SETCHECK                     0x00F1
#define BM_GETCHECK                     0x00F0
#define BST_UNCHECKED                   0x0000
#define BST_CHECKED                     0x0001
#define BST_INDETERMINATE               0x0002
#define CB_ERR                          (-1)
#define CBN_SELCHANGE                   1
#define PBM_SETPOS                      (WM_USER+2)
#define PBM_SETRANGE                    (WM_USER+1)
#define PBM_SETSTATE                    (WM_USER+16)
#define PBM_SETMARQUEE                  (WM_USER+10)
#define PBST_NORMAL                     0x0001
#define PBST_ERROR                      0x0002
#define PBST_PAUSED                     0x0003
#define SB_SETTEXTA                     (WM_USER+1)
#define SB_SETTEXTW                     (WM_USER+11)
#define SBT_OWNERDRAW                   0x1000
#define IDOK                            1
#define IDCANCEL                        2
#define IDABORT                         3
#define IDRETRY                         4
#define IDIGNORE                        5
#define IDYES                           6
#define IDNO                            7
#define IDCLOSE                         8
#define IDHELP                          9
#define IDTRYAGAIN                      10
#define IDCONTINUE                      11
#define MB_OK                           0x00000000L
#define MB_OKCANCEL                     0x00000001L
#define MB_ABORTRETRYIGNORE             0x00000002L
#define MB_YESNOCANCEL                  0x00000003L
#define MB_YESNO                        0x00000004L
#define MB_RETRYCANCEL                  0x00000005L
#define MB_CANCELTRYCONTINUE            0x00000006L
#define MB_ICONHAND                     0x00000010L
#define MB_ICONQUESTION                 0x00000020L
#define MB_ICONEXCLAMATION              0x00000030L
#define MB_ICONASTERISK                 0x00000040L
#define MB_ICONWARNING                  MB_ICONEXCLAMATION
#define MB_ICONERROR                    MB_ICONHAND
#define MB_ICONINFORMATION              MB_ICONASTERISK
#define MB_ICONSTOP                     MB_ICONHAND
#define MB_TOPMOST                      0x00040000L
#define MB_RIGHT                        0x00080000L
#define MB_RTLREADING                   0x00100000L
#define MB_SETFOREGROUND                0x00010000L
#define MB_SYSTEMMODAL                  0x00001000L
#define MB_TASKMODAL                    0x00002000L
#define MB_APPLMODAL                    0x00000000L
#define MB_DEFBUTTON1                   0x00000000L
#define MB_DEFBUTTON2                   0x00000100L
#define MB_ICONMASK                     0x000000F0L
#define MB_TYPEMASK                     0x0000000FL
#define SW_HIDE                         0
#define SW_SHOW                         5
#define SW_SHOWNORMAL                   1
#define DT_CALCRECT                     0x00000400
#define DT_LEFT                         0x00000000
#define DT_SINGLELINE                   0x00000020
#define DT_NOPREFIX                     0x00000800
#define DT_WORDBREAK                    0x00000010
#define FW_NORMAL                       400
#define FW_BOLD                         700
#define LOGPIXELSX                      88
#define LOGPIXELSY                      90

typedef struct tagMSG { HWND hwnd; UINT message; WPARAM wParam; LPARAM lParam; DWORD time; POINT pt; } MSG, *LPMSG;

/* ------------------------------------------------------------------------- */
/* PE image definitions (used by the UEFI bootloader validation code)        */
/* ------------------------------------------------------------------------- */
#define IMAGE_FILE_MACHINE_UNKNOWN      0
#define IMAGE_FILE_MACHINE_I386         0x014c
#define IMAGE_FILE_MACHINE_IA64         0x0200
#define IMAGE_FILE_MACHINE_AMD64        0x8664
#define IMAGE_FILE_MACHINE_ARM          0x01c0
#define IMAGE_FILE_MACHINE_ARMNT        0x01c4
#define IMAGE_FILE_MACHINE_THUMB        0x01c2
#define IMAGE_FILE_MACHINE_ARM64        0xaa64
#define IMAGE_FILE_MACHINE_RISCV64      0x5064
#define IMAGE_FILE_MACHINE_LOONGARCH64  0x6264
#define IMAGE_FILE_MACHINE_EBC          0x0ebc
#define IMAGE_DOS_SIGNATURE             0x5A4D
#define IMAGE_NT_SIGNATURE              0x00004550
#define IMAGE_NT_OPTIONAL_HDR32_MAGIC   0x10b
#define IMAGE_NT_OPTIONAL_HDR64_MAGIC   0x20b
#define IMAGE_NUMBEROF_DIRECTORY_ENTRIES 16
#define IMAGE_SIZEOF_SHORT_NAME         8
#define IMAGE_DIRECTORY_ENTRY_EXPORT    0
#define IMAGE_DIRECTORY_ENTRY_IMPORT    1
#define IMAGE_DIRECTORY_ENTRY_RESOURCE  2
#define IMAGE_DIRECTORY_ENTRY_SECURITY  4
#define IMAGE_DIRECTORY_ENTRY_BASERELOC 5
#define IMAGE_DIRECTORY_ENTRY_DEBUG     6
#define IMAGE_SUBSYSTEM_EFI_APPLICATION 10
#define IMAGE_SUBSYSTEM_EFI_BOOT_SERVICE_DRIVER 11
#define IMAGE_SUBSYSTEM_EFI_RUNTIME_DRIVER 12
#define IMAGE_RESOURCE_NAME_IS_STRING   0x80000000
#define IMAGE_RESOURCE_DATA_IS_DIRECTORY 0x80000000
#define IMAGE_FIRST_SECTION(ntheader) ((PIMAGE_SECTION_HEADER) ((ULONG_PTR)(ntheader) + \
	offsetof(IMAGE_NT_HEADERS, OptionalHeader) + ((ntheader))->FileHeader.SizeOfOptionalHeader))

#pragma pack(push, 2)
typedef struct _IMAGE_DOS_HEADER {
	WORD e_magic; WORD e_cblp; WORD e_cp; WORD e_crlc; WORD e_cparhdr; WORD e_minalloc; WORD e_maxalloc;
	WORD e_ss; WORD e_sp; WORD e_csum; WORD e_ip; WORD e_cs; WORD e_lfarlc; WORD e_ovno; WORD e_res[4];
	WORD e_oemid; WORD e_oeminfo; WORD e_res2[10]; LONG e_lfanew;
} IMAGE_DOS_HEADER, *PIMAGE_DOS_HEADER;
#pragma pack(pop)

typedef struct _IMAGE_FILE_HEADER {
	WORD Machine; WORD NumberOfSections; DWORD TimeDateStamp; DWORD PointerToSymbolTable;
	DWORD NumberOfSymbols; WORD SizeOfOptionalHeader; WORD Characteristics;
} IMAGE_FILE_HEADER, *PIMAGE_FILE_HEADER;

typedef struct _IMAGE_DATA_DIRECTORY { DWORD VirtualAddress; DWORD Size; } IMAGE_DATA_DIRECTORY, *PIMAGE_DATA_DIRECTORY;

typedef struct _IMAGE_OPTIONAL_HEADER32 {
	WORD Magic; BYTE MajorLinkerVersion; BYTE MinorLinkerVersion; DWORD SizeOfCode; DWORD SizeOfInitializedData;
	DWORD SizeOfUninitializedData; DWORD AddressOfEntryPoint; DWORD BaseOfCode; DWORD BaseOfData; DWORD ImageBase;
	DWORD SectionAlignment; DWORD FileAlignment; WORD MajorOperatingSystemVersion; WORD MinorOperatingSystemVersion;
	WORD MajorImageVersion; WORD MinorImageVersion; WORD MajorSubsystemVersion; WORD MinorSubsystemVersion;
	DWORD Win32VersionValue; DWORD SizeOfImage; DWORD SizeOfHeaders; DWORD CheckSum; WORD Subsystem;
	WORD DllCharacteristics; DWORD SizeOfStackReserve; DWORD SizeOfStackCommit; DWORD SizeOfHeapReserve;
	DWORD SizeOfHeapCommit; DWORD LoaderFlags; DWORD NumberOfRvaAndSizes;
	IMAGE_DATA_DIRECTORY DataDirectory[IMAGE_NUMBEROF_DIRECTORY_ENTRIES];
} IMAGE_OPTIONAL_HEADER32, *PIMAGE_OPTIONAL_HEADER32;

typedef struct _IMAGE_OPTIONAL_HEADER64 {
	WORD Magic; BYTE MajorLinkerVersion; BYTE MinorLinkerVersion; DWORD SizeOfCode; DWORD SizeOfInitializedData;
	DWORD SizeOfUninitializedData; DWORD AddressOfEntryPoint; DWORD BaseOfCode; ULONGLONG ImageBase;
	DWORD SectionAlignment; DWORD FileAlignment; WORD MajorOperatingSystemVersion; WORD MinorOperatingSystemVersion;
	WORD MajorImageVersion; WORD MinorImageVersion; WORD MajorSubsystemVersion; WORD MinorSubsystemVersion;
	DWORD Win32VersionValue; DWORD SizeOfImage; DWORD SizeOfHeaders; DWORD CheckSum; WORD Subsystem;
	WORD DllCharacteristics; ULONGLONG SizeOfStackReserve; ULONGLONG SizeOfStackCommit; ULONGLONG SizeOfHeapReserve;
	ULONGLONG SizeOfHeapCommit; DWORD LoaderFlags; DWORD NumberOfRvaAndSizes;
	IMAGE_DATA_DIRECTORY DataDirectory[IMAGE_NUMBEROF_DIRECTORY_ENTRIES];
} IMAGE_OPTIONAL_HEADER64, *PIMAGE_OPTIONAL_HEADER64;

typedef struct _IMAGE_NT_HEADERS64 {
	DWORD Signature; IMAGE_FILE_HEADER FileHeader; IMAGE_OPTIONAL_HEADER64 OptionalHeader;
} IMAGE_NT_HEADERS64, *PIMAGE_NT_HEADERS64;
typedef struct _IMAGE_NT_HEADERS {
	DWORD Signature; IMAGE_FILE_HEADER FileHeader; IMAGE_OPTIONAL_HEADER32 OptionalHeader;
} IMAGE_NT_HEADERS32, *PIMAGE_NT_HEADERS32;
typedef IMAGE_NT_HEADERS64 IMAGE_NT_HEADERS, *PIMAGE_NT_HEADERS;
typedef IMAGE_OPTIONAL_HEADER64 IMAGE_OPTIONAL_HEADER, *PIMAGE_OPTIONAL_HEADER;

typedef struct _IMAGE_SECTION_HEADER {
	BYTE Name[IMAGE_SIZEOF_SHORT_NAME];
	union { DWORD PhysicalAddress; DWORD VirtualSize; } Misc;
	DWORD VirtualAddress; DWORD SizeOfRawData; DWORD PointerToRawData; DWORD PointerToRelocations;
	DWORD PointerToLinenumbers; WORD NumberOfRelocations; WORD NumberOfLinenumbers; DWORD Characteristics;
} IMAGE_SECTION_HEADER, *PIMAGE_SECTION_HEADER;

typedef struct _IMAGE_RESOURCE_DIRECTORY {
	DWORD Characteristics; DWORD TimeDateStamp; WORD MajorVersion; WORD MinorVersion;
	WORD NumberOfNamedEntries; WORD NumberOfIdEntries;
} IMAGE_RESOURCE_DIRECTORY, *PIMAGE_RESOURCE_DIRECTORY;

typedef struct _IMAGE_RESOURCE_DIRECTORY_ENTRY {
	union {
		struct { DWORD NameOffset:31; DWORD NameIsString:1; };
		DWORD Name;
		WORD Id;
	};
	union {
		DWORD OffsetToData;
		struct { DWORD OffsetToDirectory:31; DWORD DataIsDirectory:1; };
	};
} IMAGE_RESOURCE_DIRECTORY_ENTRY, *PIMAGE_RESOURCE_DIRECTORY_ENTRY;

typedef struct _IMAGE_RESOURCE_DIR_STRING_U { WORD Length; uint16_t NameString[1]; } IMAGE_RESOURCE_DIR_STRING_U, *PIMAGE_RESOURCE_DIR_STRING_U;
typedef struct _IMAGE_RESOURCE_DATA_ENTRY { DWORD OffsetToData; DWORD Size; DWORD CodePage; DWORD Reserved; } IMAGE_RESOURCE_DATA_ENTRY, *PIMAGE_RESOURCE_DATA_ENTRY;

typedef struct _IMAGE_EXPORT_DIRECTORY {
	DWORD Characteristics; DWORD TimeDateStamp; WORD MajorVersion; WORD MinorVersion; DWORD Name; DWORD Base;
	DWORD NumberOfFunctions; DWORD NumberOfNames; DWORD AddressOfFunctions; DWORD AddressOfNames; DWORD AddressOfNameOrdinals;
} IMAGE_EXPORT_DIRECTORY, *PIMAGE_EXPORT_DIRECTORY;

typedef struct _IMAGE_DEBUG_DIRECTORY {
	DWORD Characteristics; DWORD TimeDateStamp; WORD MajorVersion; WORD MinorVersion; DWORD Type;
	DWORD SizeOfData; DWORD AddressOfRawData; DWORD PointerToRawData;
} IMAGE_DEBUG_DIRECTORY, *PIMAGE_DEBUG_DIRECTORY;

typedef struct _IMAGE_BASE_RELOCATION { DWORD VirtualAddress; DWORD SizeOfBlock; } IMAGE_BASE_RELOCATION, *PIMAGE_BASE_RELOCATION;

#define WIN_CERT_TYPE_PKCS_SIGNED_DATA  0x0002
#define WIN_CERT_REVISION_2_0           0x0200

/* ------------------------------------------------------------------------- */
/* Partitioning definitions shared by drive.h / format code                  */
/* ------------------------------------------------------------------------- */
typedef enum _PARTITION_STYLE {
	PARTITION_STYLE_MBR = 0,
	PARTITION_STYLE_GPT = 1,
	PARTITION_STYLE_RAW = 2
} PARTITION_STYLE;

typedef enum _MEDIA_TYPE {
	Unknown = 0,
	RemovableMedia = 11,
	FixedMedia = 12
} MEDIA_TYPE, *PMEDIA_TYPE;

typedef struct _DISK_GEOMETRY {
	LARGE_INTEGER Cylinders;
	MEDIA_TYPE MediaType;
	DWORD TracksPerCylinder;
	DWORD SectorsPerTrack;
	DWORD BytesPerSector;
} DISK_GEOMETRY, *PDISK_GEOMETRY;

typedef struct _DISK_GEOMETRY_EX {
	DISK_GEOMETRY Geometry;
	LARGE_INTEGER DiskSize;
	BYTE Data[1];
} DISK_GEOMETRY_EX, *PDISK_GEOMETRY_EX;

typedef struct _PARTITION_INFORMATION_MBR {
	BYTE PartitionType;
	BOOLEAN BootIndicator;
	BOOLEAN RecognizedPartition;
	DWORD HiddenSectors;
	GUID PartitionId;
} PARTITION_INFORMATION_MBR, *PPARTITION_INFORMATION_MBR;

typedef struct _PARTITION_INFORMATION_GPT {
	GUID PartitionType;
	GUID PartitionId;
	DWORD64 Attributes;
	WCHAR Name[36];
} PARTITION_INFORMATION_GPT, *PPARTITION_INFORMATION_GPT;

typedef struct _PARTITION_INFORMATION_EX {
	PARTITION_STYLE PartitionStyle;
	LARGE_INTEGER StartingOffset;
	LARGE_INTEGER PartitionLength;
	DWORD PartitionNumber;
	BOOLEAN RewritePartition;
	BOOLEAN IsServicePartition;
	union {
		PARTITION_INFORMATION_MBR Mbr;
		PARTITION_INFORMATION_GPT Gpt;
	};
} PARTITION_INFORMATION_EX, *PPARTITION_INFORMATION_EX;

typedef struct _DRIVE_LAYOUT_INFORMATION_MBR { DWORD Signature; DWORD CheckSum; } DRIVE_LAYOUT_INFORMATION_MBR;
typedef struct _DRIVE_LAYOUT_INFORMATION_GPT {
	GUID DiskId; LARGE_INTEGER StartingUsableOffset; LARGE_INTEGER UsableLength; DWORD MaxPartitionCount;
} DRIVE_LAYOUT_INFORMATION_GPT;

typedef struct _DRIVE_LAYOUT_INFORMATION_EX {
	DWORD PartitionStyle;
	DWORD PartitionCount;
	union {
		DRIVE_LAYOUT_INFORMATION_MBR Mbr;
		DRIVE_LAYOUT_INFORMATION_GPT Gpt;
	};
	PARTITION_INFORMATION_EX PartitionEntry[1];
} DRIVE_LAYOUT_INFORMATION_EX, *PDRIVE_LAYOUT_INFORMATION_EX;

#define PARTITION_ENTRY_UNUSED          0x00
#define PARTITION_FAT_12                0x01
#define PARTITION_FAT_16                0x04
#define PARTITION_EXTENDED              0x05
#define PARTITION_HUGE                  0x06
#define PARTITION_IFS                   0x07
#define PARTITION_FAT32                 0x0B
#define PARTITION_FAT32_XINT13          0x0C
#define PARTITION_XINT13                0x0E
#define PARTITION_XINT13_EXTENDED       0x0F
#define PARTITION_LINUX                 0x83
#define PARTITION_MSFT_RECOVERY         0x27


/* ------------------------------------------------------------------------- */
/* Storage definitions referenced by drive.h                                 */
/* ------------------------------------------------------------------------- */
typedef DWORD DEVICE_TYPE;
typedef struct _DISK_EXTENT {
	DWORD DiskNumber;
	LARGE_INTEGER StartingOffset;
	LARGE_INTEGER ExtentLength;
} DISK_EXTENT, *PDISK_EXTENT;
#define CTL_CODE(DeviceType, Function, Method, Access) (((DeviceType) << 16) | ((Access) << 14) | ((Function) << 2) | (Method))
#define METHOD_BUFFERED                 0
#define FILE_ANY_ACCESS                 0
#define FILE_READ_ACCESS                1
#define FILE_WRITE_ACCESS               2
#define FILE_DEVICE_DISK                0x00000007
#define FILE_DEVICE_FILE_SYSTEM         0x00000009
#define FILE_DEVICE_MASS_STORAGE        0x0000002d
#define FSCTL_LOCK_VOLUME               CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 6, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define FSCTL_UNLOCK_VOLUME             CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 7, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define FSCTL_DISMOUNT_VOLUME           CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 8, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define FSCTL_ALLOW_EXTENDED_DASD_IO    CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 32, 3, FILE_ANY_ACCESS)
#define FILE_DEVICE_CD_ROM              0x00000002
#define FILE_DEVICE_DVD                 0x00000033
#define IOCTL_DISK_BASE                 FILE_DEVICE_DISK
#define IOCTL_STORAGE_BASE              FILE_DEVICE_MASS_STORAGE
#define IOCTL_VOLUME_BASE               ((DWORD)'V')
#define IOCTL_DISK_GET_DRIVE_GEOMETRY   CTL_CODE(IOCTL_DISK_BASE, 0x0000, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_DISK_GET_PARTITION_INFO   CTL_CODE(IOCTL_DISK_BASE, 0x0001, METHOD_BUFFERED, FILE_READ_ACCESS)
#define IOCTL_DISK_GET_DRIVE_GEOMETRY_EX CTL_CODE(IOCTL_DISK_BASE, 0x0028, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_DISK_GET_PARTITION_INFO_EX CTL_CODE(IOCTL_DISK_BASE, 0x0012, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_DISK_GET_DRIVE_LAYOUT_EX  CTL_CODE(IOCTL_DISK_BASE, 0x0014, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_DISK_SET_DRIVE_LAYOUT_EX  CTL_CODE(IOCTL_DISK_BASE, 0x0015, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define IOCTL_DISK_CREATE_DISK          CTL_CODE(IOCTL_DISK_BASE, 0x0016, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define IOCTL_DISK_GET_LENGTH_INFO      CTL_CODE(IOCTL_DISK_BASE, 0x0017, METHOD_BUFFERED, FILE_READ_ACCESS)
#define IOCTL_DISK_UPDATE_PROPERTIES    CTL_CODE(IOCTL_DISK_BASE, 0x0050, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_STORAGE_CHECK_VERIFY      CTL_CODE(IOCTL_STORAGE_BASE, 0x0200, METHOD_BUFFERED, FILE_READ_ACCESS)
#define IOCTL_STORAGE_GET_DEVICE_NUMBER CTL_CODE(IOCTL_STORAGE_BASE, 0x0420, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS CTL_CODE(IOCTL_VOLUME_BASE, 0, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define FSCTL_QUERY_PERSISTENT_VOLUME_STATE CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 143, METHOD_BUFFERED, FILE_ANY_ACCESS)

typedef struct _PARTITION_INFORMATION {
	LARGE_INTEGER StartingOffset;
	LARGE_INTEGER PartitionLength;
	DWORD HiddenSectors;
	DWORD PartitionNumber;
	BYTE PartitionType;
	BOOLEAN BootIndicator;
	BOOLEAN RecognizedPartition;
	BOOLEAN RewritePartition;
} PARTITION_INFORMATION, *PPARTITION_INFORMATION;

typedef struct _STORAGE_DEVICE_NUMBER {
	DEVICE_TYPE DeviceType;
	DWORD DeviceNumber;
	DWORD PartitionNumber;
} STORAGE_DEVICE_NUMBER, *PSTORAGE_DEVICE_NUMBER;

typedef struct _GET_LENGTH_INFORMATION { LARGE_INTEGER Length; } GET_LENGTH_INFORMATION, *PGET_LENGTH_INFORMATION;

typedef struct _CREATE_DISK_MBR { DWORD Signature; } CREATE_DISK_MBR, *PCREATE_DISK_MBR;
typedef struct _CREATE_DISK_GPT { GUID DiskId; DWORD MaxPartitionCount; } CREATE_DISK_GPT, *PCREATE_DISK_GPT;
typedef struct _CREATE_DISK {
	PARTITION_STYLE PartitionStyle;
	union {
		CREATE_DISK_MBR Mbr;
		CREATE_DISK_GPT Gpt;
	};
} CREATE_DISK, *PCREATE_DISK;

#define GPT_ATTRIBUTE_PLATFORM_REQUIRED          0x0000000000000001ULL
#define GPT_BASIC_DATA_ATTRIBUTE_NO_DRIVE_LETTER 0x8000000000000000ULL
#define GPT_BASIC_DATA_ATTRIBUTE_HIDDEN          0x4000000000000000ULL
#define GPT_BASIC_DATA_ATTRIBUTE_SHADOW_COPY     0x2000000000000000ULL
#define GPT_BASIC_DATA_ATTRIBUTE_READ_ONLY       0x1000000000000000ULL
static const GUID GUID_NULL __attribute__((unused)) = { 0, 0, 0, { 0, 0, 0, 0, 0, 0, 0, 0 } };

/* ------------------------------------------------------------------------- */
/* Misc. constants the shared code references                                */
/* ------------------------------------------------------------------------- */
#define PBS_SMOOTH                      0x01
#define PBS_MARQUEE                     0x08
#define GWL_STYLE                       (-16)
#define FORMAT_MESSAGE_ALLOCATE_BUFFER  0x00000100
#define FORMAT_MESSAGE_IGNORE_INSERTS   0x00000200
#define FORMAT_MESSAGE_FROM_STRING      0x00000400
#define FORMAT_MESSAGE_FROM_HMODULE     0x00000800
#define FORMAT_MESSAGE_FROM_SYSTEM      0x00001000
#define FORMAT_MESSAGE_MAX_WIDTH_MASK   0x000000FF
#define ERROR_INVALID_IMAGE_HASH        577L
#define ERROR_NOT_SAME_DEVICE           17L
#define ERROR_NO_UNICODE_TRANSLATION    1113L
#define ERROR_PARTITION_FAILURE         1105L
#define ERROR_CANNOT_COPY               266L
#define ERROR_INSTALL_FAILURE           1603L
#define ERROR_NO_VOLUME_ID              1173L
#define ERROR_NOT_ENOUGH_SERVER_MEMORY  1130L
#define ERROR_BAD_DEVICE_PATH           330L
#define ERROR_DEVICE_HARDWARE_ERROR     483L
#define ERROR_FILE_CORRUPT              1392L
#define ERROR_DISK_CORRUPT              1393L
#define ERROR_NOT_DOS_DISK              26L
#define ERROR_INVALID_DRIVE_LETTER      ERROR_INVALID_DRIVE
#define ERROR_FUNCTION_FAILED           1627L
#define ERROR_SXS_SECTION_NOT_FOUND     14004L
#define ERROR_NO_MATCH                  1169L
#define ERROR_FILE_INVALID              1006L
#define ERROR_DISK_OPERATION_FAILED     1127L
#define ERROR_FLOPPY_BAD_REGISTERS      1125L
#define ERROR_NO_RANGES_PROCESSED       312L
#define ERROR_NOT_ALLOWED_ON_SYSTEM_FILE 313L
#define ERROR_DEVICE_ALREADY_REMEMBERED 1202L
#define ERROR_DEVICE_REINITIALIZATION_NEEDED 1164L
#define ERROR_UNRECOGNIZED_VOLUME       1005L
#define SEC_E_INVALID_TOKEN             ((HRESULT)0x80090308L)
#define TRUST_E_NOSIGNATURE             ((HRESULT)0x800B0100L)
#define TRUST_E_TIME_STAMP              ((HRESULT)0x80096005L)
#define CRYPT_E_BAD_MSG                 ((HRESULT)0x8009200DL)
#define _SH_DENYRD                      0x30

#if defined(__x86_64__) || defined(__i386__)
#include <mm_malloc.h>
#else
static inline void* _mm_malloc(size_t size, size_t align)
{
	void* p = NULL;
	if (align < sizeof(void*))
		align = sizeof(void*);
	return (posix_memalign(&p, align, size) == 0) ? p : NULL;
}
static inline void _mm_free(void* p) { free(p); }
#endif
static inline int strncpy_s(char* dst, size_t size, const char* src, size_t count)
{
	size_t n;
	if (dst == NULL || size == 0)
		return EINVAL;
	n = (src == NULL) ? 0 : strnlen(src, (count == _TRUNCATE) ? size - 1 : count);
	if (n > size - 1)
		n = size - 1;
	memcpy(dst, src, n);
	dst[n] = 0;
	return 0;
}


/* ------------------------------------------------------------------------- */
/* More error codes                                                          */
/* ------------------------------------------------------------------------- */
#define ERROR_INVALID_BLOCK             9L
#define ERROR_OBJECT_IN_LIST            5011L
#define ERROR_BAD_NETPATH               53L
#define ERROR_NOT_OWNER                 288L

/* ------------------------------------------------------------------------- */
/* Registry: settings are always stored in an INI file on Linux, so these    */
/* only ever report that keys don't exist.                                   */
/* ------------------------------------------------------------------------- */
#define HKEY_CLASSES_ROOT               ((HKEY)(ULONG_PTR)0x80000000)
#define HKEY_CURRENT_USER               ((HKEY)(ULONG_PTR)0x80000001)
#define HKEY_LOCAL_MACHINE              ((HKEY)(ULONG_PTR)0x80000002)
#define HKEY_USERS                      ((HKEY)(ULONG_PTR)0x80000003)
#define KEY_QUERY_VALUE                 0x0001
#define KEY_SET_VALUE                   0x0002
#define KEY_CREATE_SUB_KEY              0x0004
#define KEY_ENUMERATE_SUB_KEYS          0x0008
#define KEY_READ                        0x20019
#define KEY_WRITE                       0x20006
#define KEY_ALL_ACCESS                  0xF003F
#define KEY_WOW64_64KEY                 0x0100
#define REG_NONE                        0
#define REG_SZ                          1
#define REG_EXPAND_SZ                   2
#define REG_BINARY                      3
#define REG_DWORD                       4
#define REG_MULTI_SZ                    7
#define REG_QWORD                       11
#define REG_OPTION_NON_VOLATILE         0
#define REG_CREATED_NEW_KEY             1
#define REG_OPENED_EXISTING_KEY         2
#define RRF_RT_REG_SZ                   0x00000002
#define RRF_RT_REG_DWORD                0x00000010
#define RRF_RT_REG_QWORD                0x00000040
#define RRF_RT_ANY                      0x0000ffff
typedef DWORD REGSAM;
static inline LONG RegOpenKeyExA(HKEY k, LPCSTR s, DWORD o, REGSAM r, HKEY* p) { (void)k; (void)s; (void)o; (void)r; if (p) *p = NULL; return ERROR_FILE_NOT_FOUND; }
static inline LONG RegCreateKeyExA(HKEY k, LPCSTR s, DWORD r0, LPSTR c, DWORD o, REGSAM r, LPSECURITY_ATTRIBUTES a, HKEY* p, LPDWORD d)
	{ (void)k; (void)s; (void)r0; (void)c; (void)o; (void)r; (void)a; (void)d; if (p) *p = NULL; return ERROR_ACCESS_DENIED; }
static inline LONG RegQueryValueExA(HKEY k, LPCSTR n, LPDWORD r, LPDWORD t, LPBYTE d, LPDWORD s) { (void)k; (void)n; (void)r; (void)t; (void)d; (void)s; return ERROR_FILE_NOT_FOUND; }
static inline LONG RegGetValueA(HKEY k, LPCSTR s, LPCSTR v, DWORD f, LPDWORD t, PVOID d, LPDWORD c) { (void)k; (void)s; (void)v; (void)f; (void)t; (void)d; (void)c; return ERROR_FILE_NOT_FOUND; }
static inline LONG RegSetValueExA(HKEY k, LPCSTR n, DWORD r, DWORD t, const BYTE* d, DWORD s) { (void)k; (void)n; (void)r; (void)t; (void)d; (void)s; return ERROR_ACCESS_DENIED; }
static inline LONG RegDeleteValueA(HKEY k, LPCSTR n) { (void)k; (void)n; return ERROR_FILE_NOT_FOUND; }
static inline LONG RegDeleteKeyA(HKEY k, LPCSTR n) { (void)k; (void)n; return ERROR_FILE_NOT_FOUND; }
static inline LONG RegDeleteTreeA(HKEY k, LPCSTR n) { (void)k; (void)n; return ERROR_FILE_NOT_FOUND; }
static inline LONG RegCloseKey(HKEY k) { (void)k; return ERROR_SUCCESS; }
static inline LONG RegLoadKeyA(HKEY k, LPCSTR s, LPCSTR f) { (void)k; (void)s; (void)f; return ERROR_NOT_SUPPORTED; }
static inline LONG RegUnLoadKeyA(HKEY k, LPCSTR s) { (void)k; (void)s; return ERROR_NOT_SUPPORTED; }
static inline LONG RegEnumKeyExA(HKEY k, DWORD i, LPSTR n, LPDWORD l, LPDWORD r, LPSTR c, LPDWORD cl, PFILETIME t)
	{ (void)k; (void)i; (void)n; (void)l; (void)r; (void)c; (void)cl; (void)t; return ERROR_NO_MORE_ITEMS; }
#define RegOpenKeyEx RegOpenKeyExA
#define RegCreateKeyEx RegCreateKeyExA
#define RegQueryValueEx RegQueryValueExA
#define RegSetValueEx RegSetValueExA
#define RegDeleteValue RegDeleteValueA
#define RegDeleteKey RegDeleteKeyA

/* ------------------------------------------------------------------------- */
/* Keyboard layout and code page (see linux/core/locale_linux.c)             */
/* ------------------------------------------------------------------------- */
#define KL_NAMELENGTH                   9
#define LOCALE_IDEFAULTCODEPAGE         0x0000000B
#define LOCALE_IDEFAULTANSICODEPAGE     0x00001004
#define LOCALE_RETURN_NUMBER            0x20000000
#define LOCALE_SISO639LANGNAME          0x00000059
#define LOCALE_SISO3166CTRYNAME         0x0000005A
#define LOCALE_SNAME                    0x0000005c
BOOL GetKeyboardLayoutNameA(LPSTR pwszKLID);
LANGID GetSystemDefaultLangID(void);
UINT GetOEMCP(void);
UINT GetACP(void);
/* There are no Windows language packs to enumerate on Linux */
#define EnumUILanguages(proc, flags, param) ((void)(proc), (void)(param), FALSE)
#define MUI_LANGUAGE_ID 0x4

/* ------------------------------------------------------------------------- */
/* Overlapped I/O (our I/O always completes synchronously)                   */
/* ------------------------------------------------------------------------- */
BOOL GetOverlappedResultEx(HANDLE hFile, LPOVERLAPPED lpOverlapped, LPDWORD lpNumberOfBytesTransferred,
	DWORD dwMilliseconds, BOOL bAlertable);
BOOL HasOverlappedIoCompleted_(LPOVERLAPPED lpOverlapped);
#define HasOverlappedIoCompleted(o) HasOverlappedIoCompleted_(o)

/* ------------------------------------------------------------------------- */
/* Icons (only the structures are needed)                                    */
/* ------------------------------------------------------------------------- */
typedef struct tagBITMAPINFOHEADER {
	DWORD biSize; LONG biWidth; LONG biHeight; WORD biPlanes; WORD biBitCount; DWORD biCompression;
	DWORD biSizeImage; LONG biXPelsPerMeter; LONG biYPelsPerMeter; DWORD biClrUsed; DWORD biClrImportant;
} BITMAPINFOHEADER, *PBITMAPINFOHEADER;
typedef struct tagRGBQUAD { BYTE rgbBlue; BYTE rgbGreen; BYTE rgbRed; BYTE rgbReserved; } RGBQUAD;

/* ------------------------------------------------------------------------- */
/* Control model: the Linux front-end mirrors the state of the controls that */
/* the shared code reads (see linux/ui/winctl.c). Safe from any thread.      */
/* ------------------------------------------------------------------------- */
int ComboBox_GetCurSel(HWND hCtrl);
int ComboBox_SetCurSel(HWND hCtrl, int index);
LRESULT ComboBox_GetItemData(HWND hCtrl, int index);
int ComboBox_SetItemData(HWND hCtrl, int index, LRESULT data);
int ComboBox_AddString(HWND hCtrl, const char* str);
#define ComboBox_AddStringU ComboBox_AddString
int ComboBox_ResetContent(HWND hCtrl);
int ComboBox_GetCount(HWND hCtrl);
int ComboBox_GetLBTextU(HWND hCtrl, int index, char* buf, size_t buf_size);
int Button_GetCheck(HWND hCtrl);
void Button_SetCheck(HWND hCtrl, int check);
int GetWindowTextW(HWND hWnd, LPWSTR lpString, int nMaxCount);


typedef LONG LSTATUS;

/* Time zones (see linux/core/locale_linux.c for the IANA -> Windows mapping) */
#define TIME_ZONE_ID_UNKNOWN            0
#define TIME_ZONE_ID_STANDARD           1
#define TIME_ZONE_ID_DAYLIGHT           2
#define TIME_ZONE_ID_INVALID            ((DWORD)0xFFFFFFFF)
typedef struct _DYNAMIC_TIME_ZONE_INFORMATION {
	LONG Bias;
	WCHAR StandardName[32];
	SYSTEMTIME StandardDate;
	LONG StandardBias;
	WCHAR DaylightName[32];
	SYSTEMTIME DaylightDate;
	LONG DaylightBias;
	WCHAR TimeZoneKeyName[128];
	BOOLEAN DynamicDaylightTimeDisabled;
} DYNAMIC_TIME_ZONE_INFORMATION, *PDYNAMIC_TIME_ZONE_INFORMATION;
DWORD GetDynamicTimeZoneInformation(PDYNAMIC_TIME_ZONE_INFORMATION pTimeZoneInformation);

/* Volumes */
BOOL GetVolumePathNameA(LPCSTR lpszFileName, LPSTR lpszVolumePathName, DWORD cchBufferLength);
BOOL GetVolumeInformationA(LPCSTR lpRootPathName, LPSTR lpVolumeNameBuffer, DWORD nVolumeNameSize,
	LPDWORD lpVolumeSerialNumber, LPDWORD lpMaximumComponentLength, LPDWORD lpFileSystemFlags,
	LPSTR lpFileSystemNameBuffer, DWORD nFileSystemNameSize);
int SHCreateDirectoryExA(HWND hwnd, LPCSTR pszPath, const SECURITY_ATTRIBUTES* psa);
BOOL SetVolumeLabelA(LPCSTR lpRootPathName, LPCSTR lpVolumeName);

/*
 * Drive letters: on Linux, volumes we mount are assigned a virtual drive letter so
 * that the shared code can keep using "X:\path" notations, which every file API of
 * this compatibility layer translates to the actual mount point.
 */
BOOL linux_map_drive_letter(char letter, const char* mount_point);
void linux_unmap_drive_letter(char letter);
const char* linux_get_drive_letter_mount(char letter);
BOOL linux_reserve_drive_letter(char letter, const char* dev);
#ifndef MAXIMUM_WAIT_OBJECTS
#define MAXIMUM_WAIT_OBJECTS 64
#endif
BOOL linux_is_drive_letter_pending(char letter);
void linux_set_pending_mount_callback(BOOL (*cb)(char letter, const char* dev));
char linux_get_drive_letter_for_mount(const char* mount_point);

/* ------------------------------------------------------------------------- */
/* Prototypes for the Win32 subset implemented in linux/compat/compat.c      */
/* ------------------------------------------------------------------------- */
/* Errors */
DWORD GetLastError(void);
void SetLastError(DWORD dwErrCode);
DWORD ErrnoToWin32(int err);

/* Handles and files */
HANDLE CreateFileA(LPCSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode,
	LPSECURITY_ATTRIBUTES lpSecurityAttributes, DWORD dwCreationDisposition,
	DWORD dwFlagsAndAttributes, HANDLE hTemplateFile);
#define CreateFile CreateFileA
BOOL CloseHandle(HANDLE hObject);
BOOL ReadFile(HANDLE hFile, LPVOID lpBuffer, DWORD nNumberOfBytesToRead, LPDWORD lpNumberOfBytesRead, LPOVERLAPPED lpOverlapped);
BOOL WriteFile(HANDLE hFile, LPCVOID lpBuffer, DWORD nNumberOfBytesToWrite, LPDWORD lpNumberOfBytesWritten, LPOVERLAPPED lpOverlapped);
BOOL SetFilePointerEx(HANDLE hFile, LARGE_INTEGER liDistanceToMove, PLARGE_INTEGER lpNewFilePointer, DWORD dwMoveMethod);
DWORD SetFilePointer(HANDLE hFile, LONG lDistanceToMove, PLONG lpDistanceToMoveHigh, DWORD dwMoveMethod);
BOOL SetEndOfFile(HANDLE hFile);
BOOL GetFileSizeEx(HANDLE hFile, PLARGE_INTEGER lpFileSize);
DWORD GetFileSize(HANDLE hFile, LPDWORD lpFileSizeHigh);
BOOL FlushFileBuffers(HANDLE hFile);
BOOL SetFileTime(HANDLE hFile, const FILETIME* lpCreationTime, const FILETIME* lpLastAccessTime, const FILETIME* lpLastWriteTime);
BOOL GetFileInformationByHandle(HANDLE hFile, LPBY_HANDLE_FILE_INFORMATION lpFileInformation);
DWORD GetFileType(HANDLE hFile);
BOOL GetOverlappedResult(HANDLE hFile, LPOVERLAPPED lpOverlapped, LPDWORD lpNumberOfBytesTransferred, BOOL bWait);
BOOL CancelIo(HANDLE hFile);
BOOL CancelIoEx(HANDLE hFile, LPOVERLAPPED lpOverlapped);
BOOL DeviceIoControl(HANDLE hDevice, DWORD dwIoControlCode, LPVOID lpInBuffer, DWORD nInBufferSize,
	LPVOID lpOutBuffer, DWORD nOutBufferSize, LPDWORD lpBytesReturned, LPOVERLAPPED lpOverlapped);
/* Returns the underlying POSIX fd of a file handle (or -1) */
int HandleToFd(HANDLE h);
/* Wraps an existing POSIX fd into a file handle (the handle takes ownership) */
HANDLE FdToHandle(int fd, const char* path);
const char* HandleToPath(HANDLE h);
intptr_t _get_osfhandle(int fd);
int _open_osfhandle(intptr_t osfhandle, int flags);

DWORD GetFileAttributesA(LPCSTR lpFileName);
BOOL SetFileAttributesA(LPCSTR lpFileName, DWORD dwFileAttributes);
BOOL DeleteFileA(LPCSTR lpFileName);
BOOL CopyFileA(LPCSTR lpExistingFileName, LPCSTR lpNewFileName, BOOL bFailIfExists);
BOOL MoveFileA(LPCSTR lpExistingFileName, LPCSTR lpNewFileName);
BOOL MoveFileExA(LPCSTR lpExistingFileName, LPCSTR lpNewFileName, DWORD dwFlags);
BOOL CreateDirectoryA(LPCSTR lpPathName, LPSECURITY_ATTRIBUTES lpSecurityAttributes);
BOOL RemoveDirectoryA(LPCSTR lpPathName);
BOOL PathFileExistsA(LPCSTR pszPath);
BOOLEAN CreateSymbolicLinkA(LPCSTR lpSymlinkFileName, LPCSTR lpTargetFileName, DWORD dwFlags);
DWORD GetTempPathA(DWORD nBufferLength, LPSTR lpBuffer);
UINT GetTempFileNameA(LPCSTR lpPathName, LPCSTR lpPrefixString, UINT uUnique, LPSTR lpTempFileName);
DWORD GetCurrentDirectoryA(DWORD nBufferLength, LPSTR lpBuffer);
BOOL SetCurrentDirectoryA(LPCSTR lpPathName);
BOOL GetDiskFreeSpaceExA(LPCSTR lpDirectoryName, PULARGE_INTEGER lpFreeBytesAvailableToCaller,
	PULARGE_INTEGER lpTotalNumberOfBytes, PULARGE_INTEGER lpTotalNumberOfFreeBytes);
DWORD GetFullPathNameA(LPCSTR lpFileName, DWORD nBufferLength, LPSTR lpBuffer, LPSTR* lpFilePart);
DWORD GetModuleFileNameA(HMODULE hModule, LPSTR lpFilename, DWORD nSize);
HANDLE FindFirstFileA(LPCSTR lpFileName, LPWIN32_FIND_DATAA lpFindFileData);
BOOL FindNextFileA(HANDLE hFindFile, LPWIN32_FIND_DATAA lpFindFileData);
BOOL FindClose(HANDLE hFindFile);
#define GetFileAttributes GetFileAttributesA
#define SetFileAttributes SetFileAttributesA
#define DeleteFile DeleteFileA
#define CopyFile CopyFileA
#define MoveFile MoveFileA
#define MoveFileEx MoveFileExA
#define CreateDirectory CreateDirectoryA
#define RemoveDirectory RemoveDirectoryA
#define PathFileExists PathFileExistsA
#define GetTempPath GetTempPathA
#define GetTempFileName GetTempFileNameA
#define FindFirstFile FindFirstFileA
#define FindNextFile FindNextFileA

/* Threads & synchronization */
HANDLE CreateThread(LPSECURITY_ATTRIBUTES lpThreadAttributes, SIZE_T dwStackSize, LPTHREAD_START_ROUTINE lpStartAddress,
	LPVOID lpParameter, DWORD dwCreationFlags, LPDWORD lpThreadId);
uintptr_t _beginthreadex(void* security, unsigned stack_size, unsigned (*start_address)(void*),
	void* arglist, unsigned initflag, unsigned* thrdaddr);
DECLSPEC_NORETURN void ExitThread(DWORD dwExitCode);
BOOL TerminateThread(HANDLE hThread, DWORD dwExitCode);
BOOL GetExitCodeThread(HANDLE hThread, LPDWORD lpExitCode);
BOOL SetThreadPriority(HANDLE hThread, int nPriority);
DWORD_PTR SetThreadAffinityMask(HANDLE hThread, DWORD_PTR dwThreadAffinityMask);
BOOL GetProcessAffinityMask(HANDLE hProcess, PDWORD_PTR lpProcessAffinityMask, PDWORD_PTR lpSystemAffinityMask);
HANDLE GetCurrentThread(void);
HANDLE GetCurrentProcess(void);
DWORD GetCurrentThreadId(void);
DWORD GetCurrentProcessId(void);
DWORD ResumeThread(HANDLE hThread);
HANDLE CreateEventA(LPSECURITY_ATTRIBUTES lpEventAttributes, BOOL bManualReset, BOOL bInitialState, LPCSTR lpName);
#define CreateEvent CreateEventA
BOOL SetEvent(HANDLE hEvent);
BOOL ResetEvent(HANDLE hEvent);
HANDLE CreateMutexA(LPSECURITY_ATTRIBUTES lpMutexAttributes, BOOL bInitialOwner, LPCSTR lpName);
#define CreateMutex CreateMutexA
BOOL ReleaseMutex(HANDLE hMutex);
DWORD WaitForSingleObject(HANDLE hHandle, DWORD dwMilliseconds);
DWORD WaitForMultipleObjects(DWORD nCount, const HANDLE* lpHandles, BOOL bWaitAll, DWORD dwMilliseconds);
void Sleep(DWORD dwMilliseconds);
DWORD GetTickCount(void);
ULONGLONG GetTickCount64(void);
BOOL QueryPerformanceCounter(LARGE_INTEGER* lpPerformanceCount);
BOOL QueryPerformanceFrequency(LARGE_INTEGER* lpFrequency);
void InitializeCriticalSection(LPCRITICAL_SECTION lpCriticalSection);
void DeleteCriticalSection(LPCRITICAL_SECTION lpCriticalSection);
void EnterCriticalSection(LPCRITICAL_SECTION lpCriticalSection);
void LeaveCriticalSection(LPCRITICAL_SECTION lpCriticalSection);
static inline LONG InterlockedIncrement(LONG volatile* p) { return __sync_add_and_fetch(p, 1); }
static inline LONG InterlockedDecrement(LONG volatile* p) { return __sync_sub_and_fetch(p, 1); }
static inline LONG InterlockedExchange(LONG volatile* p, LONG v) { return __sync_lock_test_and_set(p, v); }
static inline LONG InterlockedCompareExchange(LONG volatile* p, LONG x, LONG c) { return __sync_val_compare_and_swap(p, c, x); }

/* Time */
void GetSystemTime(LPSYSTEMTIME lpSystemTime);
void GetLocalTime(LPSYSTEMTIME lpSystemTime);
void GetSystemTimeAsFileTime(LPFILETIME lpSystemTimeAsFileTime);
BOOL SystemTimeToFileTime(const SYSTEMTIME* lpSystemTime, LPFILETIME lpFileTime);
BOOL FileTimeToSystemTime(const FILETIME* lpFileTime, LPSYSTEMTIME lpSystemTime);
BOOL LocalFileTimeToFileTime(const FILETIME* lpLocalFileTime, LPFILETIME lpFileTime);

/* Strings & locale */
int MultiByteToWideChar(UINT CodePage, DWORD dwFlags, LPCSTR lpMultiByteStr, int cbMultiByte, LPWSTR lpWideCharStr, int cchWideChar);
int WideCharToMultiByte(UINT CodePage, DWORD dwFlags, LPCWSTR lpWideCharStr, int cchWideChar, LPSTR lpMultiByteStr,
	int cbMultiByte, LPCSTR lpDefaultChar, LPBOOL lpUsedDefaultChar);
LPSTR CharLowerA(LPSTR lpsz);
LPSTR CharUpperA(LPSTR lpsz);
LPSTR CharNextA(LPCSTR lpsz);
#define CharLower CharLowerA
#define CharUpper CharUpperA
PCSTR StrStrIA(PCSTR pszFirst, PCSTR pszSrch);
PCSTR StrRStrIA(PCSTR pszSource, PCSTR pszLast, PCSTR pszSrch);
BOOL PathMatchSpecA(LPCSTR pszFile, LPCSTR pszSpec);
LANGID GetUserDefaultUILanguage(void);
LANGID GetSystemDefaultUILanguage(void);
LANGID GetUserDefaultLangID(void);
LCID GetUserDefaultLCID(void);
LCID GetSystemDefaultLCID(void);
int GetLocaleInfoA(LCID Locale, LCTYPE LCType, LPSTR lpLCData, int cchData);
int MulDiv(int nNumber, int nNumerator, int nDenominator);
int lstrlenA(LPCSTR lpString);
static inline LANGID GetThreadUILanguage(void) { return MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US); }
static inline LANGID SetThreadUILanguage(LANGID l) { return l; }
static inline int wcsncpy_s(wchar_t* d, size_t ds, const wchar_t* s, size_t c)
{
	size_t n;
	if (d == NULL || ds == 0) return EINVAL;
	n = (s == NULL) ? 0 : wcsnlen(s, (c == _TRUNCATE) ? ds - 1 : c);
	if (n > ds - 1) n = ds - 1;
	wmemcpy(d, s, n);
	d[n] = 0;
	return 0;
}
#define lstrcmpiA strcasecmp
#define lstrcmpA strcmp
#define lstrcpyA strcpy

/* Environment / system */
DWORD GetEnvironmentVariableA(LPCSTR lpName, LPSTR lpBuffer, DWORD nSize);
BOOL SetEnvironmentVariableA(LPCSTR lpName, LPCSTR lpValue);
DWORD ExpandEnvironmentStringsA(LPCSTR lpSrc, LPSTR lpDst, DWORD nSize);
UINT GetSystemDirectoryA(LPSTR lpBuffer, UINT uSize);
UINT GetWindowsDirectoryA(LPSTR lpBuffer, UINT uSize);
HMODULE GetModuleHandleA(LPCSTR lpModuleName);
HMODULE GetModuleHandleW(LPCWSTR lpModuleName);
HMODULE LoadLibraryA(LPCSTR lpLibFileName);
HMODULE LoadLibraryExW(LPCWSTR lpLibFileName, HANDLE hFile, DWORD dwFlags);
FARPROC GetProcAddress(HMODULE hModule, LPCSTR lpProcName);
BOOL FreeLibrary(HMODULE hLibModule);
#define LOAD_LIBRARY_SEARCH_SYSTEM32 0x00000800
void OutputDebugStringA(LPCSTR lpOutputString);
#define OutputDebugString OutputDebugStringA
BOOL IsDebuggerPresent(void);

/* Resources (served from /usr/share/rufus, see compat.c) */
HRSRC FindResourceA(HMODULE hModule, LPCSTR lpName, LPCSTR lpType);
HGLOBAL LoadResource(HMODULE hModule, HRSRC hResInfo);
LPVOID LockResource(HGLOBAL hResData);
DWORD SizeofResource(HMODULE hModule, HRSRC hResInfo);
#define FindResource FindResourceA
#define RT_RCDATA MAKEINTRESOURCEA(10)
#define RT_ICON MAKEINTRESOURCEA(3)
#define RT_GROUP_ICON MAKEINTRESOURCEA(14)

/* Memory */
HGLOBAL GlobalAlloc(UINT uFlags, SIZE_T dwBytes);
HGLOBAL GlobalFree(HGLOBAL hMem);
LPVOID GlobalLock(HGLOBAL hMem);
BOOL GlobalUnlock(HGLOBAL hMem);
#define GMEM_FIXED 0x0000
#define GMEM_MOVEABLE 0x0002
#define GMEM_ZEROINIT 0x0040
#define GHND (GMEM_MOVEABLE | GMEM_ZEROINIT)
#define GPTR (GMEM_FIXED | GMEM_ZEROINIT)
#define LocalFree(p) (free(p), (HLOCAL)NULL)
#define ZeroMemory(p, n) memset((p), 0, (n))
#define RtlZeroMemory ZeroMemory
#define CopyMemory(d, s, n) memcpy((d), (s), (n))
#define MoveMemory(d, s, n) memmove((d), (s), (n))
#define FillMemory(d, n, v) memset((d), (v), (n))
#define SecureZeroMemory(p, n) explicit_bzero((p), (n))

/* GUIDs */
HRESULT CoCreateGuid(GUID* pguid);
int StringFromGUID2(REFGUID rguid, LPWSTR lpsz, int cchMax);
typedef GUID *LPCLSID;
HRESULT CLSIDFromString(LPCWSTR lpsz, LPCLSID pclsid);

/* UI hooks the portable code uses: implemented by the Linux front-end (ui) */
HWND GetDlgItem(HWND hDlg, int nIDDlgItem);
LRESULT SendMessageA(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam);
BOOL PostMessageA(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam);
LRESULT SendDlgItemMessageA(HWND hDlg, int nIDDlgItem, UINT Msg, WPARAM wParam, LPARAM lParam);
#define SendMessage SendMessageA
#define PostMessage PostMessageA
#define SendDlgItemMessage SendDlgItemMessageA
UINT IsDlgButtonChecked(HWND hDlg, int nIDButton);
BOOL CheckDlgButton(HWND hDlg, int nIDButton, UINT uCheck);
BOOL EnableWindow(HWND hWnd, BOOL bEnable);
BOOL IsWindowEnabled(HWND hWnd);
BOOL ShowWindow(HWND hWnd, int nCmdShow);
BOOL IsWindow(HWND hWnd);
BOOL IsWindowVisible(HWND hWnd);
BOOL SetWindowTextA(HWND hWnd, LPCSTR lpString);
int GetWindowTextA(HWND hWnd, LPSTR lpString, int nMaxCount);
int GetWindowTextLengthA(HWND hWnd);
#define SetWindowText SetWindowTextA
#define GetWindowText GetWindowTextA
UINT_PTR SetTimer(HWND hWnd, UINT_PTR nIDEvent, UINT uElapse, TIMERPROC lpTimerFunc);
BOOL KillTimer(HWND hWnd, UINT_PTR uIDEvent);
BOOL InvalidateRect(HWND hWnd, const RECT* lpRect, BOOL bErase);
BOOL UpdateWindow(HWND hWnd);
BOOL SetFocus(HWND hWnd);
int MessageBoxA(HWND hWnd, LPCSTR lpText, LPCSTR lpCaption, UINT uType);
#define MessageBox MessageBoxA
BOOL EndDialog(HWND hDlg, INT_PTR nResult);
BOOL DestroyWindow(HWND hWnd);
BOOL MessageBeep(UINT uType);

/* CRT names */
#define _stricmp strcasecmp
#define _strnicmp strncasecmp
#define stricmp strcasecmp
#define strnicmp strncasecmp
#define _wcsicmp wcscasecmp
#define _wcsnicmp wcsncasecmp
#define _strdup strdup
#define _wcsdup wcsdup
#define _strtoui64 strtoull
#define _strtoi64 strtoll
#define _atoi64 atoll
#define _snprintf snprintf
#define _vsnprintf vsnprintf
#define _snwprintf swprintf
#define _fileno fileno
#define _fdopen fdopen
#define _fseeki64 fseeko
#define _ftelli64 ftello
#define _lseeki64 lseek
#define _lseek lseek
#define _read read
#define _write write
#define _close close
#define _dup dup
#define _dup2 dup2
#define _pipe(fds, size, mode) pipe(fds)
#define _unlink _unlinkU
#define _rmdir _rmdirU
#define _getcwd getcwd
#define _chmod(p, m) chmod(linux_path_tmp(p), (m))
#define _access _accessU
#define _isatty isatty
#define _getpid getpid
#define _alloca alloca
#define _mm_malloc_compat(s, a) aligned_alloc((a), (((s) + (a) - 1) / (a)) * (a))
#define _aligned_malloc(s, a) _mm_malloc_compat(s, a)
#define _aligned_free free
#ifndef _rotl
#define _rotl(x, n) (((uint32_t)(x) << ((n) & 31)) | ((uint32_t)(x) >> ((32 - ((n) & 31)) & 31)))
#endif
#define _rotl64(x, n) (((uint64_t)(x) << ((n) & 63)) | ((uint64_t)(x) >> ((64 - ((n) & 63)) & 63)))
#ifndef _rotr
#define _rotr(x, n) (((uint32_t)(x) >> ((n) & 31)) | ((uint32_t)(x) << ((32 - ((n) & 31)) & 31)))
#endif
#define _byteswap_ushort __builtin_bswap16
#define _byteswap_ulong __builtin_bswap32
#define _byteswap_uint64 __builtin_bswap64
#define _O_RDONLY O_RDONLY
#define _O_WRONLY O_WRONLY
#define _O_RDWR O_RDWR
#define _O_CREAT O_CREAT
#define _O_TRUNC O_TRUNC
#define _O_EXCL O_EXCL
#define _O_APPEND O_APPEND
#define _O_BINARY 0
#define _O_TEXT 0
#define O_BINARY 0
#define _O_U8TEXT 0
#define _O_WTEXT 0
#define _O_NOINHERIT O_CLOEXEC
#define _S_IREAD S_IRUSR
#define _S_IWRITE S_IWUSR
#define _S_IFDIR S_IFDIR
#define _S_IFREG S_IFREG
#define _S_IFMT S_IFMT
#define _SH_DENYNO 0x40
#define _SH_DENYWR 0x20
#define _SH_DENYRW 0x10
#define _P_WAIT 0
typedef struct stat _stat64_t;
#define _stat64 stat
#define _stati64 stat
#define _fstat64 fstat
#define _fstati64 fstat

static inline int _snprintf_s(char* buf, size_t size, size_t count, const char* fmt, ...)
	__attribute__((format(printf, 4, 5)));
static inline int _snprintf_s(char* buf, size_t size, size_t count, const char* fmt, ...)
{
	va_list ap;
	int r;
	(void)count;
	if (buf == NULL || size == 0)
		return -1;
	va_start(ap, fmt);
	r = vsnprintf(buf, size, fmt, ap);
	va_end(ap);
	return (r < 0 || (size_t)r >= size) ? -1 : r;
}
static inline int _vsnprintf_s(char* buf, size_t size, size_t count, const char* fmt, va_list ap)
{
	int r;
	(void)count;
	if (buf == NULL || size == 0)
		return -1;
	r = vsnprintf(buf, size, fmt, ap);
	return (r < 0 || (size_t)r >= size) ? -1 : r;
}
static inline int strncat_s(char* dst, size_t dst_size, const char* src, size_t count)
{
	size_t len, n;
	if (dst == NULL || src == NULL || dst_size == 0)
		return EINVAL;
	len = strnlen(dst, dst_size);
	if (len >= dst_size)
		return EINVAL;
	n = strlen(src);
	if (count != _TRUNCATE && count < n)
		n = count;
	if (n > dst_size - len - 1)
		n = dst_size - len - 1;
	memcpy(&dst[len], src, n);
	dst[len + n] = 0;
	return 0;
}
static inline int strcpy_s(char* dst, size_t size, const char* src)
{
	if (dst == NULL || size == 0)
		return EINVAL;
	snprintf(dst, size, "%s", (src == NULL) ? "" : src);
	return 0;
}
static inline int strcat_s(char* dst, size_t size, const char* src)
{
	return strncat_s(dst, size, src, _TRUNCATE);
}
static inline int sprintf_s(char* buf, size_t size, const char* fmt, ...) __attribute__((format(printf, 3, 4)));
static inline int sprintf_s(char* buf, size_t size, const char* fmt, ...)
{
	va_list ap;
	int r;
	va_start(ap, fmt);
	r = vsnprintf(buf, size, fmt, ap);
	va_end(ap);
	return r;
}
static inline char* _strlwr(char* s) { char* p = s; while (p && *p) { *p = (char)tolower((unsigned char)*p); p++; } return s; }
static inline char* _strupr(char* s) { char* p = s; while (p && *p) { *p = (char)toupper((unsigned char)*p); p++; } return s; }
static inline char* _itoa(int value, char* str, int radix) { (void)radix; sprintf(str, "%d", value); return str; }
static inline wchar_t* _wcslwr(wchar_t* s) { wchar_t* p = s; while (p && *p) { *p = (wchar_t)towlower(*p); p++; } return s; }
static inline int fopen_s(FILE** f, const char* path, const char* mode)
{
	*f = fopen(path, mode);
	return (*f == NULL) ? errno : 0;
}
int _mkdirU(const char* dirname);
int _chdirU(const char* dirname);
int _unlinkU(const char* path);
int _rmdirU(const char* dirname);
int _accessU(const char* path, int mode);
int _openU(const char* filename, int oflag, int pmode);
/* Returns a thread local translated copy of path (valid until the next call) */
const char* linux_path_tmp(const char* path);
#define _mkdir _mkdirU
#define _chdir _chdirU
static inline int64_t _filelengthi64(int fd) { struct stat st; return (fstat(fd, &st) == 0) ? (int64_t)st.st_size : -1; }
static inline long _filelength(int fd) { return (long)_filelengthi64(fd); }
static inline int _open(const char* path, int flags, ...)
{
	va_list ap;
	int mode = 0;
	if (flags & O_CREAT) {
		va_start(ap, flags);
		mode = va_arg(ap, int);
		va_end(ap);
	}
	return _openU(path, flags, mode);
}
static inline int _sopen_s(int* fd, const char* path, int oflag, int shflag, int pmode)
{
	(void)shflag;
	*fd = _open(path, oflag, pmode);
	return (*fd < 0) ? errno : 0;
}

#ifdef __cplusplus
}
#endif

#include "winctl_api.h"

#endif /* RUFUS_LINUX_WINDOWS_H */
