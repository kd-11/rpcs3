#pragma once

#include "common.h"

namespace gl
{
	static inline void insert_texture_barrier()
	{
		auto& caps = gl::get_driver_caps();

		if (caps.ARB_texture_barrier_supported)
			glTextureBarrier();
		else if (caps.NV_texture_barrier_supported)
			glTextureBarrierNV();
	}
}
