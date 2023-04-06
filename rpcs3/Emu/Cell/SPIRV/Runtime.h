#pragma once

#include <util/types.hpp>

#include "Emu/RSX/VK/VulkanAPI.h"

namespace vk
{
	namespace glsl
	{
		class shader;
		class program;
	}
}

namespace spv
{
	struct build_info
	{
		VkPipelineLayout layout;
		std::string source;
	};

	// TODO: This is still rather clunky
	struct executable
	{
		std::unique_ptr<vk::glsl::shader> compute;
		std::unique_ptr<vk::glsl::program> prog;

		// PC
		int start_pc = -1;
		int end_pc = -1;

		// Flags
		bool is_dynamic_branch_block = false;
		bool issues_memory_op = false;

		executable(const build_info& compiler_input);
		~executable();
	};
}
