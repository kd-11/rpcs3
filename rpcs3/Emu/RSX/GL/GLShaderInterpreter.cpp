#include "stdafx.h"
#include "GLShaderInterpreter.h"
#include "GLTextureCache.h"
#include "GLVertexProgram.h"
#include "GLFragmentProgram.h"
#include "GLPipelineCompiler.h"

#include "Emu/RSX/rsx_methods.h"
#include "Emu/RSX/Overlays/Shaders/shader_loading_dialog.h"
#include "Emu/RSX/Program/ShaderInterpreter.h"
#include "Emu/RSX/Program/GLSLCommon.h"

namespace gl
{
	using glsl::shader;
	using enum program_common::interpreter::compiler_option;
	using enum program_common::interpreter::cached_pipeline_flags;

	namespace interpreter
	{
		void texture_pool_allocator::create(::glsl::program_domain domain)
		{
			GLenum pname;
			switch (domain)
			{
			default:
				rsx_log.fatal("Unexpected program domain %d", static_cast<int>(domain));
				[[fallthrough]];
			case ::glsl::program_domain::glsl_vertex_program:
				pname = GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS; break;
			case ::glsl::program_domain::glsl_fragment_program:
				pname = GL_MAX_TEXTURE_IMAGE_UNITS; break;
			}

			glGetIntegerv(pname, &max_image_units);
		}

		void texture_pool_allocator::allocate(int size)
		{
			if ((used + size) > max_image_units)
			{
				rsx_log.fatal("Out of image binding slots!");
			}

			used += size;
			texture_pool pool;
			pool.pool_size = size;
			pools.push_back(pool);
		}
	}

	void shader_interpreter::create(rsx::shader_loading_dialog* dlg)
	{
		dlg->create("Precompiling interpreter variants.\nPlease wait...", "Shader Compilation");

		const auto variants = program_common::interpreter::get_interpreter_variants();
		const u32 limit1 = ::size32(variants.base_pipelines);
		const u32 limit2 = ::size32(variants.pipelines);
		dlg->set_limit(0, limit1);
		dlg->set_limit(1, limit2);

		atomic_t<u32> ctr = 0;
		auto progress_hook = [&](const std::shared_ptr<interpreter::cached_program>&) { ctr++; };

		auto update_progress = [&](u32 stage)
		{
			const auto completed = ctr.load();
			const auto limit = stage ? limit2 : limit1;
			const auto message = fmt::format("%s variant %u of %u...", stage ? "Linking" : "Building", ctr.load(), limit);
			dlg->update_msg(stage, message);
			dlg->set_value(stage, completed);
		};

		// We only need to build the base "compatible pipeline" pairs.
		for (const auto& variant : variants.base_pipelines)
		{
			build_program_async(variant.first | variant.second, progress_hook);
		}

		do
		{
			std::this_thread::sleep_for(16ms);
			update_progress(0);
		}
		while (ctr < limit1);

		// Show final progress
		update_progress(0);

		// Second stage. Propagate base pipelines to all compatible
		ctr = 0;
		std::lock_guard lock(m_program_cache_lock);

		for (const auto& variant : variants.pipelines)
		{
			const u64 compiler_options = variant.vs_opts.shader_opt | variant.fs_opts.shader_opt;
			if (m_program_cache.find(compiler_options) != m_program_cache.end())
			{
				// Base variant
				continue;
			}

			const u64 compatible_options = variant.vs_opts.compatible_shader_opts | variant.fs_opts.compatible_shader_opts;
			auto base_pipeline = m_program_cache.find(compatible_options);
			if (base_pipeline == m_program_cache.end())
			{
				fmt::throw_exception("Base variant was not found in the cache.");
			}

			auto data = new interpreter::cached_program();
			data->flags |= CACHED_PIPE_UNOPTIMIZED;
			data->allocator = base_pipeline->second->allocator;
			data->vertex_shader = base_pipeline->second->vertex_shader;
			data->fragment_shader = base_pipeline->second->fragment_shader;
			data->prog = base_pipeline->second->prog;
			m_program_cache[compiler_options].reset(data);
		}

		ctr = limit2;
		update_progress(1);

		// Minor stall to avoid visual flashing
		std::this_thread::sleep_for(16ms);
	}

	void shader_interpreter::destroy()
	{
		for (auto& prog : m_program_cache)
		{
			prog.second->vertex_shader->remove();
			prog.second->fragment_shader->remove();
			prog.second->prog->remove();
		}

		for (auto& shader : m_vs_cache)
		{
			shader.second->remove();
		}

		for (auto& shader : m_fs_cache)
		{
			shader.second->remove();
		}
	}

