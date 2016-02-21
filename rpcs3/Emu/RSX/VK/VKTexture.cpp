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


	texture::texture(vk::swap_chain_image &img)
	{
		m_image_contents = img;
		m_view = img;
		m_sampler = nullptr;
		
		//We did not create this object, do not allow internal modification!
		owner = nullptr;
	}

	void texture::create(vk::render_device &device, VkFormat format, VkImageUsageFlags usage, u32 width, u32 height, u32 mipmaps, bool gpu_only)
	{
		owner = &device;

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
		image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
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
		view_info.components = default_component_map();
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

	void texture::create(vk::render_device &device, VkFormat format, VkImageUsageFlags usage, u32 width, u32 height)
	{
		create(device, format, usage, width, height, 1, false);
	}

	u32 texture::vk_wrap_mode(u32 gcm_wrap)
	{
		return 0;
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

	void texture::sampler_setup(VkSamplerAddressMode clamp_mode, VkImageViewType type, VkComponentMapping swizzle)
	{
		VkSamplerCreateInfo sampler_info;
		sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
		sampler_info.addressModeU = clamp_mode;
		sampler_info.addressModeV = clamp_mode;
		sampler_info.addressModeW = clamp_mode;
		sampler_info.anisotropyEnable = VK_FALSE;
		sampler_info.compareEnable = VK_FALSE;
		sampler_info.pNext = nullptr;
		sampler_info.unnormalizedCoordinates = VK_FALSE;
		sampler_info.mipLodBias = 0;
		sampler_info.maxAnisotropy = 0;
		sampler_info.magFilter = VK_FILTER_NEAREST;
		sampler_info.minFilter = VK_FILTER_NEAREST;
		sampler_info.compareOp = VK_COMPARE_OP_NEVER;
		sampler_info.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;

		CHECK_RESULT(vkCreateSampler((*owner), &sampler_info, nullptr, &m_sampler));
	}

	void texture::init(int index, rsx::texture& tex)
	{
		//TODO check if mappable and throw otherwise

		if (!m_sampler)
			sampler_setup(VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_IMAGE_VIEW_TYPE_2D, default_component_map());

		VkImageSubresource subres;
		subres.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		subres.mipLevel = 0;
		subres.arrayLayer = 0;

		VkSubresourceLayout layout;
		void *data;

		vkGetImageSubresourceLayout((*owner), m_image_contents, &subres, &layout);
		CHECK_RESULT(vkMapMemory((*owner), vram_allocation, 0, m_memory_layout.size, 0, &data));

		//Should write bytes to the texture.
		//For now, just clear
		memset(data, 0x80, m_memory_layout.size);

		vkUnmapMemory((*owner), vram_allocation);
	}

	void texture::destroy()
	{
		if (!owner) return;

		//Destroy all objects managed by this object
		vkDestroyImageView((*owner), m_view, nullptr);
		vkDestroySampler((*owner), m_sampler, nullptr);
		vkDestroyImage((*owner), m_image_contents, nullptr);

		vram_allocation.destroy();
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
