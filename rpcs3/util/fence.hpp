#pragma once

#include "util/types.hpp"

#ifdef _M_X64
#ifdef _MSC_VER
extern "C" void _mm_lfence();
#else
#include <immintrin.h>
#endif
#endif

namespace utils
{
	inline void lfence()
	{
#ifdef _M_X64
		_mm_lfence();
#elif defined(ARCH_X64)
		__builtin_ia32_lfence();
#elif defined(_M_ARM64)
		__dmb(_ARM64_BARRIER_ISHLD);
		__dsb(_ARM64_BARRIER_ISHLD);
#elif defined(ARCH_ARM64)
		__asm__ volatile("dmb ishld");
		__asm__ volatile("dsb ishld");
#else
#error "Missing lfence() implementation"
#endif
	}
}
