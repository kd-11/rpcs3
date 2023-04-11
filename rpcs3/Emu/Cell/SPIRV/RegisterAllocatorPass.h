#pragma once

#include "Compiler.h"

namespace spv
{
	struct register_initializer_t
	{
		std::string old_name;
		std::string new_name;
		bool require_load = false;
		bool require_sync = false;
		bool is_const = false;
	};

	struct register_allocator_output
	{
		std::unordered_map<std::string, register_initializer_t> temp_regs;
	};

	register_allocator_output run_allocator_pass(spv::function_t& function);
}
