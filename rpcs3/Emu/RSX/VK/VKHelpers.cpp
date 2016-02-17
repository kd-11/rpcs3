#include "stdafx.h"
#include "VKHelpers.h"

namespace vk
{
	context *g_current_vulkan_ctx = nullptr;
	render_device *g_current_renderer = nullptr;

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
		return g_current_renderer;
	}

	void set_current_renderer(const vk::render_device &device)
	{
		g_current_renderer = (vk::render_device *)&device;
	}
}