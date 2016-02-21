#pragma once
#include "../Common/ShaderParam.h"
#include "VKHelpers.h"

namespace vk
{
	std::string getFloatTypeNameImpl(size_t elementCount);
	std::string getFunctionImpl(FUNCTION f);
	std::string compareFunctionImpl(COMPARE f, const std::string &Op0, const std::string &Op1);
	void insert_glsl_legacy_function(std::ostream& OS);

	bool compile_glsl_to_spv(std::string& shader, glsl::program_domain domain, std::vector<u32> &spv);
}