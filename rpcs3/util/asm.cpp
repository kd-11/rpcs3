#include "stdafx.h"

#if defined(_MSC_VER) && !defined(_M_X64)
#include <3rdparty/libdivide/include/libdivide.h>

s64 _div128(s64, s64, s64, s64*)
{
	return 0;
}

u64 _udiv128(u64, u64, u64, u64*)
{
	return 0;
}

#endif
