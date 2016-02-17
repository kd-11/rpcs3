#pragma once
#include "VKVertexProgram.h"
#include "VKFragmentProgram.h"
#include "../Common/ProgramStateCache.h"

struct VKTraits
{
	using vertex_program_type = VKVertexProgram;
	using fragment_program_type = VKFragmentProgram;
	using pipeline_storage_type = vk::glsl::program;
	using pipeline_properties = void*;

	static
	void recompile_fragment_program(const RSXFragmentProgram &RSXFP, fragment_program_type& fragmentProgramData, size_t ID)
	{
		fragmentProgramData.Decompile(RSXFP);
		fragmentProgramData.Compile();
		//checkForGlError("m_fragment_prog.Compile");

		fs::file(fs::get_config_dir() + "FragmentProgram.txt", fom::rewrite).write(fragmentProgramData.shader);
	}

	static
	void recompile_vertex_program(const RSXVertexProgram &RSXVP, vertex_program_type& vertexProgramData, size_t ID)
	{
		vertexProgramData.Decompile(RSXVP);
		vertexProgramData.Compile();
		//checkForGlError("m_vertex_prog.Compile");

		fs::file(fs::get_config_dir() + "VertexProgram.txt", fom::rewrite).write(vertexProgramData.shader);
	}

	static
	pipeline_storage_type build_pipeline(const vertex_program_type &vertexProgramData, const fragment_program_type &fragmentProgramData, const pipeline_properties &pipelineProperties)
	{
		pipeline_storage_type result(*vk::get_current_renderer());
		__vkcheck result.attachVertexProgram(vertexProgramData.handle)
			.attachFragmentProgram(fragmentProgramData.handle)
			.bind_fragment_data_location("ocol0", 0)
			.bind_fragment_data_location("ocol1", 1)
			.bind_fragment_data_location("ocol2", 2)
			.bind_fragment_data_location("ocol3", 3)
			.make();

		return result;
	}
};

class VKProgramBuffer : public program_state_cache<VKTraits>
{
};
