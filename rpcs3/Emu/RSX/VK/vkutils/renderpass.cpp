#include "stdafx.h"
#include "renderpass.h"
#include "image_helpers.h"

namespace vk
{
	renderpass_static::renderpass_static(const vk::render_device* pdev, const VkRenderPassCreateInfo& create_info)
		: m_device(pdev), m_create_info(create_info)
	{
		vkCreateRenderPass(*m_device, &create_info, nullptr, &m_value);
	}

	renderpass_static::~renderpass_static()
	{
		vkDestroyRenderPass(*m_device, m_value, nullptr);
	}

	void renderpass_static::begin(VkCommandBuffer cmd, const framebuffer& fbo, const VkRect2D& viewport)
	{
		VkRenderPassBeginInfo begin_info
		{
			.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
			.renderPass = m_value,
			.framebuffer = fbo.value,
			.renderArea = viewport
		};
		vkCmdBeginRenderPass(cmd, &begin_info, VK_SUBPASS_CONTENTS_INLINE);
	}

	void renderpass_static::end(VkCommandBuffer cmd)
	{
		vkCmdEndRenderPass(cmd);
	}

	renderpass_dynamic::renderpass_dynamic(const vk::render_device* pdev)
		: m_device(pdev)
	{
		m_render_info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO_KHR;
		m_render_info.layerCount = 1;

		auto prep_attachment = [](VkRenderingAttachmentInfoKHR& att)
		{
			att.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR;
			att.resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			att.resolveImageView = VK_NULL_HANDLE;
			att.resolveMode = VK_RESOLVE_MODE_NONE_KHR;
			att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
			att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		};

		prep_attachment(m_depth_stencil_attachment_info);
		for (auto& att : m_color_attachment_info)
		{
			prep_attachment(att);
		}
	}

	renderpass_dynamic::~renderpass_dynamic()
	{
		// NOP
	}

	void renderpass_dynamic::begin(VkCommandBuffer cmd, const framebuffer& fbo, const VkRect2D& viewport)
	{
		m_render_info.renderArea = viewport;
		m_render_info.colorAttachmentCount = ::size32(fbo.attachments);
		m_render_info.pDepthAttachment = nullptr;
		m_render_info.pColorAttachments = nullptr;
		m_render_info.pStencilAttachment = nullptr;

		if (fbo.aspect_flags & VK_IMAGE_ASPECT_DEPTH_BIT)
		{
			m_render_info.colorAttachmentCount--;
			m_render_info.pDepthAttachment = &m_depth_stencil_attachment_info;
		}

		if (fbo.aspect_flags & VK_IMAGE_ASPECT_COLOR_BIT)
		{
			m_render_info.pColorAttachments = m_color_attachment_info.data();
		}

		if (fbo.aspect_flags & VK_IMAGE_ASPECT_STENCIL_BIT)
		{
			m_render_info.pStencilAttachment = &m_depth_stencil_attachment_info;
		}

		u32 color_index = 0;
		for (const auto& att : fbo.attachments)
		{
			const auto rsc = att->image();
			const auto aspect = rsc ? rsc->aspect() : vk::get_aspect_flags(att->info.format);
			const auto layout = rsc ? rsc->current_layout : (aspect == VK_IMAGE_ASPECT_COLOR_BIT ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);

			if (aspect == VK_IMAGE_ASPECT_COLOR_BIT)
			{
				auto& info = m_color_attachment_info[color_index++];
				info.imageView = att->value;
				info.imageLayout = layout;
			}
			else
			{
				m_depth_stencil_attachment_info.imageView = att->value;
				m_depth_stencil_attachment_info.imageLayout = layout;
			}
		}

		m_device->vk1_3._vkCmdBeginRenderingKHR(cmd, &m_render_info);
	}

	void renderpass_dynamic::end(VkCommandBuffer cmd)
	{
		m_device->vk1_3._vkCmdEndRenderingKHR(cmd);
	}
}
