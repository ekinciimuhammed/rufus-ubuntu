/*
 * Rufus for Linux: emulation of the MSVC wide character text streams
 * Copyright © 2026 Rufus Linux port contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */
#include <windows.h>
#include <msapi_utf8_linux.h>
#include <pthread.h>

/* We want the real stdio functions here */
#undef fopen
#undef fclose
#undef fgetws
#undef fputws

enum stream_encoding { ENC_ANSI = 0, ENC_UTF8, ENC_UTF16LE };

typedef struct {
	FILE* f;
	int encoding;
	BOOL text;		/* text mode: strip CR from CR/LF when reading */
} stream_info;

#define MAX_STREAMS 64
static stream_info streams[MAX_STREAMS];
static pthread_mutex_t streams_lock = PTHREAD_MUTEX_INITIALIZER;

static stream_info* find_stream(FILE* f)
{
	int i;
	for (i = 0; i < MAX_STREAMS; i++)
		if (streams[i].f == f)
			return &streams[i];
	return NULL;
}

static void register_stream(FILE* f, int encoding, BOOL text)
{
	stream_info* s;
	pthread_mutex_lock(&streams_lock);
	s = find_stream(f);
	if (s == NULL)
		s = find_stream(NULL);
	if (s != NULL) {
		s->f = f;
		s->encoding = encoding;
		s->text = text;
	}
	pthread_mutex_unlock(&streams_lock);
}

static stream_info get_stream(FILE* f)
{
	stream_info r = { f, ENC_UTF8, FALSE }, *s;
	pthread_mutex_lock(&streams_lock);
	s = find_stream(f);
	if (s != NULL)
		r = *s;
	pthread_mutex_unlock(&streams_lock);
	return r;
}

int rufus_fclose(FILE* f)
{
	stream_info* s;
	if (f == NULL)
		return EOF;
	pthread_mutex_lock(&streams_lock);
	s = find_stream(f);
	if (s != NULL)
		memset(s, 0, sizeof(*s));
	pthread_mutex_unlock(&streams_lock);
	return fclose(f);
}

/*
 * Open a file using an MSVC style mode string, e.g. "r, ccs=UNICODE",
 * "w, ccs=UTF-16LE" or "rb".
 */
FILE* rufus_fopen(const char* path, const char* mode)
{
	char m[8], *p = linux_path(path);
	const char* ccs;
	int i, j, encoding = ENC_ANSI;
	BOOL text = TRUE, has_ccs = FALSE, reading;
	uint8_t bom[3];
	FILE* f;

	if (p == NULL || mode == NULL) {
		free(p);
		return NULL;
	}
	for (i = 0, j = 0; mode[i] != 0 && mode[i] != ',' && j < (int)sizeof(m) - 2; i++) {
		if (mode[i] == 'b')
			text = FALSE;
		if (strchr("rwa+bx", mode[i]) != NULL)
			m[j++] = mode[i];
	}
	m[j++] = 'e';
	m[j] = 0;
	ccs = strstr(mode, "ccs=");
	if (ccs != NULL) {
		has_ccs = TRUE;
		ccs += 4;
		if (strncasecmp(ccs, "UTF-16LE", 8) == 0)
			encoding = ENC_UTF16LE;
		else if (strncasecmp(ccs, "UTF-8", 5) == 0)
			encoding = ENC_UTF8;
		else	/* UNICODE: depends on the BOM when reading */
			encoding = ENC_UTF8;
	}
	reading = (m[0] == 'r');
	f = fopen(p, m);
	free(p);
	if (f == NULL)
		return NULL;
	if (reading) {
		/* Detect the encoding from the BOM (left in the stream, skipped by fgetws) */
		size_t n = fread(bom, 1, sizeof(bom), f);
		if (n >= 2 && bom[0] == 0xFF && bom[1] == 0xFE)
			encoding = ENC_UTF16LE;
		else if (n >= 3 && bom[0] == 0xEF && bom[1] == 0xBB && bom[2] == 0xBF)
			encoding = ENC_UTF8;
		else if (encoding == ENC_UTF16LE && !has_ccs)
			encoding = ENC_ANSI;
		fseek(f, 0, SEEK_SET);
	} else if (has_ccs && m[0] == 'w') {
		/* MSVC writes a BOM for the Unicode encodings */
		if (encoding == ENC_UTF16LE)
			fwrite("\xFF\xFE", 1, 2, f);
		else if (encoding == ENC_UTF8)
			fwrite("\xEF\xBB\xBF", 1, 3, f);
	}
	if (has_ccs || text)
		register_stream(f, encoding, text);
	return f;
}

