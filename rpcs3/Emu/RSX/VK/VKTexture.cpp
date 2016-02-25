#include "stdafx.h"
#include "VKHelpers.h"
#include "../GCM.h"
#include "../RSXThread.h"
#include "../RSXTexture.h"
#include "../rsx_utils.h"
#include "../Common/TextureUtils.h"

namespace vk
{
	VkComponentMapping default_component_map()
	{
		VkComponentMapping result;
		result.a = VK_COMPONENT_SWIZZLE_A;
		result.r = VK_COMPONENT_SWIZZLE_R;
		result.g = VK_COMPONENT_SWIZZLE_G;
		result.b = VK_COMPONENT_SWIZZLE_B;

		return result;
	}

	VkImageSubresource default_image_subresource()
	{
		VkImageSubresource subres;
		subres.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		subres.mipLevel = 0;
		subres.arrayLayer = 0;

		return subres;
	}

	VkImageSubresourceRange default_image_subresource_range()
	{
		VkImageSubresourceRange subres;
		subres.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		subres.baseArrayLayer = 0;
		subres.baseMipLevel = 0;
		subres.layerCount = 1;
		subres.levelCount = 1;

		return subres;
	}

	void copy_texture(VkCommandBuffer cmd, texture &src, texture &dst, VkImageLayout srcLayout, VkImageLayout dstLayout, u32 width, u32 height, VkImageAspectFlagBits aspect)
	{
		VkImageSubresourceLayers a_src, a_dst;
		a_src.aspectMask = aspect;
		a_src.baseArrayLayer = 0;
		a_src.layerCount = 1;
		a_src.mipLevel = 0;

		a_dst = a_src;

		VkImageCopy rgn;
		rgn.extent.depth = 1;
		rgn.extent.width = width;
		rgn.extent.height = height;
		rgn.dstOffset = { 0, 0, 0 };
		rgn.srcOffset = { 0, 0, 0 };
		rgn.srcSubresource = a_src;
		rgn.dstSubresource = a_dst;

		if (srcLayout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)
			change_image_layout(cmd, src, srcLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, aspect);

		if (dstLayout != VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
			change_image_layout(cmd, dst, dstLayout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, aspect);

		vkCmdCopyImage(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rgn);

		if (srcLayout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)
			change_image_layout(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, srcLayout, aspect);
		
		if (dstLayout != VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
			change_image_layout(cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, dstLayout, aspect);
	}

	texture::texture(vk::swap_chain_image &img)
	{
		m_image_contents = img;
		m_view = img;
		m_sampler = nullptr;
		
		//We did not create this object, do not allow internal modification!
		owner = nullptr;
	}

	void texture::create(vk::render_device &device, VkFormat format, VkImageUsageFlags usage, u32 width, u32 height, u32 mipmaps, bool gpu_only, VkComponentMapping& swizzle)
	{
		owner = &device;

		VkImageTiling tiling = VK_IMAGE_TILING_OPTIMAL;

		if (usage & VK_IMAGE_USAGE_SAMPLED_BIT)
		{
			VkFormatProperties props;
			vkGetPhysicalDeviceFormatProperties(device.gpu(), format, &props);

			//Enable linear tiling if supported and we request a sampled image..
			if (props.linearTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)
				tiling = VK_IMAGE_TILING_LINEAR;
			else
				usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
		}

		//First create the image
		VkImageCreateInfo image_info;
		memset(&image_info, 0, sizeof(image_info));

		image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
		image_info.pNext = nullptr;
		image_info.imageType = VK_IMAGE_TYPE_2D;
		image_info.format = format;
		image_info.extent = { width, height, 1 };
		image_info.mipLevels = mipmaps;
		image_info.arrayLayers = 1;
		image_info.samples = VK_SAMPLE_COUNT_1_BIT;
		image_info.tiling = tiling;
		image_info.usage = usage;
		image_info.flags = 0;
		image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

		CHECK_RESULT(vkCreateImage(device, &image_info, nullptr, &m_image_contents));

		vkGetImageMemoryRequirements(device, m_image_contents, &m_memory_layout);
		vram_allocation.allocate_from_pool(device, m_memory_layout.size, m_memory_layout.memoryTypeBits);

		CHECK_RESULT(vkBindImageMemory(device, m_image_contents, vram_allocation, 0));

		VkImageViewCreateInfo view_info;
		view_info.format = format;
		view_info.image = m_image_contents;
		view_info.pNext = nullptr;
		view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
		view_info.components = swizzle;
		view_info.subresourceRange = default_image_subresource_range();
		view_info.flags = 0;

		if (usage == VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)
			view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT/* | VK_IMAGE_ASPECT_STENCIL_BIT*/;

		CHECK_RESULT(vkCreateImageView(device, &view_info, nullptr, &m_view));

		m_width = width;
		m_height = height;
		m_mipmaps = mipmaps;
		m_internal_format = format;
		m_flags = usage;
	}

	void texture::create(vk::render_device &device, VkFormat format, VkImageUsageFlags usage, u32 width, u32 height, u32 mipmaps, bool gpu_only)
	{
		create(device, format, usage, width, height, mipmaps, gpu_only, vk::default_component_map());
	}

	void texture::create(vk::render_device &device, VkFormat format, VkImageUsageFlags usage, u32 width, u32 height)
	{
		create(device, format, usage, width, height, 1, false);
	}

	VkSamplerAddressMode texture::vk_wrap_mode(u32 gcm_wrap)
	{
		switch (gcm_wrap)
		{
		case CELL_GCM_TEXTURE_WRAP: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
		case CELL_GCM_TEXTURE_MIRROR: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
		case CELL_GCM_TEXTURE_CLAMP_TO_EDGE: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		case CELL_GCM_TEXTURE_BORDER: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
		case CELL_GCM_TEXTURE_CLAMP: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		case CELL_GCM_TEXTURE_MIRROR_ONCE_CLAMP_TO_EDGE: return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
		case CELL_GCM_TEXTURE_MIRROR_ONCE_BORDER: return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
		case CELL_GCM_TEXTURE_MIRROR_ONCE_CLAMP: return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
		default:
			throw EXCEPTION("unhandled texture clamp mode 0x%X", gcm_wrap);
		}
	}

	float texture::max_aniso(u32 gcm_aniso)
	{
		switch (gcm_aniso)
		{
		case CELL_GCM_TEXTURE_MAX_ANISO_1: return 1.0f;
		case CELL_GCM_TEXTURE_MAX_ANISO_2: return 2.0f;
		case CELL_GCM_TEXTURE_MAX_ANISO_4: return 4.0f;
		case CELL_GCM_TEXTURE_MAX_ANISO_6: return 6.0f;
		case CELL_GCM_TEXTURE_MAX_ANISO_8: return 8.0f;
		case CELL_GCM_TEXTURE_MAX_ANISO_10: return 10.0f;
		case CELL_GCM_TEXTURE_MAX_ANISO_12: return 12.0f;
		case CELL_GCM_TEXTURE_MAX_ANISO_16: return 16.0f;
		}

		LOG_ERROR(RSX, "Texture anisotropy error: bad max aniso (%d).", gcm_aniso);
		return 1.0f;
	}

	void texture::sampler_setup(rsx::texture &tex, VkImageViewType type, VkComponentMapping swizzle)
	{
		VkSamplerAddressMode clamp_s = vk_wrap_mode(tex.wrap_s());
		VkSamplerAddressMode clamp_t = vk_wrap_mode(tex.wrap_t());
		VkSamplerAddressMode clamp_r = vk_wrap_mode(tex.wrap_r());

		VkSamplerCreateInfo sampler_info;
		sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
		sampler_info.addressModeU = clamp_s;
		sampler_info.addressModeV = clamp_t;
		sampler_info.addressModeW = clamp_r;
		sampler_info.anisotropyEnable = VK_FALSE;
		sampler_info.compareEnable = VK_FALSE;
		sampler_info.pNext = nullptr;
		sampler_info.unnormalizedCoordinates = VK_FALSE;
		sampler_info.mipLodBias = 0;
		sampler_info.maxAnisotropy = 0;
		sampler_info.flags = 0;
		sampler_info.maxLod = 0;
		sampler_info.minLod = 0;
		sampler_info.magFilter = VK_FILTER_LINEAR;
		sampler_info.minFilter = VK_FILTER_LINEAR;
		sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
		sampler_info.compareOp = VK_COMPARE_OP_NEVER;
		sampler_info.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;

		CHECK_RESULT(vkCreateSampler((*owner), &sampler_info, nullptr, &m_sampler));
	}

	void texture::init(rsx::texture& tex)
	{
		if (!m_sampler)
			sampler_setup(tex, VK_IMAGE_VIEW_TYPE_2D, default_component_map());

		VkImageSubresource subres;
		subres.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		subres.mipLevel = 0;
		subres.arrayLayer = 0;

		VkSubresourceLayout layout;
		u8 *data;

		VkFormatProperties props;
		vk::physical_device dev = owner->gpu();
		vkGetPhysicalDeviceFormatProperties(dev, m_internal_format, &props);

		if (props.linearTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)
		{
			vkGetImageSubresourceLayout((*owner), m_image_contents, &subres, &layout);
			
			u16 alignment = 4096;
			while (alignment > 1)
			{
				//Test if is wholly divisible by alignment..
				if (!(layout.rowPitch & (alignment-1)))
					break;

				alignment >>= 1;
			}

			u32 buffer_size = get_placed_texture_storage_size(tex, alignment, alignment);
			if (buffer_size != layout.size)
				throw EXCEPTION("Bad texture alignment computation!");

			CHECK_RESULT(vkMapMemory((*owner), vram_allocation, 0, m_memory_layout.size, 0, (void**)&data));

			gsl::span<gsl::byte> mapped{ (gsl::byte*)(data+layout.offset), gsl::narrow<int>(buffer_size) };
			upload_placed_texture(mapped, tex, alignment);
			
			vkUnmapMemory((*owner), vram_allocation);
		}
		else
		{
			LOG_ERROR(RSX, "Texture upload failed: staging texture required!");
		}
	}

	void texture::destroy()
	{
		if (!owner) return;

		if (m_sampler)
			vkDestroySampler((*owner), m_sampler, nullptr);

		//Destroy all objects managed by this object
		vkDestroyImageView((*owner), m_view, nullptr);
		vkDestroyImage((*owner), m_image_contents, nullptr);

		vram_allocation.destroy();

		owner = nullptr;
		m_sampler = nullptr;
		m_view = nullptr;
		m_image_contents = nullptr;
	}

	const VkFormat texture::get_format()
	{
		return m_internal_format;
	}

	texture::operator VkImage()
	{
		return m_image_contents;
	}

	texture::operator VkImageView()
	{
		return m_view;
	}

	texture::operator VkSampler()
	{
		return m_sampler;
	}
}
