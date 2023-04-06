#include "stdafx.h"
#include "Runtime.h"

#include "Emu/RSX/VK/vkutils/buffer_object.h"
#include "Emu/RSX/VK/vkutils/descriptors.h"
#include "Emu/RSX/VK/vkutils/memory.h"
#include "Emu/RSX/VK/VKProgramPipeline.h"
#include "Emu/RSX/VK/VKPipelineCompiler.h"
#include "Emu/RSX/VK/VKGSRender.h"

namespace spv
{
	executable::executable(const build_info& compiler_input)
	{
		compute = std::make_unique<vk::glsl::shader>();
		compute->create(::glsl::glsl_compute_program, compiler_input.source);
		const auto handle = compute->compile();

		VkPipelineShaderStageCreateInfo shader_stage{};
		shader_stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		shader_stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
		shader_stage.module = handle;
		shader_stage.pName = "main";

		VkComputePipelineCreateInfo info{};
		info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
		info.stage = shader_stage;
		info.layout = compiler_input.layout;
		info.basePipelineIndex = -1;
		info.basePipelineHandle = VK_NULL_HANDLE;

		auto compiler = vk::get_pipe_compiler();
		prog = compiler->compile(info, compiler_input.layout, vk::pipe_compiler::COMPILE_INLINE);
	}

	executable::~executable()
	{
		prog.reset();
		compute->destroy();
	}
}
