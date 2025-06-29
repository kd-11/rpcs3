#pragma once

#include "commands.h"

#include <string>
#include <vector>
#include <variant>
#include <vulkan/vulkan_core.h>

namespace vk
{
	class shader
	{
	public:
		shader() = default;
		virtual ~shader() = default;

		VkShaderModule create(const std::vector<u32>& binary);
		void destroy();

		const std::vector<u32> bytecode() const { return m_compiled; }
		inline VkShaderModule handle() const { return m_handle; }

	protected:
		VkShaderModule m_handle = VK_NULL_HANDLE;
		std::vector<u32> m_compiled;
	};

	class pipeline
	{
	public:
		pipeline(VkDevice dev, const VkGraphicsPipelineCreateInfo& create_info);
		pipeline(VkDevice dev, const VkComputePipelineCreateInfo& create_info);
		pipeline(const pipeline&) = delete;
		pipeline(pipeline&& other) = delete;
		virtual ~pipeline();

		inline VkPipeline handle() const { return m_pipeline; }

	protected:
		VkDevice m_device = VK_NULL_HANDLE;
		VkPipeline m_pipeline = VK_NULL_HANDLE;
		std::variant<VkGraphicsPipelineCreateInfo, VkComputePipelineCreateInfo> m_info;
	};
}
