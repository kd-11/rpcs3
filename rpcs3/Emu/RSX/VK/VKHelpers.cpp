#include "stdafx.h"
#include "VKHelpers.h"

namespace vk
{
	context *g_current_vulkan_ctx = nullptr;
	render_device g_current_renderer;

	buffer g_null_buffer;
	texture g_null_texture;

	VkSampler g_null_sampler = nullptr;
	VkImageView g_null_image_view = nullptr;

	VKAPI_ATTR void *VKAPI_CALL mem_realloc(void *pUserData, void *pOriginal, size_t size, size_t alignment, VkSystemAllocationScope allocationScope)
	{
		return realloc(pOriginal, size);
	}

	VKAPI_ATTR void *VKAPI_CALL mem_alloc(void *pUserData, size_t size, size_t alignment, VkSystemAllocationScope allocationScope)
	{
		return _aligned_malloc(size, alignment);
	}

	VKAPI_ATTR void VKAPI_CALL mem_free(void *pUserData, void *pMemory)
	{
		_aligned_free(pMemory);
	}

	VkAllocationCallbacks default_callbacks()
	{
		VkAllocationCallbacks callbacks;
		callbacks.pfnAllocation = vk::mem_alloc;
		callbacks.pfnFree = vk::mem_free;
		callbacks.pfnReallocation = vk::mem_realloc;

		return callbacks;
	}

	VkBuffer null_buffer()
	{
		if (g_null_buffer.size())
			return g_null_buffer;

		g_null_buffer.create(g_current_renderer, 32, VK_FORMAT_R32_SFLOAT, VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT);
		return g_null_buffer;
	}

	VkSampler null_sampler()
	{
		if (g_null_sampler)
			return g_null_sampler;

		VkSamplerCreateInfo sampler_info;
		memset(&sampler_info, 0, sizeof(sampler_info));

		sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
		sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
		sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
		sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
		sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
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

		vkCreateSampler(g_current_renderer, &sampler_info, nullptr, &g_null_sampler);
		return g_null_sampler;
	}

	VkImageView null_image_view()
	{
		if (g_null_image_view)
			return g_null_image_view;

		g_null_texture.create(g_current_renderer, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT, 4, 4);
		g_null_image_view = g_null_texture;
		return g_null_image_view;
	}

	VkBufferView null_buffer_view()
	{
		if (g_null_buffer.size())
			return g_null_buffer;

		g_null_buffer.create(g_current_renderer, 32, VK_FORMAT_R32_SFLOAT, VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT);
		return g_null_buffer;
	}

	void destroy_global_resources()
	{
		g_null_buffer.destroy();
		g_null_texture.destroy();

		if (g_null_sampler)
			vkDestroySampler(g_current_renderer, g_null_sampler, nullptr);

		g_null_sampler = nullptr;
		g_null_image_view = nullptr;
	}

	void set_current_thread_ctx(const vk::context &ctx)
	{
		g_current_vulkan_ctx = (vk::context *)&ctx;
	}

	context *get_current_thread_ctx()
	{
		return g_current_vulkan_ctx;
	}

	vk::render_device *get_current_renderer()
	{
		return &g_current_renderer;
	}

	void set_current_renderer(const vk::render_device &device)
	{
		g_current_renderer = device;
	}

	void change_image_layout(VkCommandBuffer cmd, VkImage image, VkImageLayout current_layout, VkImageLayout new_layout, VkImageAspectFlags aspect_flags)
	{
		//Prepare an image to match the new layout..
		VkImageSubresourceRange range = default_image_subresource_range();
		range.aspectMask = aspect_flags;

		VkImageMemoryBarrier barrier;
		barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		barrier.pNext = nullptr;
		barrier.newLayout = new_layout;
		barrier.oldLayout = current_layout;
		barrier.image = image;
		barrier.srcAccessMask = 0;
		barrier.dstAccessMask = 0;
		barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.subresourceRange = range;

		//This section is lifted from the samples...
		if (new_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
			barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

		if (new_layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
			barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

		if (new_layout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL)
			barrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

		if (new_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
			barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_INPUT_ATTACHMENT_READ_BIT;

		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
	}

	VKAPI_ATTR VkBool32 VKAPI_CALL dbgFunc(VkFlags msgFlags, VkDebugReportObjectTypeEXT objType,
											uint64_t srcObject, size_t location, int32_t msgCode,
											const char *pLayerPrefix, const char *pMsg, void *pUserData)
	{
		if (msgFlags & VK_DEBUG_REPORT_ERROR_BIT_EXT)
		{
			LOG_ERROR(RSX, "ERROR: [%s] Code %d : %s", pLayerPrefix, msgCode, pMsg);
		}
		else if (msgFlags & VK_DEBUG_REPORT_WARNING_BIT_EXT)
		{
			LOG_WARNING(RSX, "WARNING: [%s] Code %d : %s", pLayerPrefix, msgCode, pMsg);
		}
		else
		{
			return false;
		}

		//Let the app crash..
		return false;
	}

	VkBool32 BreakCallback(VkFlags msgFlags, VkDebugReportObjectTypeEXT objType,
							uint64_t srcObject, size_t location, int32_t msgCode,
							const char *pLayerPrefix, const char *pMsg, void *pUserData)
	{
		DebugBreak();

		return false;
	}
}