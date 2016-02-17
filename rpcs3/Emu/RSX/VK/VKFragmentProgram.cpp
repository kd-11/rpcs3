#include "stdafx.h"
#include "Emu/Memory/Memory.h"
#include "Emu/System.h"
#include "VKFragmentProgram.h"

#include "VKCommonDecompiler.h"
#include "VKHelpers.h"
#include "../GCM.h"

std::string VKFragmentDecompilerThread::getFloatTypeName(size_t elementCount)
{
	return getFloatTypeNameImpl(elementCount);
}

std::string VKFragmentDecompilerThread::getFunction(FUNCTION f)
{
	return getFunctionImpl(f);
}

std::string VKFragmentDecompilerThread::saturate(const std::string & code)
{
	return "clamp(" + code + ", 0., 1.)";
}

std::string VKFragmentDecompilerThread::compareFunction(COMPARE f, const std::string &Op0, const std::string &Op1)
{
	return compareFunctionImpl(f, Op0, Op1);
}

void VKFragmentDecompilerThread::insertHeader(std::stringstream & OS)
{
	OS << "#version 420" << std::endl;
}

void VKFragmentDecompilerThread::insertIntputs(std::stringstream & OS)
{
	for (const ParamType& PT : m_parr.params[PF_PARAM_IN])
	{
		for (const ParamItem& PI : PT.items)
			OS << "in " << PT.type << " " << PI.name << ";" << std::endl;
	}
}

void VKFragmentDecompilerThread::insertOutputs(std::stringstream & OS)
{
	const std::pair<std::string, std::string> table[] =
	{
		{ "ocol0", m_ctrl & CELL_GCM_SHADER_CONTROL_32_BITS_EXPORTS ? "r0" : "h0" },
		{ "ocol1", m_ctrl & CELL_GCM_SHADER_CONTROL_32_BITS_EXPORTS ? "r2" : "h4" },
		{ "ocol2", m_ctrl & CELL_GCM_SHADER_CONTROL_32_BITS_EXPORTS ? "r3" : "h6" },
		{ "ocol3", m_ctrl & CELL_GCM_SHADER_CONTROL_32_BITS_EXPORTS ? "r4" : "h8" },
	};

	for (int i = 0; i < sizeof(table) / sizeof(*table); ++i)
	{
		if (m_parr.HasParam(PF_PARAM_NONE, "vec4", table[i].second))
			OS << "out vec4 " << table[i].first << ";" << std::endl;
	}
}

void VKFragmentDecompilerThread::insertConstants(std::stringstream & OS)
{
	for (const ParamType& PT : m_parr.params[PF_PARAM_UNIFORM])
	{
		if (PT.type != "sampler1D" &&
			PT.type != "sampler2D" &&
			PT.type != "sampler3D" &&
			PT.type != "samplerCube")
			continue;

		for (const ParamItem& PI : PT.items)
		{
			std::string samplerType = PT.type;
			int index = atoi(&PI.name.data()[3]);

			if (m_prog.unnormalized_coords & (1 << index))
				samplerType = "sampler2DRect";

			OS << "uniform " << samplerType << " " << PI.name << ";" << std::endl;
		}
	}

	OS << "layout(std140, binding = 2) uniform FragmentConstantsBuffer" << std::endl;
	OS << "{" << std::endl;

	for (const ParamType& PT : m_parr.params[PF_PARAM_UNIFORM])
	{
		if (PT.type == "sampler1D" ||
			PT.type == "sampler2D" ||
			PT.type == "sampler3D" ||
			PT.type == "samplerCube")
			continue;

		for (const ParamItem& PI : PT.items)
			OS << "	" << PT.type << " " << PI.name << ";" << std::endl;
	}

	// A dummy value otherwise it's invalid to create an empty uniform buffer
	OS << "	vec4 void_value;" << std::endl;
	OS << "};" << std::endl;
}

