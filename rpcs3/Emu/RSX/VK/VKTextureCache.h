#pragma once
#include "stdafx.h"
#include "VKGSRender.h"
#include "../Common/TextureUtils.h"

namespace vk
{
	struct cached_texture_object
	{
		u32 native_rsx_address;
		u32 native_rsx_size;
		
		u16 width;
		u16 height;
		u16 depth;
		u16 mipmaps;
		
		vk::texture uploaded_texture;

		u64  protected_rgn_start;
		u64  protected_rgn_end;
		
		bool exists = false;
		bool locked = false;
		bool dirty = true;
	};

	class texture_cache
	{
	private:
		std::vector<cached_texture_object> m_cache;

	public:

		texture_cache() {}
		~texture_cache() {}

		void destroy()
		{
			for (cached_texture_object &tex : m_cache)
			{
				tex.uploaded_texture.destroy();
				tex.exists = false;
			}
		}

		cached_texture_object& find_cached_texture(u32 rsx_address, u32 rsx_size, bool confirm_dimensions=false, u16 width=0, u16 height=0, u16 mipmaps=0)
		{
			for (cached_texture_object &tex: m_cache)
			{
				if (!tex.dirty && tex.exists &&
					tex.native_rsx_address == rsx_address &&
					tex.native_rsx_size == rsx_size)
				{
					if (!confirm_dimensions) return tex;
					
					if (tex.width == width && tex.height == height && tex.mipmaps == mipmaps)
						return tex;
					else
					{
						LOG_ERROR(RSX, "Cached object for address 0x%X was found, but it does not match stored parameters.");
						LOG_ERROR(RSX, "%d x %d vs %d x %d", width, height, tex.width, tex.height);
					}
				}
			}

			for (cached_texture_object &tex : m_cache)
			{
				if (tex.dirty)
				{
					tex.uploaded_texture.destroy();
					tex.exists = false;

					return tex;
				}
			}

			cached_texture_object object;
			m_cache.push_back(object);

			return m_cache[m_cache.size() - 1];
		}

		vk::texture& upload_texture(command_buffer cmd, rsx::texture &tex)
		{
			const u32 texaddr = rsx::get_address(tex.offset(), tex.location());
			const u32 range = (u32)get_texture_size(tex);

			cached_texture_object& cto = find_cached_texture(texaddr, range, false);
			if (cto.exists && !cto.dirty)
				return cto.uploaded_texture;

			u32 raw_format = tex.format();
			u32 format = raw_format & ~(CELL_GCM_TEXTURE_LN | CELL_GCM_TEXTURE_UN);

			VkFormat vk_format = get_compatible_sampler_format(format);

			cto.uploaded_texture.create(*vk::get_current_renderer(), vk_format, VK_IMAGE_USAGE_SAMPLED_BIT, tex.width(), tex.height());
			cto.uploaded_texture.init(tex);

			change_image_layout(cmd, cto.uploaded_texture, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);

			cto.exists = true;
			cto.dirty = false;
			cto.native_rsx_address = texaddr;
			cto.native_rsx_size = range;

			return cto.uploaded_texture;
		}
	};
}
