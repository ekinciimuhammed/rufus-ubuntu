/*
 * Rufus for Linux: emulation of the MSVC wide character text streams
 * Copyright © 2026 Rufus Linux port contributors
 *
 * Rufus' config file parsers use _wfopen(..., L"r, ccs=UNICODE"), fgetws(),
 * fputws() and fwprintf_s() the way the Microsoft C runtime implements them:
 * - files are decoded according to their BOM (UTF-8, UTF-16LE or ANSI),
 * - text mode strips the CR of CR/LF line endings when reading,
 * - "%s" in a wide format string designates a wide string.
 * glibc's wide streams behave differently (fixed orientation, %ls...), so
 * these byte-oriented replacements are used instead.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */
#pragma once
#ifndef RUFUS_LINUX_WSTDIO_COMPAT_H
#define RUFUS_LINUX_WSTDIO_COMPAT_H

#include <stdio.h>
#include <wchar.h>

FILE* rufus_fopen(const char* path, const char* mode);
FILE* rufus_wfopen(const wchar_t* path, const wchar_t* mode);
int rufus_fclose(FILE* f);
wchar_t* rufus_fgetws(wchar_t* buf, int n, FILE* f);
int rufus_fputws(const wchar_t* s, FILE* f);
int rufus_fwprintf(FILE* f, const wchar_t* fmt, ...);
int rufus_vfwprintf(FILE* f, const wchar_t* fmt, va_list ap);
int rufus_wunlink(const wchar_t* path);
int rufus_wrename(const wchar_t* oldpath, const wchar_t* newpath);

#define _wfopen rufus_wfopen
#define fclose rufus_fclose
#define fgetws rufus_fgetws
#define fputws rufus_fputws
#define fwprintf_s rufus_fwprintf
#define _wunlink rufus_wunlink
#define _wrename rufus_wrename

#endif
