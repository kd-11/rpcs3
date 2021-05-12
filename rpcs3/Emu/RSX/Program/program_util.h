#pragma once

#include "util/types.hpp"
#include "../gcm_enums.h"

namespace rsx
{
	// Convert u16 to u32
	static u32 duplicate_and_extend(u16 bits)
	{
		u32 x = bits;

		x = (x | (x << 8)) & 0x00FF00FF;
		x = (x | (x << 4)) & 0x0F0F0F0F;
		x = (x | (x << 2)) & 0x33333333;
		x = (x | (x << 1)) & 0x55555555;

		return x | (x << 1);
	}

	struct fragment_program_texture_state
	{
		u16 unnormalized_coords = 0;
		u16 redirected_textures = 0;
		u16 shadow_textures = 0;
		u32 texture_dimensions = 0;

		void import(const fragment_program_texture_state& other, u16 mask)
		{
			unnormalized_coords = other.unnormalized_coords & mask;
			redirected_textures = other.redirected_textures & mask;
			shadow_textures = other.shadow_textures & mask;
			texture_dimensions = other.texture_dimensions & duplicate_and_extend(mask);
		}

		void set_dimension(texture_dimension_extended type, u32 index)
		{
			const auto offset = (index * 2);
			const auto mask = 3 << offset;
			texture_dimensions &= ~mask;
			texture_dimensions |= static_cast<u32>(type) << offset;
		}

		bool operator == (const fragment_program_texture_state& other) const
		{
			return texture_dimensions == other.texture_dimensions &&
				redirected_textures == other.redirected_textures &&
				shadow_textures == other.shadow_textures &&
				unnormalized_coords == other.unnormalized_coords;
		}
	};

	struct vertex_program_texture_state
	{
		u32 texture_dimensions = 0;

		void import(const vertex_program_texture_state& other, u16 mask)
		{
			texture_dimensions = other.texture_dimensions & duplicate_and_extend(mask);
		}

		void set_dimension(texture_dimension_extended type, u32 index)
		{
			const auto offset = (index * 2);
			const auto mask = 3 << offset;
			texture_dimensions &= ~mask;
			texture_dimensions |= static_cast<u32>(type) << offset;
		}

		bool operator == (const vertex_program_texture_state& other) const
		{
			return texture_dimensions == other.texture_dimensions;
		}
	};
}