void VKFragmentDecompilerThread::insertMainStart(std::stringstream & OS)
{
	insert_glsl_legacy_function(OS);

	OS << "void main ()" << std::endl;
	OS << "{" << std::endl;

	for (const ParamType& PT : m_parr.params[PF_PARAM_NONE])
	{
		for (const ParamItem& PI : PT.items)
		{
			OS << "	" << PT.type << " " << PI.name;
			if (!PI.value.empty())
				OS << " = " << PI.value;
			OS << ";" << std::endl;
		}
	}
}

void VKFragmentDecompilerThread::insertMainEnd(std::stringstream & OS)
{
	const std::pair<std::string, std::string> table[] =
	{
		{ "ocol0", m_ctrl & CELL_GCM_SHADER_CONTROL_32_BITS_EXPORTS ? "r0" : "h0" },
		{ "ocol1", m_ctrl & CELL_GCM_SHADER_CONTROL_32_BITS_EXPORTS ? "r2" : "h4" },
		{ "ocol2", m_ctrl & CELL_GCM_SHADER_CONTROL_32_BITS_EXPORTS ? "r3" : "h6" },
		{ "ocol3", m_ctrl & CELL_GCM_SHADER_CONTROL_32_BITS_EXPORTS ? "r4" : "h8" },
	};

	for (int i = 0; i < sizeof(table) / sizeof(*table); ++i)
	{
		if (m_parr.HasParam(PF_PARAM_NONE, "vec4", table[i].second))
			OS << "	" << table[i].first << " = " << table[i].second << ";" << std::endl;
	}

	if (m_ctrl & CELL_GCM_SHADER_CONTROL_DEPTH_EXPORT)
	{
		{
			/** Note: Naruto Shippuden : Ultimate Ninja Storm 2 sets CELL_GCM_SHADER_CONTROL_32_BITS_EXPORTS in a shader
			* but it writes depth in r1.z and not h2.z.
			* Maybe there's a different flag for depth ?
			*/
			//OS << ((m_ctrl & CELL_GCM_SHADER_CONTROL_32_BITS_EXPORTS) ? "\tgl_FragDepth = r1.z;\n" : "\tgl_FragDepth = h0.z;\n") << std::endl;
			OS << "	gl_FragDepth = r1.z;\n";
		}
	}


	OS << "}" << std::endl;
}

void VKFragmentDecompilerThread::Task()
{
	m_shader = Decompile();
}

VKFragmentProgram::VKFragmentProgram()
{
}

VKFragmentProgram::~VKFragmentProgram()
{
	Delete();
}

void VKFragmentProgram::Decompile(const RSXFragmentProgram& prog)
{
	u32 size;
	VKFragmentDecompilerThread decompiler(shader, parr, prog, size);
	decompiler.Task();
	
	for (const ParamType& PT : decompiler.m_parr.params[PF_PARAM_UNIFORM])
	{
		for (const ParamItem& PI : PT.items)
		{
			if (PT.type == "sampler2D")
				continue;
			size_t offset = atoi(PI.name.c_str() + 2);
			FragmentConstantOffsetCache.push_back(offset);
		}
	}
}

void VKFragmentProgram::Compile()
{
	//Create the object and compile
	VkShaderModuleCreateInfo fs_info;
	fs_info.codeSize = shader.length();
	fs_info.pNext = nullptr;
	fs_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	fs_info.pCode = (uint32_t*)shader.data();
	fs_info.flags = 0;

	VkDevice dev = (VkDevice)*vk::get_current_renderer();
	vkCreateShaderModule(dev, &fs_info, nullptr, &handle);
}

void VKFragmentProgram::Delete()
{
	shader.clear();

	if (handle)
	{
		if (Emu.IsStopped())
		{
			LOG_WARNING(RSX, "VKFragmentProgram::Delete(): vkDestroyShaderModule(0x%X) avoided", handle);
		}
		else
		{
			VkDevice dev = (VkDevice)*vk::get_current_renderer();
			vkDestroyShaderModule(dev, handle, NULL);
			handle = nullptr;
		}
	}
}
