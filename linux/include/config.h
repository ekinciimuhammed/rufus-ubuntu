/*
 * Rufus for Linux: config.h for the bundled libcdio (replaces src/libcdio/config.h)
 * Copyright © 2026 Rufus Linux port contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */
#ifndef LIBCDIO_CONFIG_H
#define LIBCDIO_CONFIG_H 1

#define EMPTY_ARRAY_SIZE
#define HAVE_DAYLIGHT 1
#define HAVE_ERRNO_H 1
#define HAVE_FCNTL_H 1
#define HAVE_LSEEK64 1
#define HAVE_FSEEKO 1
/* HAVE_FSEEKO64 deliberately not set: with _FILE_OFFSET_BITS=64, fseeko is 64-bit */
#define HAVE_FTRUNCATE 1
#define DO_NOT_WANT_COMPATIBILITY /**/
#define HAVE_INTTYPES_H 1
/* glibc provides iconv, which gives us Joliet support */
#define HAVE_ICONV 1
#define ICONV_CONST
#define HAVE_JOLIET 1
#define HAVE_LANGINFO_CODESET 1
#define HAVE_LIMITS_H 1
#define HAVE_MEMCPY 1
#define HAVE_MEMORY_H 1
#define HAVE_MEMSET 1
#define HAVE_RAND 1
#define HAVE_ROCK 1
#define HAVE_SNPRINTF 1
#define HAVE_SETENV 1
#define HAVE_UNSETENV 1
#define HAVE_STRTOK_R 1
#define HAVE_LSTAT 1
#define HAVE_READLINK 1
#define HAVE_VSNPRINTF 1
#define HAVE_STDARG_H 1
#define HAVE_STDINT_H 1
#define HAVE_STDIO_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STDBOOL_H 1
#define HAVE_STRDUP 1
#define HAVE_STRNDUP 1
#define HAVE_STRING_H 1
#define HAVE_STRINGS_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_SYS_TIME_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_TIMEGM 1
#define HAVE_GMTIME_R 1
#define HAVE_LOCALTIME_R 1
#define HAVE_TIMEZONE_VAR 1
#define HAVE_TZNAME 1
#define HAVE_TZSET 1
#define HAVE_UNISTD_H 1
#define HAVE_STRUCT_TM_TM_GMTOFF 1
#define LIBCDIO_SOURCE_PATH "."
#define STDC_HEADERS 1
#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif
#ifndef _LARGEFILE_SOURCE
#define _LARGEFILE_SOURCE 1
#endif

#endif /* LIBCDIO_CONFIG_H */
