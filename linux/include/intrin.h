/* Rufus for Linux: replacement for MSVC's <intrin.h> */
#pragma once
#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#include <cpuid.h>
/* MSVC's __cpuid(int[4], leaf) (GCC's <cpuid.h> already provides __cpuidex) */
#undef __cpuid
static inline void __cpuid(int info[4], int leaf)
{
	__cpuid_count(leaf, 0, info[0], info[1], info[2], info[3]);
}
#elif defined(__aarch64__) || defined(__arm__)
#include <arm_acle.h>
#endif