FILE* rufus_wfopen(const wchar_t* path, const wchar_t* mode)
{
	char *p = wchar_to_utf8(path), *m = wchar_to_utf8(mode);
	FILE* f = NULL;
	if (p != NULL && m != NULL)
		f = rufus_fopen(p, m);
	free(p);
	free(m);
	return f;
}

/* Decode one code point from a stream according to its encoding. Returns WEOF on EOF. */
static wint_t read_char(FILE* f, int encoding)
{
	int c, c2, c3, c4;
	uint32_t cp, lo;

	if (encoding == ENC_UTF16LE) {
		if ((c = getc(f)) == EOF || (c2 = getc(f)) == EOF)
			return WEOF;
		cp = (uint32_t)c | ((uint32_t)c2 << 8);
		if (cp >= 0xD800 && cp < 0xDC00) {
			if ((c = getc(f)) == EOF || (c2 = getc(f)) == EOF)
				return WEOF;
			lo = (uint32_t)c | ((uint32_t)c2 << 8);
			if (lo >= 0xDC00 && lo < 0xE000)
				cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
		}
		return (wint_t)cp;
	}
	if ((c = getc(f)) == EOF)
		return WEOF;
	if (c < 0x80)
		return (wint_t)c;
	/* UTF-8, with a Latin-1 fallback for invalid sequences (ANSI files) */
	if ((c & 0xE0) == 0xC0) {
		c2 = getc(f);
		if (c2 != EOF && (c2 & 0xC0) == 0x80)
			return (wint_t)(((c & 0x1F) << 6) | (c2 & 0x3F));
		if (c2 != EOF)
			ungetc(c2, f);
		return (wint_t)c;
	}
	if ((c & 0xF0) == 0xE0) {
		c2 = getc(f);
		if (c2 != EOF && (c2 & 0xC0) == 0x80) {
			c3 = getc(f);
			if (c3 != EOF && (c3 & 0xC0) == 0x80)
				return (wint_t)(((c & 0x0F) << 12) | ((c2 & 0x3F) << 6) | (c3 & 0x3F));
			if (c3 != EOF)
				ungetc(c3, f);
		}
		/* Can't push back more than one byte reliably: degrade gracefully */
		return (wint_t)c;
	}
	if ((c & 0xF8) == 0xF0) {
		c2 = getc(f); c3 = getc(f); c4 = getc(f);
		if (c2 != EOF && c3 != EOF && c4 != EOF && (c2 & 0xC0) == 0x80 && (c3 & 0xC0) == 0x80 && (c4 & 0xC0) == 0x80)
			return (wint_t)(((c & 0x07) << 18) | ((c2 & 0x3F) << 12) | ((c3 & 0x3F) << 6) | (c4 & 0x3F));
		return (wint_t)c;
	}
	return (wint_t)c;
}

wchar_t* rufus_fgetws(wchar_t* buf, int n, FILE* f)
{
	stream_info s = get_stream(f);
	int i = 0;
	wint_t c;

	if (buf == NULL || n <= 0 || f == NULL)
		return NULL;
	/* Skip the BOM at the start of the file */
	if (ftell(f) == 0) {
		uint8_t b[3] = { 0 };
		size_t r = fread(b, 1, 3, f);
		if (r >= 2 && b[0] == 0xFF && b[1] == 0xFE) {
			s.encoding = ENC_UTF16LE;
			fseek(f, 2, SEEK_SET);
		} else if (r >= 3 && b[0] == 0xEF && b[1] == 0xBB && b[2] == 0xBF) {
			fseek(f, 3, SEEK_SET);
		} else {
			fseek(f, 0, SEEK_SET);
		}
	}
	while (i < n - 1) {
		c = read_char(f, s.encoding);
		if (c == WEOF)
			break;
		if (c == L'\r' && s.text) {
			/* Text mode: CR/LF -> LF */
			wint_t c2 = read_char(f, s.encoding);
			if (c2 == L'\n') {
				buf[i++] = L'\n';
				break;
			}
			buf[i++] = L'\r';
			if (c2 == WEOF)
				break;
			if (i >= n - 1) {
				/* No room: rewind the extra character */
				fseek(f, (s.encoding == ENC_UTF16LE) ? -2 : -1, SEEK_CUR);
				break;
			}
			c = c2;
		}
		buf[i++] = (wchar_t)c;
		if (c == L'\n')
			break;
	}
	if (i == 0)
		return NULL;
	buf[i] = 0;
	return buf;
}

