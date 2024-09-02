#pragma once

#include "util/types.hpp"

#ifdef _M_X64
#ifdef _MSC_VER
extern "C" u64 __rdtsc();
#else
#include <immintrin.h>
#endif
#elif defined(_MSC_VER)
#include <arm64intr.h>
#ifndef ARM64_CNTVCT
#define ARM64_CNTVCT ARM64_SYSREG(3, 3, 14, 0, 2)
#endif
#endif


namespace utils
{
	inline u64 get_tsc()
	{
#if defined(ARCH_ARM64) && !defined(_MSC_VER)
		u64 r = 0;
		__asm__ volatile("mrs %0, cntvct_el0" : "=r" (r));
		return r;
#elif defined(_MSC_VER)
		return _ReadStatusReg(ARM64_CNTVCT);
#elif defined(_M_X64)
		return __rdtsc();
#elif defined(ARCH_X64)
		return __builtin_ia32_rdtsc();
#else
#error "Missing utils::get_tsc() implementation"
#endif
	}
}
