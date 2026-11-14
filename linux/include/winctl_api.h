/*
 * Rufus for Linux: Win32 user interface API subset implemented by the control
 * model (linux/ui/winctl.c). Does not depend on GTK.
 * Copyright © 2026 Rufus Linux port contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */
#pragma once

/* Notification codes (in the high word of WM_COMMAND's wParam) */
#define BN_CLICKED          0
#define CBN_SELCHANGE_      CBN_SELCHANGE
#define CBN_DROPDOWN        7
#define CBN_CLOSEUP         8
#define EN_CHANGE           0x0300
#define EN_KILLFOCUS        0x0200
#define EN_SETFOCUS         0x0100
#define STN_CLICKED         0

/* Window messages handled by the model that windows.h doesn't define */
#define WM_HSCROLL          0x0114
#define WM_DROPFILES        0x0233
#define WM_NEXTDLGCTL       0x0028
#define WM_KEYDOWN          0x0100
#define WM_SYSKEYDOWN       0x0104
#define TB_ENABLEBUTTON     (WM_USER + 1)
#define TB_GETRECT          (WM_USER + 51)
#define TBM_GETPOS          (WM_USER)
#define TBM_SETPOS          (WM_USER + 5)
#define TBM_SETRANGEMIN     (WM_USER + 7)
#define TBM_SETRANGEMAX     (WM_USER + 8)
#define EM_LIMITTEXT        0x00C5
#define EM_SETSEL           0x00B1
#define RDW_INVALIDATE      0x0001
#define RDW_ALLCHILDREN     0x0080
#define RDW_UPDATENOW       0x0100
#define BS_AUTOCHECKBOX     0x00000003L
#define BS_AUTORADIOBUTTON  0x00000009L

/* Windows API calls implemented by the model (in addition to windows.h) */
BOOL SetDlgItemTextU(HWND hDlg, int nIDDlgItem, const char* lpString);
int ComboBox_DeleteString(HWND hCtrl, int index);
int ComboBox_InsertStringU(HWND hCtrl, int index, const char* str);
int ComboBox_GetTextU(HWND hCtrl, char* str, int max_str);
int ComboBox_FindStringExact(HWND hCtrl, int start, const char* str);
BOOL RedrawWindow(HWND hWnd, const RECT* lprcUpdate, void* hrgnUpdate, UINT flags);
BOOL SetWindowPos(HWND hWnd, HWND hWndInsertAfter, int X, int Y, int cx, int cy, UINT uFlags);
BOOL MoveWindow(HWND hWnd, int X, int Y, int nWidth, int nHeight, BOOL bRepaint);
BOOL GetWindowRect(HWND hWnd, LPRECT lpRect);
BOOL GetClientRect(HWND hWnd, LPRECT lpRect);
#define SWP_NOSIZE          0x0001
#define SWP_NOMOVE          0x0002
#define SWP_NOZORDER        0x0004
#define SWP_SHOWWINDOW      0x0040
#define SWP_HIDEWINDOW      0x0080
#define HWND_TOP            ((HWND)0)

/* File mapping (used by rufus.c to read bootloaders) */
#define PAGE_READONLY       0x02
#define FILE_MAP_READ       0x0004
HANDLE CreateFileMappingA(HANDLE hFile, LPSECURITY_ATTRIBUTES lpAttributes, DWORD flProtect,
	DWORD dwMaximumSizeHigh, DWORD dwMaximumSizeLow, LPCSTR lpName);
#define CreateFileMapping CreateFileMappingA
LPVOID MapViewOfFile(HANDLE hFileMappingObject, DWORD dwDesiredAccess, DWORD dwFileOffsetHigh,
	DWORD dwFileOffsetLow, SIZE_T dwNumberOfBytesToMap);
BOOL UnmapViewOfFile(LPCVOID lpBaseAddress);
