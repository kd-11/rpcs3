#pragma once

#include "../VulkanAPI.h"
#include "device.h"
#include "commands.h"
#include "image.h"
#include "framebuffer_object.hpp"
#include <concepts>

namespace vk
{
	struct renderpass_t
	{
		virtual void begin(VkCommandBuffer cmd, const framebuffer& fbo, const VkRect2D& viewport) = 0;
		virtual void end(VkCommandBuffer cmd) = 0;
		virtual VkRenderPass get() const = 0;
	};

	class renderpass_static : public renderpass_t
	{
		const vk::render_device* m_device = nullptr;
		VkRenderPass m_value = VK_NULL_HANDLE;
		VkRenderPassCreateInfo m_create_info{};

	public:
		renderpass_static(const vk::render_device* pdev, const VkRenderPassCreateInfo& create_info);
		~renderpass_static();

		void begin(VkCommandBuffer cmd, const framebuffer& fbo, const VkRect2D& viewport) override;
		void end(VkCommandBuffer cmd) override;

		VkRenderPass get() const override { return m_value; }
	};

	class renderpass_dynamic : public renderpass_t
	{
		VkRenderingInfoKHR m_render_info{};
		std::array<VkRenderingAttachmentInfoKHR, 4> m_color_attachment_info{{}};
		VkRenderingAttachmentInfoKHR m_depth_stencil_attachment_info{};

	public:
		renderpass_dynamic();
		~renderpass_dynamic();

		void begin(VkCommandBuffer cmd, const framebuffer& fbo, const VkRect2D& viewport) override;
		void end(VkCommandBuffer cmd) override;

		VkRenderPass get() const override { return VK_NULL_HANDLE; }
	};
}