	glsl::program* shader_interpreter::get(const interpreter::program_metadata& metadata, u32 vp_ctrl, u32 fp_ctrl)
	{
		// Build options
		u64 opt = 0;
		if (rsx::method_registers.alpha_test_enabled()) [[unlikely]]
		{
			switch (rsx::method_registers.alpha_func())
			{
			case rsx::comparison_function::always:
				break;
			case rsx::comparison_function::never:
				return nullptr;
			case rsx::comparison_function::greater_or_equal:
				opt |= COMPILER_OPT_ENABLE_ALPHA_TEST_GE;
				break;
			case rsx::comparison_function::greater:
				opt |= COMPILER_OPT_ENABLE_ALPHA_TEST_G;
				break;
			case rsx::comparison_function::less_or_equal:
				opt |= COMPILER_OPT_ENABLE_ALPHA_TEST_LE;
				break;
			case rsx::comparison_function::less:
				opt |= COMPILER_OPT_ENABLE_ALPHA_TEST_L;
				break;
			case rsx::comparison_function::equal:
				opt |= COMPILER_OPT_ENABLE_ALPHA_TEST_EQ;
				break;
			case rsx::comparison_function::not_equal:
				opt |= COMPILER_OPT_ENABLE_ALPHA_TEST_NE;
				break;
			}
		}

		if (fp_ctrl & CELL_GCM_SHADER_CONTROL_DEPTH_EXPORT) opt |= COMPILER_OPT_ENABLE_DEPTH_EXPORT;
		if (fp_ctrl & CELL_GCM_SHADER_CONTROL_32_BITS_EXPORTS) opt |= COMPILER_OPT_ENABLE_F32_EXPORT;
		if (fp_ctrl & RSX_SHADER_CONTROL_USES_KIL) opt |= COMPILER_OPT_ENABLE_KIL;
		if (metadata.referenced_textures_mask) opt |= COMPILER_OPT_ENABLE_TEXTURES;
		if (metadata.has_branch_instructions) opt |= COMPILER_OPT_ENABLE_FLOW_CTRL;
		if (metadata.has_pack_instructions) opt |= COMPILER_OPT_ENABLE_PACKING;
		if (rsx::method_registers.polygon_stipple_enabled()) opt |= COMPILER_OPT_ENABLE_STIPPLING;
		if (vp_ctrl & RSX_SHADER_CONTROL_INSTANCED_CONSTANTS) opt |= COMPILER_OPT_ENABLE_INSTANCING;

		{
			reader_lock lock(m_program_cache_lock);
			if (auto it = m_program_cache.find(opt); it != m_program_cache.end()) [[likely]]
			{
				m_current_interpreter = it->second;
			}

			if (m_current_interpreter)
			{
				constexpr u32 test_mask = (CACHED_PIPE_UNOPTIMIZED | CACHED_PIPE_RECOMPILING);
				constexpr u32 unoptimized_mask = CACHED_PIPE_UNOPTIMIZED;
				if ((m_current_interpreter->flags & test_mask) == unoptimized_mask)
				{
					// Interpreter is unoptimized and we haven't tried to recompile it
					// NOTE: This operation effectively orphans the current interpreter, but since unoptimized pipelines just have a non-owning reference then it's actually fine and we don't really leak anything.
					m_current_interpreter->flags |= CACHED_PIPE_RECOMPILING;
					build_program_async(opt, {});
				}
				return m_current_interpreter->prog.get();
			}
		}

		m_current_interpreter = build_program(opt);
		return m_current_interpreter->prog.get();
	}

