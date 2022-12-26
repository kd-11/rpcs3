#pragma once

#include "VulkanAPI.h"
#include "vkutils/device.h"
#include "vkutils/renderpass.h"
#include "Utilities/geometry.h"

namespace vk
{
	class image;

	u64 get_renderpass_key(const std::vector<vk::image*>& images, const std::vector<u8>& input_attachment_ids = {});
	u64 get_renderpass_key(const std::vector<vk::image*>& images, u64 previous_key);
	u64 get_renderpass_key(VkFormat surface_format);
	renderpass_t* get_renderpass(const render_device* pdev, u64 renderpass_key);

	void clear_renderpass_cache(const render_device* pdev);

	// Renderpass scope management helpers.
	// NOTE: These are not thread safe by design.
	void begin_renderpass(const render_device* pdev, VkCommandBuffer cmd, u64 renderpass_key, const framebuffer* target, const coordu& framebuffer_region);
	void begin_renderpass(VkCommandBuffer cmd, renderpass_t* pass, const framebuffer* target, const coordu& framebuffer_region);
	void end_renderpass(VkCommandBuffer cmd);
	bool is_renderpass_open(VkCommandBuffer cmd);

	using renderpass_op_callback_t = std::function<void(VkCommandBuffer, renderpass_t*, const framebuffer*)>;
	void renderpass_op(VkCommandBuffer cmd, const renderpass_op_callback_t& op);
}
