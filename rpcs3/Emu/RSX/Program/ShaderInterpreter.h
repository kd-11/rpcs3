#pragma once

#include <util/types.hpp>

namespace program_common
{
	namespace interpreter
	{
		enum compiler_option : u32
		{
			// FS Mix-N-Match
			COMPILER_OPT_ENABLE_TEXTURES       = (1 << 0),
			COMPILER_OPT_ENABLE_DEPTH_EXPORT   = (1 << 1),
			COMPILER_OPT_ENABLE_F32_EXPORT     = (1 << 2),
			COMPILER_OPT_ENABLE_PACKING        = (1 << 3),
			COMPILER_OPT_ENABLE_KIL            = (1 << 4),
			COMPILER_OPT_ENABLE_STIPPLING      = (1 << 5),
			COMPILER_OPT_ENABLE_FLOW_CTRL      = (1 << 6),

			// VS Mix-N-Match
			COMPILER_OPT_ENABLE_INSTANCING     = (1 << 7),
			COMPILER_OPT_ENABLE_VTX_TEXTURES   = (1 << 8),

			// Exclusive bits. Only one can be set at a time
			COMPILER_OPT_ENABLE_ALPHA_TEST_GE  = (1 << 9),
			COMPILER_OPT_ENABLE_ALPHA_TEST_G   = (1 << 10),
			COMPILER_OPT_ENABLE_ALPHA_TEST_LE  = (1 << 11),
			COMPILER_OPT_ENABLE_ALPHA_TEST_L   = (1 << 12),
			COMPILER_OPT_ENABLE_ALPHA_TEST_EQ  = (1 << 13),
			COMPILER_OPT_ENABLE_ALPHA_TEST_NE  = (1 << 14),

			// Meta
			COMPILER_OPT_MAX                   = COMPILER_OPT_ENABLE_ALPHA_TEST_NE,
			COMPILER_OPT_ALPHA_TEST_MASK       = (0b111111 << 9),

			// Bounds
			COMPILER_OPT_FS_MAX                = COMPILER_OPT_ENABLE_FLOW_CTRL,
			COMPILER_OPT_FS_MIN                = COMPILER_OPT_ENABLE_TEXTURES,
			COMPILER_OPT_VS_MAX                = COMPILER_OPT_ENABLE_VTX_TEXTURES,
			COMPILER_OPT_VS_MIN                = COMPILER_OPT_ENABLE_INSTANCING,
		};

		static std::string get_vertex_interpreter()
		{
			const char* s =
			#include "../Program/GLSLInterpreter/VertexInterpreter.glsl"
			;
			return s;
		}

		static std::string get_fragment_interpreter()
		{
			const char* s =
			#include "../Program/GLSLInterpreter/FragmentInterpreter.glsl"
			;
			return s;
		}

		using interpreter_variant_t = std::pair<u32, u32>;
		std::vector<interpreter_variant_t> get_interpreter_variants();
	}
}
