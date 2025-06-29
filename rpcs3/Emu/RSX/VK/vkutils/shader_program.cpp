#include "stdafx.h"
#include "shader_program.h"

namespace vk
{
	VkShaderModule shader::create(const std::vector<u32>& binary)
	{
		ensure(!m_handle);
		m_compiled = binary;

		VkShaderModuleCreateInfo vs_info;
		vs_info.codeSize = m_compiled.size() * sizeof(u32);
		vs_info.pNext    = nullptr;
		vs_info.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
		vs_info.pCode    = m_compiled.data();
		vs_info.flags    = 0;

		vkCreateShaderModule(*g_render_device, &vs_info, nullptr, &m_handle);
		return m_handle;
	}

	void shader::destroy()
	{
		m_compiled.clear();

		if (m_handle)
		{
			vkDestroyShaderModule(*g_render_device, m_handle, nullptr);
			m_handle = VK_NULL_HANDLE;
		}
	}

	pipeline::pipeline(VkDevice dev, const VkGraphicsPipelineCreateInfo& create_info)
		: m_device(dev)
		, m_info(create_info)
	{
		CHECK_RESULT(vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &create_info, nullptr, &m_pipeline));
	}

	pipeline::pipeline(VkDevice dev, const VkComputePipelineCreateInfo& create_info)
		: m_device(dev)
		, m_info(create_info)
	{
		CHECK_RESULT(vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1, &create_info, nullptr, &m_pipeline));
	}

	pipeline::~pipeline()
	{
		vkDestroyPipeline(m_device, m_pipeline, nullptr);
	}
}