	void shader_interpreter::build_vs(u64 compiler_options, interpreter::cached_program& prog_data)
	{
		compiler_options &= COMPILER_OPT_ALL_VS_MASK;
		{
			reader_lock lock(m_vs_cache_lock);

			if (auto found = m_vs_cache.find(compiler_options);
				found != m_vs_cache.end())
			{
				prog_data.vertex_shader = found->second;
				return;
			}
		}

		::glsl::shader_properties properties{};
		properties.domain = ::glsl::program_domain::glsl_vertex_program;
		properties.require_lit_emulation = true;
		properties.require_clip_plane_functions = true;

		// TODO: Extend decompiler thread
		// TODO: Rename decompiler thread, it no longer spawns a thread
		RSXVertexProgram null_prog;
		std::string shader_str;
		ParamArray arr;

		null_prog.ctrl = (compiler_options & COMPILER_OPT_ENABLE_INSTANCING)
			? RSX_SHADER_CONTROL_INSTANCED_CONSTANTS
			: 0;
		GLVertexDecompilerThread comp(null_prog, shader_str, arr);

		// Initialize compiler properties
		comp.properties.has_indexed_constants = true;

		ParamType uniforms = { PF_PARAM_UNIFORM, "vec4" };
		uniforms.items.emplace_back("vc[468]", -1);

		std::stringstream builder;
		comp.insertHeader(builder);

		builder << "#define Z_NEGATIVE_ONE_TO_ONE\n\n";

		comp.insertConstants(builder, { uniforms });
		comp.insertInputs(builder, {});

		// Insert vp stream input
		builder << "\n"
			"layout(std140, binding = " << GL_INTERPRETER_VERTEX_BLOCK << ") readonly restrict buffer VertexInstructionBlock\n"
			"{\n"
			"	uint base_address;\n"
			"	uint entry;\n"
			"	uint output_mask;\n"
			"	uint control;\n"
			"	uvec4 vp_instructions[];\n"
			"};\n\n";

		if (compiler_options & COMPILER_OPT_ENABLE_INSTANCING)
		{
			builder << "#define _ENABLE_INSTANCED_CONSTANTS\n";
		}

		if (compiler_options)
		{
			builder << "\n";
		}

		::glsl::insert_glsl_legacy_function(builder, properties);
		::glsl::insert_vertex_input_fetch(builder, ::glsl::glsl_rules::glsl_rules_opengl4);

		builder << program_common::interpreter::get_vertex_interpreter();
		const std::string s = builder.str();

		prog_data.vertex_shader = std::make_shared<glsl::shader>();
		prog_data.vertex_shader->create(::glsl::program_domain::glsl_vertex_program, s);
		prog_data.vertex_shader->compile();

		{
			std::lock_guard lock(m_vs_cache_lock);

			const auto [found, inserted] = m_vs_cache.try_emplace(compiler_options, prog_data.vertex_shader);
			if (!inserted)
			{
				// Cache hit
				prog_data.vertex_shader->remove();
				prog_data.vertex_shader = found->second;
			}
		}
	}