static int write_wide(const wchar_t* s, size_t len, FILE* f, int encoding)
{
	size_t i;
	for (i = 0; i < len; i++) {
		uint32_t cp = (uint32_t)s[i];
		if (encoding == ENC_UTF16LE) {
			if (cp >= 0x10000) {
				uint32_t v = cp - 0x10000;
				uint16_t hi = (uint16_t)(0xD800 + (v >> 10)), lo = (uint16_t)(0xDC00 + (v & 0x3FF));
				putc(hi & 0xFF, f); putc(hi >> 8, f);
				putc(lo & 0xFF, f); putc(lo >> 8, f);
			} else {
				putc(cp & 0xFF, f);
				putc((cp >> 8) & 0xFF, f);
			}
		} else {
			char tmp[4];
			int n;
			if (cp < 0x80) {
				tmp[0] = (char)cp; n = 1;
			} else if (cp < 0x800) {
				tmp[0] = (char)(0xC0 | (cp >> 6)); tmp[1] = (char)(0x80 | (cp & 0x3F)); n = 2;
			} else if (cp < 0x10000) {
				tmp[0] = (char)(0xE0 | (cp >> 12)); tmp[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
				tmp[2] = (char)(0x80 | (cp & 0x3F)); n = 3;
			} else {
				tmp[0] = (char)(0xF0 | (cp >> 18)); tmp[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
				tmp[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); tmp[3] = (char)(0x80 | (cp & 0x3F)); n = 4;
			}
			if (fwrite(tmp, 1, n, f) != (size_t)n)
				return EOF;
		}
	}
	return ferror(f) ? EOF : 0;
}

int rufus_fputws(const wchar_t* s, FILE* f)
{
	stream_info si = get_stream(f);
	if (s == NULL || f == NULL)
		return EOF;
	return write_wide(s, wcslen(s), f, si.encoding);
}

/* Convert an MSVC wide format string, where %s/%c designate wide arguments, to glibc's */
static wchar_t* msvc_to_glibc_wformat(const wchar_t* fmt)
{
	size_t len = wcslen(fmt), i, o = 0;
	wchar_t* r = calloc(2 * len + 1, sizeof(wchar_t));
	if (r == NULL)
		return NULL;
	for (i = 0; i < len; i++) {
		r[o++] = fmt[i];
		if (fmt[i] != L'%')
			continue;
		if (fmt[i + 1] == L'%') {
			r[o++] = fmt[++i];
			continue;
		}
		/* Copy flags, width, precision */
		while (i + 1 < len && wcschr(L"-+ #0123456789.*", fmt[i + 1]) != NULL)
			r[o++] = fmt[++i];
		if (fmt[i + 1] == L's' || fmt[i + 1] == L'c') {
			r[o++] = L'l';
			r[o++] = fmt[++i];
		} else if (fmt[i + 1] == L'S' || fmt[i + 1] == L'C') {
			r[o++] = (fmt[++i] == L'S') ? L's' : L'c';
		}
	}
	r[o] = 0;
	return r;
}

int rufus_vfwprintf(FILE* f, const wchar_t* fmt, va_list ap)
{
	stream_info si = get_stream(f);
	wchar_t* gfmt = msvc_to_glibc_wformat(fmt);
	wchar_t* buf = NULL;
	size_t size = 1024;
	int r = -1;
	va_list aq;

	if (gfmt == NULL)
		return -1;
	for (;;) {
		buf = calloc(size, sizeof(wchar_t));
		if (buf == NULL)
			break;
		va_copy(aq, ap);
		r = vswprintf(buf, size, gfmt, aq);
		va_end(aq);
		if (r >= 0)
			break;
		free(buf);
		buf = NULL;
		if (size >= 1024 * 1024)
			break;
		size *= 4;
	}
	if (buf != NULL && r >= 0 && write_wide(buf, (size_t)r, f, si.encoding) != 0)
		r = -1;
	free(buf);
	free(gfmt);
	return r;
}

int rufus_fwprintf(FILE* f, const wchar_t* fmt, ...)
{
	va_list ap;
	int r;
	va_start(ap, fmt);
	r = rufus_vfwprintf(f, fmt, ap);
	va_end(ap);
	return r;
}

int rufus_wunlink(const wchar_t* path)
{
	char* p = wchar_to_utf8(path);
	int r = (p == NULL) ? -1 : _unlinkU(p);
	free(p);
	return r;
}

int rufus_wrename(const wchar_t* oldpath, const wchar_t* newpath)
{
	char *o = wchar_to_utf8(oldpath), *n = wchar_to_utf8(newpath);
	int r = -1;
	if (o != NULL && n != NULL)
		r = MoveFileExA(o, n, MOVEFILE_REPLACE_EXISTING) ? 0 : -1;
	free(o);
	free(n);
	return r;
}
