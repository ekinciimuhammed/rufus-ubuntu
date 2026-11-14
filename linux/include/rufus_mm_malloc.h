/* Rufus for Linux: _mm_malloc()/_mm_free() for code that gets them from MSVC's <malloc.h> */
#pragma once
#if defined(__x86_64__) || defined(__i386__)
#include <mm_malloc.h>
#else
#include <stdlib.h>
static inline void* _mm_malloc(size_t size, size_t align)
{
	void* p = NULL;
	if (align < sizeof(void*))
		align = sizeof(void*);
	return (posix_memalign(&p, align, size) == 0) ? p : NULL;
}
static inline void _mm_free(void* p) { free(p); }
#endif