	void shader_interpreter::build_fs(u64 compiler_options, interpreter::cached_program& prog_data)
	{
		// Allocate TIUs
		auto& allocator = prog_data.allocator;
		if (compiler_options & COMPILER_OPT_ENABLE_TEXTURES)
		{
			allocator.create(::glsl::program_domain::glsl_fragment_program);
			if (allocator.max_image_units >= 32)
			{
				// 16 + 4 + 4 + 4
				allocator.allocate(4);   // 1D
				allocator.allocate(16);  // 2D
				allocator.allocate(4);   // CUBE
				allocator.allocate(4);   // 3D
			}
			else if (allocator.max_image_units >= 24)
			{
				// 16 + 4 + 2 + 2
				allocator.allocate(2);   // 1D
				allocator.allocate(16);  // 2D
				allocator.allocate(2);   // CUBE
				allocator.allocate(4);   // 3D
			}
			else if (allocator.max_image_units >= 16)
			{
				// 10 + 2 + 2 + 2
				allocator.allocate(2);   // 1D
				allocator.allocate(10);  // 2D
				allocator.allocate(2);   // CUBE
				allocator.allocate(2);   // 3D
			}
			else
			{
				// Unusable
				rsx_log.fatal("Failed to allocate enough TIUs for shader interpreter.");
			}
		}

		// Cache lookup
		compiler_options &= COMPILER_OPT_ALL_FS_MASK;
		{
			reader_lock lock(m_fs_cache_lock);

			if (auto found = m_fs_cache.find(compiler_options);
				found != m_fs_cache.end())
			{
				prog_data.fragment_shader = found->second;
				return;
			}
		}

		u32 len;
		ParamArray arr;
		std::string shader_str;
		RSXFragmentProgram frag;
		GLFragmentDecompilerThread comp(shader_str, arr, frag, len);

		std::stringstream builder;
		builder <<
		"#version 450\n"
		"//#extension GL_ARB_bindless_texture : require\n\n";

		::glsl::insert_subheader_block(builder);
		comp.insertConstants(builder);

		if (compiler_options & COMPILER_OPT_ENABLE_ALPHA_TEST_GE)
		{
			builder << "#define ALPHA_TEST_GEQUAL\n";
		}

		if (compiler_options & COMPILER_OPT_ENABLE_ALPHA_TEST_G)
		{
			builder << "#define ALPHA_TEST_GREATER\n";
		}

		if (compiler_options & COMPILER_OPT_ENABLE_ALPHA_TEST_LE)
		{
			builder << "#define ALPHA_TEST_LEQUAL\n";
		}

		if (compiler_options & COMPILER_OPT_ENABLE_ALPHA_TEST_L)
		{
			builder << "#define ALPHA_TEST_LESS\n";
		}

		if (compiler_options & COMPILER_OPT_ENABLE_ALPHA_TEST_EQ)
		{
			builder << "#define ALPHA_TEST_EQUAL\n";
		}

		if (compiler_options & COMPILER_OPT_ENABLE_ALPHA_TEST_NE)
		{
			builder << "#define ALPHA_TEST_NEQUAL\n";
		}

		if (!(compiler_options & COMPILER_OPT_ENABLE_F32_EXPORT))
		{
			builder << "#define WITH_HALF_OUTPUT_REGISTER\n";
		}

		if (compiler_options & COMPILER_OPT_ENABLE_DEPTH_EXPORT)
		{
			builder << "#define WITH_DEPTH_EXPORT\n";
		}

		if (compiler_options & COMPILER_OPT_ENABLE_FLOW_CTRL)
		{
			builder << "#define WITH_FLOW_CTRL\n";
		}

		if (compiler_options & COMPILER_OPT_ENABLE_PACKING)
		{
			builder << "#define WITH_PACKING\n";
		}

		if (compiler_options & COMPILER_OPT_ENABLE_KIL)
		{
			builder << "#define WITH_KIL\n";
		}

		if (compiler_options & COMPILER_OPT_ENABLE_STIPPLING)
		{
			builder << "#define WITH_STIPPLING\n";
		}

		if (compiler_options & COMPILER_OPT_ENABLE_TEXTURES)
		{
			builder << "#define WITH_TEXTURES\n\n";

			const char* type_names[] = { "sampler1D", "sampler2D", "samplerCube", "sampler3D" };
			for (int i = 0; i < 4; ++i)
			{
				builder << "uniform " << type_names[i] << " " << type_names[i] << "_array[" << allocator.pools[i].pool_size << "];\n";
			}

			builder << "\n"
				"#undef  TEX_PARAM\n"
				"#define TEX_PARAM(index) texture_parameters[index + texture_base_index]\n"
				"#define IS_TEXTURE_RESIDENT(index) (texture_handles[index] < 0xFF)\n"
				"#define SAMPLER1D(index) sampler1D_array[texture_handles[index]]\n"
				"#define SAMPLER2D(index) sampler2D_array[texture_handles[index]]\n"
				"#define SAMPLER3D(index) sampler3D_array[texture_handles[index]]\n"
				"#define SAMPLERCUBE(index) samplerCube_array[texture_handles[index]]\n\n";
		}
		else if (compiler_options)
		{
			builder << "\n";
		}

		builder <<
			"layout(std430, binding =" << GL_INTERPRETER_FRAGMENT_BLOCK << ") readonly restrict buffer FragmentInstructionBlock\n"
			"{\n"
			"	uint shader_control;\n"
			"	uint texture_control;\n"
			"	uint reserved1;\n"
			"	uint reserved2;\n"
			"	uint texture_handles[16];\n"
			"	uvec4 fp_instructions[];\n"
			"};\n\n";

		builder << program_common::interpreter::get_fragment_interpreter();
		const std::string s = builder.str();

		prog_data.fragment_shader = std::make_shared<glsl::shader>();
		prog_data.fragment_shader->create(::glsl::program_domain::glsl_fragment_program, s);
		prog_data.fragment_shader->compile();

		{
			std::lock_guard lock(m_fs_cache_lock);

			const auto [found, inserted] = m_fs_cache.try_emplace(compiler_options, prog_data.fragment_shader);
			if (!inserted)
			{
				// Cache hit
				prog_data.fragment_shader->remove();
				prog_data.fragment_shader = found->second;
			}
		}
	}

	std::shared_ptr<interpreter::cached_program> shader_interpreter::build_program(u64 compiler_options)
	{
		auto data = std::make_shared<interpreter::cached_program>();
		build_fs(compiler_options, *data);
		build_vs(compiler_options, *data);

		data->prog = std::make_shared<glsl::program>();
		data->prog->create().
			attach(*data->vertex_shader).
			attach(*data->fragment_shader).
			link();

		post_init_hook(data, compiler_options);
		return data;
	}

