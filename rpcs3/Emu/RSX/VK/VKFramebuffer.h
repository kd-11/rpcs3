#pragma once

#include "vkutils/framebuffer_object.hpp"

namespace vk
{
	struct renderpass_t;

	struct framebuffer_holder : public vk::framebuffer, public rsx::ref_counted
	{
		using framebuffer::framebuffer;
	};

	vk::framebuffer_holder* get_framebuffer(VkDevice dev, u16 width, u16 height, VkBool32 has_input_attachments, renderpass_t* renderpass, const std::vector<vk::image*>& image_list);
	vk::framebuffer_holder* get_framebuffer(VkDevice dev, u16 width, u16 height, VkBool32 has_input_attachments, renderpass_t* renderpass, VkFormat format, VkImage attachment);

	void remove_unused_framebuffers();
	void clear_framebuffer_cache();
}