	void shader_interpreter::build_program_async(u64 compiler_options, async_build_callback_t callback)
	{
		auto data = std::make_shared<interpreter::cached_program>();

		auto post_create_hook = [=](glsl::program* prog)
		{
			build_fs(compiler_options, *data);
			build_vs(compiler_options, *data);

			prog->attach(*data->vertex_shader).
				attach(*data->fragment_shader);
		};

		auto storage_hook = [=](std::unique_ptr<glsl::program>& prog)
		{
			data->prog = std::move(prog);
			post_init_hook(data, compiler_options);

			if (callback)
			{
				callback(data);
			}
		};

		auto compiler = gl::get_pipe_compiler();
		compiler->compile(
			gl::pipe_compiler::COMPILE_DEFERRED,
			post_create_hook,
			{},
			storage_hook
		);
	}

	void shader_interpreter::post_init_hook(const std::shared_ptr<interpreter::cached_program>& data, u64 compiler_options)
	{
		data->prog->uniforms[0] = GL_STREAM_BUFFER_START + 0;
		data->prog->uniforms[1] = GL_STREAM_BUFFER_START + 1;

		if (compiler_options & COMPILER_OPT_ENABLE_TEXTURES)
		{
			// Initialize texture bindings
			int assigned = 0;
			auto& allocator = data->allocator;
			const char* type_names[] = { "sampler1D_array", "sampler2D_array", "samplerCube_array", "sampler3D_array" };

			for (int i = 0; i < 4; ++i)
			{
				for (int j = 0; j < allocator.pools[i].pool_size; ++j)
				{
					allocator.pools[i].allocate(assigned++);
				}

				data->prog->uniforms[type_names[i]] = allocator.pools[i].allocated;
			}
		}

		std::lock_guard lock(m_program_cache_lock);
		m_program_cache[compiler_options] = data;
	}

	bool shader_interpreter::is_interpreter(const glsl::program* program) const
	{
		return (m_current_interpreter && program == m_current_interpreter->prog.get());
	}

	void shader_interpreter::update_fragment_textures(
		const std::array<std::unique_ptr<rsx::sampled_image_descriptor_base>, 16>& descriptors,
		u16 reference_mask, u32* out)
	{
		if (reference_mask == 0 || !m_current_interpreter)
		{
			return;
		}

		// Reset allocation
		auto& allocator = m_current_interpreter->allocator;
		for (unsigned i = 0; i < 4; ++i)
		{
			allocator.pools[i].num_used = 0;
			allocator.pools[i].flags = 0;
		}

		rsx::simple_array<utils::pair<int, int>> replacement_map;
		for (int i = 0; i < rsx::limits::fragment_textures_count; ++i)
		{
			if (reference_mask & (1 << i))
			{
				auto sampler_state = static_cast<gl::texture_cache::sampled_image_descriptor*>(descriptors[i].get());
				ensure(sampler_state);

				int pool_id = static_cast<int>(sampler_state->image_type);
				auto& pool = allocator.pools[pool_id];

				const int old = pool.allocated[pool.num_used];
				if (!pool.allocate(i))
				{
					rsx_log.error("Could not allocate texture resource for shader interpreter.");
					break;
				}

				out[i] = (pool.num_used - 1);
				if (old != i)
				{
					// Check if the candidate target has also been replaced
					bool found = false;
					for (auto& e : replacement_map)
					{
						if (e.second == old)
						{
							// This replacement consumed this 'old' value
							e.second = i;
							found = true;
							break;
						}
					}

					if (!found)
					{
						replacement_map.push_back({ old, i });
					}
				}
			}
			else
			{
				out[i] = 0xFF;
			}
		}

		// Bind TIU locations
		if (replacement_map.empty()) [[likely]]
		{
			return;
		}

		// Overlapping texture bindings are trouble. Cannot bind one TIU to two types of samplers simultaneously
		for (unsigned i = 0; i < replacement_map.size(); ++i)
		{
			for (int j = 0; j < 4; ++j)
			{
				auto& pool = allocator.pools[j];
				for (int k = pool.num_used; k < pool.pool_size; ++k)
				{
					if (pool.allocated[k] == replacement_map[i].second)
					{
						pool.allocated[k] = replacement_map[i].first;
						pool.flags |= static_cast<u32>(interpreter::texture_pool_flags::dirty);

						// Exit nested loop
						j = 4;
						break;
					}
				}
			}
		}

		if (allocator.pools[0].flags) m_current_interpreter->prog->uniforms["sampler1D_array"] = allocator.pools[0].allocated;
		if (allocator.pools[1].flags) m_current_interpreter->prog->uniforms["sampler2D_array"] = allocator.pools[1].allocated;
		if (allocator.pools[2].flags) m_current_interpreter->prog->uniforms["samplerCube_array"] = allocator.pools[2].allocated;
		if (allocator.pools[3].flags) m_current_interpreter->prog->uniforms["sampler3D_array"] = allocator.pools[3].allocated;
	}
}
