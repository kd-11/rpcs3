#include "stdafx.h"
#include "Utilities/rPlatform.h" // only for rImage
#include "Emu/Memory/Memory.h"
#include "Emu/System.h"
#include "Emu/state.h"
#include "VKGSRender.h"
#include "../rsx_methods.h"
#include "../Common/BufferUtils.h"

namespace
{
	u32 get_max_depth_value(rsx::surface_depth_format format)
	{
		switch (format)
		{
		case rsx::surface_depth_format::z16: return 0xFFFF;
		case rsx::surface_depth_format::z24s8: return 0xFFFFFF;
		}
		throw EXCEPTION("Unknow depth format");
	}

	u8 get_pixel_size(rsx::surface_depth_format format)
	{
		switch (format)
		{
		case rsx::surface_depth_format::z16: return 2;
		case rsx::surface_depth_format::z24s8: return 4;
		}
		throw EXCEPTION("Unknow depth format");
	}
}

namespace vk
{
	VkCompareOp compare_op(u32 gl_name)
	{
		switch (gl_name)
		{
		case CELL_GCM_GREATER:
			return VK_COMPARE_OP_GREATER;
		case CELL_GCM_LESS:
			return VK_COMPARE_OP_LESS;
		case CELL_GCM_LEQUAL:
			return VK_COMPARE_OP_LESS_OR_EQUAL;
		case CELL_GCM_GEQUAL:
			return VK_COMPARE_OP_EQUAL;
		case CELL_GCM_EQUAL:
			return VK_COMPARE_OP_EQUAL;
		case CELL_GCM_ALWAYS:
			return VK_COMPARE_OP_ALWAYS;
		default:
			throw EXCEPTION("Unsupported compare op: 0x%X", gl_name);
		}
	}

	VkFormat get_suitable_vk_format(rsx::vertex_base_type type, u8 size)
	{
		/**
		* Set up buffer fetches to only work on 4-component access
		*/
		const VkFormat vec1_types[] = { VK_FORMAT_R16_UNORM, VK_FORMAT_R32_SFLOAT, VK_FORMAT_R16_SFLOAT, VK_FORMAT_R8_UNORM, VK_FORMAT_R32_SINT, VK_FORMAT_R16_SFLOAT, VK_FORMAT_R8_UNORM };
		const VkFormat vec2_types[] = { VK_FORMAT_R16G16_UNORM, VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_R8G8_UNORM, VK_FORMAT_R32G32_SINT, VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_R8G8_UNORM };
		const VkFormat vec3_types[] = { VK_FORMAT_R16G16B16A16_UNORM, VK_FORMAT_R32G32B32A32_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R32G32B32A32_SINT, VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R8G8B8A8_UNORM };	//VEC3 COMPONENTS NOT SUPPORTED!
		const VkFormat vec4_types[] = { VK_FORMAT_R16G16B16A16_UNORM, VK_FORMAT_R32G32B32A32_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R32G32B32A32_SINT, VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R8G8B8A8_UNORM };

		const VkFormat* vec_selectors[] = { 0, vec1_types, vec2_types, vec3_types, vec4_types };

		if (type > rsx::vertex_base_type::ub256)
			throw EXCEPTION("VKGS error: unknown vertex base type 0x%X.", (u32)type);

		return vec_selectors[size][(int)type];
	}

	u32 get_suitable_vk_size(rsx::vertex_base_type type, u32 size)
	{
		if (size == 3)
		{
			switch (type)
			{
			case rsx::vertex_base_type::f:
			case rsx::vertex_base_type::s32k:
				return 16;
			}
		}
		
		return rsx::get_vertex_type_size_on_host(type, size);
	}

	bool requires_component_expansion(rsx::vertex_base_type type, u32 size)
	{
		if (size == 3)
		{
			switch (type)
			{
			case rsx::vertex_base_type::f:
			case rsx::vertex_base_type::s32k:
				return true;
			}
		}

		return false;
	}

	VkPrimitiveTopology get_appropriate_topology(rsx::primitive_type& mode, bool &requires_modification)
	{
		requires_modification = false;

		switch (mode)
		{
		case rsx::primitive_type::lines:
			return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
		case rsx::primitive_type::line_loop:
			requires_modification = true;
			return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
		case rsx::primitive_type::line_strip:
			return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
		case rsx::primitive_type::points:
			return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
		case rsx::primitive_type::triangles:
			return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
		case rsx::primitive_type::triangle_strip:
			return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
		case rsx::primitive_type::triangle_fan:
			return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
		case rsx::primitive_type::quads:
		case rsx::primitive_type::polygon:
			requires_modification = true;
			return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
		case rsx::primitive_type::quad_strip:
			return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
		default:
			throw ("Unsupported primitive topology 0x%X", (u8)mode);
		}
	}

	/**
	 * Expand line loop array to line strip array; simply loop back the last vertex to the first..
	 */
	u32 expand_line_loop_array_to_strip(u32 vertex_draw_count, std::vector<u16>& indices)
	{
		int i = 0;
		indices.resize(vertex_draw_count + 1);
		
		for (; i < vertex_draw_count; ++i)
			indices[i] = i;

		indices[i] = 0;
		return indices.size();
	}

	template<typename T>
	u32 expand_indexed_line_loop_to_strip(u32 original_count, const T* original_indices, std::vector<T>& indices)
	{
		indices.resize(original_count + 1);

		int i = 0;
		for (; i < original_count; ++i)
			indices[i] = original_indices[i];

		indices[i] = original_indices[0];
		return indices.size();
	}

	VkFormat get_compatible_surface_format(rsx::surface_color_format color_format)
	{
		switch (color_format)
		{
		case rsx::surface_color_format::r5g6b5:
			return VK_FORMAT_R5G6B5_UNORM_PACK16;

		case rsx::surface_color_format::a8r8g8b8:
			return VK_FORMAT_B8G8R8A8_UNORM;

		case rsx::surface_color_format::x8r8g8b8_o8r8g8b8:
			LOG_ERROR(RSX, "Format 0x%X may be buggy.", color_format);
			return VK_FORMAT_B8G8R8A8_UNORM;

		case rsx::surface_color_format::w16z16y16x16:
			return VK_FORMAT_R16G16B16A16_SFLOAT;

		case rsx::surface_color_format::w32z32y32x32:
			return VK_FORMAT_R32G32B32A32_SFLOAT;

		case rsx::surface_color_format::b8:
		case rsx::surface_color_format::x1r5g5b5_o1r5g5b5:
		case rsx::surface_color_format::x1r5g5b5_z1r5g5b5:
		case rsx::surface_color_format::x8r8g8b8_z8r8g8b8:
		case rsx::surface_color_format::g8b8:
		case rsx::surface_color_format::x32:
		case rsx::surface_color_format::x8b8g8r8_o8b8g8r8:
		case rsx::surface_color_format::x8b8g8r8_z8b8g8r8:
		case rsx::surface_color_format::a8b8g8r8:
		default:
			LOG_ERROR(RSX, "Surface color buffer: Unsupported surface color format (0x%x)", color_format);
			return VK_FORMAT_B8G8R8A8_UNORM;
		}
	}

	VkBlendFactor get_blend_factor(u16 factor)
	{
		switch (factor)
		{
		case CELL_GCM_ONE: return VK_BLEND_FACTOR_ONE;
		case CELL_GCM_ZERO: return VK_BLEND_FACTOR_ZERO;
		case CELL_GCM_SRC_ALPHA: return VK_BLEND_FACTOR_SRC_ALPHA;
		case CELL_GCM_DST_ALPHA: return VK_BLEND_FACTOR_DST_ALPHA;
		case CELL_GCM_SRC_COLOR: return VK_BLEND_FACTOR_SRC_COLOR;
		case CELL_GCM_DST_COLOR: return VK_BLEND_FACTOR_DST_COLOR;
		case CELL_GCM_CONSTANT_COLOR: return VK_BLEND_FACTOR_CONSTANT_COLOR;
		case CELL_GCM_CONSTANT_ALPHA: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
		case CELL_GCM_ONE_MINUS_SRC_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
		case CELL_GCM_ONE_MINUS_DST_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
		case CELL_GCM_ONE_MINUS_SRC_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
		case CELL_GCM_ONE_MINUS_DST_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
		case CELL_GCM_ONE_MINUS_CONSTANT_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
		case CELL_GCM_ONE_MINUS_CONSTANT_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
		default:
			throw EXCEPTION("Unknown blend factor 0x%X", factor);
		}
	};

	VkBlendOp get_blend_op(u16 op)
	{
		switch (op)
		{
		case CELL_GCM_FUNC_ADD: return VK_BLEND_OP_ADD;
		case CELL_GCM_FUNC_SUBTRACT: return VK_BLEND_OP_SUBTRACT;
		case CELL_GCM_FUNC_REVERSE_SUBTRACT: return VK_BLEND_OP_REVERSE_SUBTRACT;
		default:
			throw EXCEPTION("Unknown blend op: 0x%X", op);
		}
	}
}

VKGSRender::VKGSRender() : GSRender(frame_type::Vulkan)
{
	shaders_cache.load(rsx::shader_language::glsl);

	HINSTANCE hInstance = NULL;
	HWND hWnd = (HWND)m_frame->handle();

	m_thread_context.createInstance("RPCS3");
	m_thread_context.makeCurrentInstance(1);
	m_thread_context.enable_debugging();

	std::vector<vk::physical_device>& gpus = m_thread_context.enumerateDevices();
	m_swap_chain = m_thread_context.createSwapChain(hInstance, hWnd, gpus[0]);

	m_device = (vk::render_device *)(&m_swap_chain->get_device());
	
	vk::set_current_thread_ctx(m_thread_context);
	vk::set_current_renderer(m_swap_chain->get_device());

	m_swap_chain->init_swapchain(m_frame->client_size().width, m_frame->client_size().height);

	//create command buffer...
	m_command_buffer_pool.create((*m_device));
	m_command_buffer.create(m_command_buffer_pool);

	VkCommandBufferInheritanceInfo inheritance_info;
	inheritance_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO;
	inheritance_info.pNext = nullptr;
	inheritance_info.renderPass = VK_NULL_HANDLE;
	inheritance_info.subpass = 0;
	inheritance_info.framebuffer = VK_NULL_HANDLE;
	inheritance_info.occlusionQueryEnable = VK_FALSE;
	inheritance_info.queryFlags = 0;
	inheritance_info.pipelineStatistics = 0;

	VkCommandBufferBeginInfo begin_infos;
	begin_infos.flags = 0;
	begin_infos.pInheritanceInfo = &inheritance_info;
	begin_infos.pNext = nullptr;
	begin_infos.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

	CHECK_RESULT(vkBeginCommandBuffer(m_command_buffer, &begin_infos));

	for (u32 i = 0; i < m_swap_chain->get_swap_image_count(); ++i)
	{
		vk::change_image_layout(m_command_buffer, m_swap_chain->get_swap_chain_image(i),
								VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
								VK_IMAGE_ASPECT_COLOR_BIT);
	}
	
	CHECK_RESULT(vkEndCommandBuffer(m_command_buffer));
	execute_command_buffer(false);

	m_scale_offset_buffer.create((*m_device), 64);
	m_vertex_constants_buffer.create((*m_device), 512 * 16);
	m_fragment_constants_buffer.create((*m_device), 512 * 16);
	m_index_buffer.create((*m_device), 65536, VK_FORMAT_R16_UINT, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
}

VKGSRender::~VKGSRender()
{
	if (m_submit_fence)
	{
		vkWaitForFences((*m_device), 1, &m_submit_fence, VK_TRUE, 1000000L);
		vkDestroyFence((*m_device), m_submit_fence, nullptr);
		m_submit_fence = nullptr;
	}

	if (m_present_semaphore)
	{
		vkDestroySemaphore((*m_device), m_present_semaphore, nullptr);
		m_present_semaphore = nullptr;
	}

	vk::destroy_global_resources();

	//TODO: Properly destroy shader modules instead of calling clear...
	m_prog_buffer.clear();

	m_scale_offset_buffer.destroy();
	m_vertex_constants_buffer.destroy();
	m_fragment_constants_buffer.destroy();
	m_index_buffer.destroy();

	for (int i = 0; i < 4; ++i)
	{
		m_fbo_surfaces[i].destroy();
		m_framebuffers[i].destroy();
	}

	if (m_render_pass)
		destroy_render_pass();

	m_command_buffer.destroy();
	m_command_buffer_pool.destroy();

	m_depth_buffer.destroy();
	m_swap_chain->destroy();

	m_thread_context.close();
	delete m_swap_chain;
}

bool VKGSRender::on_access_violation(u32 address, bool is_writing)
{
	if (is_writing)
		return m_texture_cache.invalidate_address(address);

	return false;
}

void VKGSRender::begin()
{
	rsx::thread::begin();

	//TODO: Fence sync, ring-buffers, etc
	//CHECK_RESULT(vkDeviceWaitIdle((*m_device)));

	if (!load_program())
		return;

	if (!recording)
		begin_command_buffer_recording();

	init_buffers();

	m_program->set_draw_buffer_count(m_nb_targets);

	u32 color_mask = rsx::method_registers[NV4097_SET_COLOR_MASK];
	bool color_mask_b = !!(color_mask & 0xff);
	bool color_mask_g = !!((color_mask >> 8) & 0xff);
	bool color_mask_r = !!((color_mask >> 16) & 0xff);
	bool color_mask_a = !!((color_mask >> 24) & 0xff);

	VkColorComponentFlags mask = 0;
	if (color_mask_a) mask |= VK_COLOR_COMPONENT_A_BIT;
	if (color_mask_b) mask |= VK_COLOR_COMPONENT_B_BIT;
	if (color_mask_g) mask |= VK_COLOR_COMPONENT_G_BIT;
	if (color_mask_r) mask |= VK_COLOR_COMPONENT_R_BIT;

	VkColorComponentFlags color_masks[4] = { mask };

	u8 render_targets[] = { 0, 1, 2, 3 };
	m_program->set_color_mask(m_nb_targets, render_targets, color_masks);

	u32 depth_mask = rsx::method_registers[NV4097_SET_DEPTH_MASK];
	u32 stencil_mask = rsx::method_registers[NV4097_SET_STENCIL_MASK];

	if (rsx::method_registers[NV4097_SET_DEPTH_TEST_ENABLE])
	{
		m_program->set_depth_test_enable(VK_TRUE);
		m_program->set_depth_compare_op(vk::compare_op(rsx::method_registers[NV4097_SET_DEPTH_FUNC]));
		m_program->set_depth_write_mask(rsx::method_registers[NV4097_SET_DEPTH_MASK]);
	}
	else
		m_program->set_depth_test_enable(VK_FALSE);

	if (rsx::method_registers[NV4097_SET_BLEND_ENABLE])
	{
		u32 sfactor = rsx::method_registers[NV4097_SET_BLEND_FUNC_SFACTOR];
		u32 dfactor = rsx::method_registers[NV4097_SET_BLEND_FUNC_DFACTOR];

		VkBlendFactor sfactor_rgb[4] = { vk::get_blend_factor(sfactor) };
		VkBlendFactor sfactor_a[4] = { vk::get_blend_factor(sfactor >> 16) };
		VkBlendFactor dfactor_rgb[4] = {vk::get_blend_factor(dfactor)};
		VkBlendFactor dfactor_a[4] = { vk::get_blend_factor(dfactor >> 16) };

		//TODO: Separate target blending

		VkBool32 blend_state[4] = { VK_TRUE };

		m_program->set_blend_state(m_nb_targets, render_targets, blend_state);
		m_program->set_blend_func(m_nb_targets, render_targets, sfactor_rgb, dfactor_rgb, sfactor_a, dfactor_a);

		u32 equation = rsx::method_registers[NV4097_SET_BLEND_EQUATION];
		VkBlendOp equation_rgb[4] = { vk::get_blend_op(equation) };
		VkBlendOp equation_a[4] = { vk::get_blend_op(equation >> 16) };

		m_program->set_blend_op(m_nb_targets, render_targets, equation_rgb, equation_a);
	}
	else
	{
		VkBool32 blend_state[4] = { VK_FALSE };
		m_program->set_blend_state(m_nb_targets, render_targets, blend_state);
	}

	u32 line_width = rsx::method_registers[NV4097_SET_LINE_WIDTH];
	float actual_line_width = (line_width >> 3) + (line_width & 7) / 8.f;
	
	vkCmdSetLineWidth(m_command_buffer, actual_line_width);

	//TODO: Set up other render-state parameters into the program pipeline

	VkRenderPassBeginInfo rp_begin;
	rp_begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	rp_begin.pNext = NULL;
	rp_begin.renderPass = m_render_pass;
	rp_begin.framebuffer = m_framebuffers[m_current_fbo];
	rp_begin.renderArea.offset.x = 0;
	rp_begin.renderArea.offset.y = 0;
	rp_begin.renderArea.extent.width = m_frame->client_size().width;
	rp_begin.renderArea.extent.height = m_frame->client_size().height;
	rp_begin.clearValueCount = 0;
	rp_begin.pClearValues = nullptr;

	vkCmdBeginRenderPass(m_command_buffer, &rp_begin, VK_SUBPASS_CONTENTS_INLINE);
	m_draw_calls++;
}

namespace
{
	bool normalize(rsx::vertex_base_type type)
	{
		switch (type)
		{
		case rsx::vertex_base_type::s1:
		case rsx::vertex_base_type::ub:
		case rsx::vertex_base_type::cmp:
			return true;
		case rsx::vertex_base_type::f:
		case rsx::vertex_base_type::sf:
		case rsx::vertex_base_type::ub256:
		case rsx::vertex_base_type::s32k:
			return false;
		}
		throw EXCEPTION("unknown vertex type");
	}
}

void VKGSRender::end()
{	
	//TODO:
	//1. Bind output fbo
	//2. Enable output program
	//3. Texture setup

	//setup textures
	for (int i = 0; i < rsx::limits::textures_count; ++i)
	{
		if (m_program->has_uniform(vk::glsl::glsl_fragment_program, "tex" + std::to_string(i)))
		{
			if (!textures[i].enabled())
			{
				m_program->bind_uniform(vk::glsl::glsl_fragment_program, "tex" + std::to_string(i));
				continue;
			}

			vk::texture &tex = m_texture_cache.upload_texture(m_command_buffer, textures[i]);
			m_program->bind_uniform(vk::glsl::glsl_fragment_program, "tex" + std::to_string(i), tex);
		}
	}

	//initialize vertex attributes
	std::vector<u8> vertex_arrays_data;
	u32 vertex_arrays_offsets[rsx::limits::vertex_count];

	const std::string reg_table[] =
	{
		"in_pos_buffer", "in_weight_buffer", "in_normal_buffer",
		"in_diff_color_buffer", "in_spec_color_buffer",
		"in_fog_buffer",
		"in_point_size_buffer", "in_7_buffer",
		"in_tc0_buffer", "in_tc1_buffer", "in_tc2_buffer", "in_tc3_buffer",
		"in_tc4_buffer", "in_tc5_buffer", "in_tc6_buffer", "in_tc7_buffer"
	};

	u32 input_mask = rsx::method_registers[NV4097_SET_VERTEX_ATTRIB_INPUT_MASK];

	std::vector<u8> vertex_index_array;
	vertex_draw_count = 0;
	u32 min_index, max_index;

	if (draw_command == rsx::draw_command::indexed)
	{
		rsx::index_array_type type = rsx::to_index_array_type(rsx::method_registers[NV4097_SET_INDEX_ARRAY_DMA] >> 4);
		u32 type_size = gsl::narrow<u32>(get_index_type_size(type));
		for (const auto& first_count : first_count_commands)
		{
			vertex_draw_count += first_count.second;
		}

		vertex_index_array.resize(vertex_draw_count * type_size);

		switch (type)
		{
		case rsx::index_array_type::u32:
			std::tie(min_index, max_index) = write_index_array_data_to_buffer_untouched(gsl::span<u32>((u32*)vertex_index_array.data(), vertex_draw_count), first_count_commands);
			break;
		case rsx::index_array_type::u16:
			std::tie(min_index, max_index) = write_index_array_data_to_buffer_untouched(gsl::span<u16>((u16*)vertex_index_array.data(), vertex_draw_count), first_count_commands);
			break;
		}
	}

	if (draw_command == rsx::draw_command::inlined_array)
	{
		vertex_arrays_data.resize(inline_vertex_array.size() * sizeof(u32));
		write_inline_array_to_buffer(vertex_arrays_data.data());
		u32 offset = 0;
		for (int index = 0; index < rsx::limits::vertex_count; ++index)
		{
			auto &vertex_info = vertex_arrays_info[index];

			if (!vertex_info.size) // disabled
				continue;

			if (!m_program->has_uniform(vk::glsl::glsl_vertex_program, reg_table[index]))
				continue;

			const u32 element_size = rsx::get_vertex_type_size_on_host(vertex_info.type, vertex_info.size);
			const u32 data_size = element_size * vertex_draw_count;
			const VkFormat format = vk::get_suitable_vk_format(vertex_info.type, vertex_info.size);

			auto &buffer = m_attrib_buffers[index];

			buffer.sub_data(0, data_size, vertex_arrays_data.data() + offset);
			buffer.set_format(format);
			//Attach buffer to texture
			//texture->copy_from(*buffer, gl_type);

			//Link texture to uniform location
			m_program->bind_uniform(vk::glsl::glsl_vertex_program, reg_table[index], buffer, true);

			offset += rsx::get_vertex_type_size_on_host(vertex_info.type, vertex_info.size);
		}
	}

	if (draw_command == rsx::draw_command::array)
	{
		for (const auto &first_count : first_count_commands)
		{
			vertex_draw_count += first_count.second;
		}
	}

	if (draw_command == rsx::draw_command::array || draw_command == rsx::draw_command::indexed)
	{
		for (int index = 0; index < rsx::limits::vertex_count; ++index)
		{
			bool enabled = !!(input_mask & (1 << index));
			if (!enabled)
				continue;

			if (!m_program->has_uniform(vk::glsl::glsl_vertex_program, reg_table[index]))
				continue;

			if (vertex_arrays_info[index].size > 0)
			{
				auto &vertex_info = vertex_arrays_info[index];
				// Active vertex array
				std::vector<u8> vertex_array;

				// Fill vertex_array
				u32 element_size = rsx::get_vertex_type_size_on_host(vertex_info.type, vertex_info.size);
				vertex_array.resize(vertex_draw_count * sizeof(u32)*4);
				if (draw_command == rsx::draw_command::array)
				{
					size_t offset = 0;
					for (const auto &first_count : first_count_commands)
					{
						write_vertex_array_data_to_buffer(vertex_array.data() + offset, first_count.first, first_count.second, index, vertex_info, true);
						offset += first_count.second * element_size;
					}
				}
				if (draw_command == rsx::draw_command::indexed)
				{
					vertex_array.resize((max_index + 1) * element_size);
					write_vertex_array_data_to_buffer(vertex_array.data(), 0, max_index + 1, index, vertex_info, true);
				}

				const VkFormat format = vk::get_suitable_vk_format(vertex_info.type, vertex_info.size);
				u32 data_size = element_size * vertex_draw_count;

				auto &buffer = m_attrib_buffers[index];

				if (vertex_info.size == 3)
				{
					if (vk::requires_component_expansion(vertex_info.type, vertex_info.size))
						data_size = (data_size * 4) / 3;
				}

				buffer.sub_data(0, data_size, vertex_array.data());
				buffer.set_format(format);
				//Attach buffer to texture
				//texture->copy_from(*buffer, gl_type);

				//Link texture to uniform
				m_program->bind_uniform(vk::glsl::glsl_vertex_program, reg_table[index], buffer, true);
			}
			else if (register_vertex_info[index].size > 0)
			{
				//Untested!
				auto &vertex_data = register_vertex_data[index];
				auto &vertex_info = register_vertex_info[index];

				switch (vertex_info.type)
				{
				case rsx::vertex_base_type::f:
				{
					const size_t data_size = vertex_data.size();
					const VkFormat format = vk::get_suitable_vk_format(vertex_info.type, vertex_info.size);

					auto &buffer = m_attrib_buffers[index];

					buffer.sub_data(0, data_size, vertex_data.data());
					buffer.set_format(format);

					//Attach buffer to texture
					//texture->copy_from(*buffer, gl_type);

					//Link texture to uniform
					m_program->bind_uniform(vk::glsl::glsl_vertex_program, reg_table[index], buffer, true);
					break;
				}
				default:
					LOG_ERROR(RSX, "bad non array vertex data format (type = %d, size = %d)", vertex_info.type, vertex_info.size);
					break;
				}
			}
		}
	}

	bool is_indexed_draw = (draw_command == rsx::draw_command::indexed);
	bool index_buffer_filled = false;
	bool primitives_emulated = false;
	u32  index_count = 0;
	VkIndexType index_format = VK_INDEX_TYPE_UINT16;

	m_program->set_primitive_topology(vk::get_appropriate_topology(draw_mode, primitives_emulated));
	m_program->use(m_command_buffer, m_render_pass, 0);

	if (primitives_emulated)
	{	
		//Line loops are line-strips with loop-back; using line-strips-with-adj doesnt work for vulkan
		if (draw_mode == rsx::primitive_type::line_loop)
		{
			std::vector<u16> indices;

			if (!is_indexed_draw)
			{
				index_count = vk::expand_line_loop_array_to_strip(vertex_draw_count, indices);
				m_index_buffer.sub_data(0, index_count*sizeof(u16), indices.data());
			}
			else
			{
				rsx::index_array_type indexed_type = rsx::to_index_array_type(rsx::method_registers[NV4097_SET_INDEX_ARRAY_DMA] >> 4);
				if (indexed_type == rsx::index_array_type::u32)
				{
					index_format = VK_INDEX_TYPE_UINT32;
					std::vector<u32> indices32;
					
					index_count = vk::expand_indexed_line_loop_to_strip(vertex_draw_count, (u32*)vertex_index_array.data(), indices32);
					m_index_buffer.sub_data(0, index_count*sizeof(u32), indices32.data());
				}
				else
				{
					index_count = vk::expand_indexed_line_loop_to_strip(vertex_draw_count, (u16*)vertex_index_array.data(), indices);
					m_index_buffer.sub_data(0, index_count*sizeof(u16), indices.data());
				}
			}
		}
		else
		{
			index_count = get_index_count(draw_mode, vertex_draw_count);
			std::vector<u16> indices(index_count);

			if (is_indexed_draw)
			{
				rsx::index_array_type indexed_type = rsx::to_index_array_type(rsx::method_registers[NV4097_SET_INDEX_ARRAY_DMA] >> 4);
				size_t index_size = get_index_type_size(indexed_type);

				std::vector<std::pair<u32, u32>> ranges;
				ranges.push_back(std::pair<u32, u32>(0, vertex_draw_count));

				gsl::span<u16> dst = { (u16*)indices.data(), gsl::narrow<int>(index_count) };
				write_index_array_data_to_buffer(dst, draw_mode, ranges);
			}
			else
			{
				write_index_array_for_non_indexed_non_native_primitive_to_buffer(reinterpret_cast<char*>(indices.data()), draw_mode, 0, vertex_draw_count);
			}

			m_index_buffer.sub_data(0, index_count * sizeof(u16), indices.data());
		}

		is_indexed_draw = true;
		index_buffer_filled = true;
	}
	
	if (!is_indexed_draw)
		vkCmdDraw(m_command_buffer, vertex_draw_count, 1, 0, 0);
	else
	{
		if (index_buffer_filled)
		{
			vkCmdBindIndexBuffer(m_command_buffer, m_index_buffer, 0, index_format);
			vkCmdDrawIndexed(m_command_buffer, index_count, 1, 0, 0, 0);
		}
		else
		{
			rsx::index_array_type indexed_type = rsx::to_index_array_type(rsx::method_registers[NV4097_SET_INDEX_ARRAY_DMA] >> 4);
			VkIndexType itype = VK_INDEX_TYPE_UINT16;
			VkFormat fmt = VK_FORMAT_R16_UINT;
			u32 elem_size = get_index_type_size(indexed_type);

			if (indexed_type == rsx::index_array_type::u32)
			{
				itype = VK_INDEX_TYPE_UINT32;
				fmt = VK_FORMAT_R32_UINT;
			}

			u32 index_sz = vertex_index_array.size() / elem_size;
			if (index_sz != vertex_draw_count)
				LOG_ERROR(RSX, "Vertex draw count mismatch!");

			m_index_buffer.sub_data(0, index_sz, vertex_index_array.data());
			m_index_buffer.set_format(fmt);		//Unnecessary unless viewing contents in sampler...

			vkCmdBindIndexBuffer(m_command_buffer, m_index_buffer, 0, itype);
			vkCmdDrawIndexed(m_command_buffer, vertex_draw_count, 1, 0, 0, 0);
		}
	}

	vkCmdEndRenderPass(m_command_buffer);

	m_texture_cache.flush(m_command_buffer);

	recording = false;
	end_command_buffer_recording();
	execute_command_buffer(false);

	rsx::thread::end();
}

void VKGSRender::set_viewport()
{
	u32 viewport_horizontal = rsx::method_registers[NV4097_SET_VIEWPORT_HORIZONTAL];
	u32 viewport_vertical = rsx::method_registers[NV4097_SET_VIEWPORT_VERTICAL];

	u16 viewport_x = viewport_horizontal & 0xffff;
	u16 viewport_y = viewport_vertical & 0xffff;
	u16 viewport_w = viewport_horizontal >> 16;
	u16 viewport_h = viewport_vertical >> 16;

	u32 scissor_horizontal = rsx::method_registers[NV4097_SET_SCISSOR_HORIZONTAL];
	u32 scissor_vertical = rsx::method_registers[NV4097_SET_SCISSOR_VERTICAL];
	u16 scissor_x = scissor_horizontal;
	u16 scissor_w = scissor_horizontal >> 16;
	u16 scissor_y = scissor_vertical;
	u16 scissor_h = scissor_vertical >> 16;

//	u32 shader_window = rsx::method_registers[NV4097_SET_SHADER_WINDOW];
//	rsx::window_origin shader_window_origin = rsx::to_window_origin((shader_window >> 12) & 0xf);

	VkViewport viewport;
	viewport.x = viewport_x;
	viewport.y = viewport_y;
	viewport.width = viewport_w;
	viewport.height = viewport_h;
	viewport.minDepth = 0.f;
	viewport.maxDepth = 1.f;

	vkCmdSetViewport(m_command_buffer, 0, 1, &viewport);

	VkRect2D scissor;
	scissor.extent.height = scissor_h;
	scissor.extent.width = scissor_w;
	scissor.offset.x = scissor_x;
	scissor.offset.y = scissor_y;

	vkCmdSetScissor(m_command_buffer, 0, 1, &scissor);
}

void VKGSRender::on_init_thread()
{
	GSRender::on_init_thread();

	for (auto &attrib_buffer : m_attrib_buffers)
	{
		attrib_buffer.create((*m_device), 65536, VK_FORMAT_R32G32B32A32_SFLOAT, VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT);
	}
}

void VKGSRender::on_exit()
{
	m_texture_cache.destroy();
	
	for (auto &attrib_buffer : m_attrib_buffers)
	{
		attrib_buffer.destroy();
	}
}

void VKGSRender::clear_surface(u32 mask)
{
	//TODO: Build clear commands into current renderpass descriptor set
	if (!(mask & 0xF3)) return;

	if (m_current_present_image== 0xFFFF || recording) return;
	//return;
	begin_command_buffer_recording();

	float depth_clear = 1.f;
	u32   stencil_clear = 0.f;

	VkClearValue depth_stencil_clear_values, color_clear_values;
	VkImageSubresourceRange depth_range = vk::default_image_subresource_range();
	depth_range.aspectMask = 0;

	if (mask & 0x1)
	{
		rsx::surface_depth_format surface_depth_format = rsx::to_surface_depth_format((rsx::method_registers[NV4097_SET_SURFACE_FORMAT] >> 5) & 0x7);
		u32 max_depth_value = get_max_depth_value(surface_depth_format);

		u32 clear_depth = rsx::method_registers[NV4097_SET_ZSTENCIL_CLEAR_VALUE] >> 8;
		float depth_clear = (float)clear_depth / max_depth_value;

		depth_range.aspectMask |= VK_IMAGE_ASPECT_DEPTH_BIT;
		depth_stencil_clear_values.depthStencil.depth = depth_clear;
		depth_stencil_clear_values.depthStencil.stencil = stencil_clear;
	}

	if (mask & 0x2)
	{
		u8 clear_stencil = rsx::method_registers[NV4097_SET_ZSTENCIL_CLEAR_VALUE] & 0xff;
		u32 stencil_mask = rsx::method_registers[NV4097_SET_STENCIL_MASK];

		//TODO set stencil mask
		depth_range.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
		depth_stencil_clear_values.depthStencil.stencil = stencil_mask;
	}

	if (mask & 0xF0)
	{
		u32 clear_color = rsx::method_registers[NV4097_SET_COLOR_CLEAR_VALUE];
		u8 clear_a = clear_color >> 24;
		u8 clear_r = clear_color >> 16;
		u8 clear_g = clear_color >> 8;
		u8 clear_b = clear_color;

		//TODO set color mask
		/*VkBool32 clear_red = (VkBool32)!!(mask & 0x20);
		VkBool32 clear_green = (VkBool32)!!(mask & 0x40);
		VkBool32 clear_blue = (VkBool32)!!(mask & 0x80);
		VkBool32 clear_alpha = (VkBool32)!!(mask & 0x10);*/

		color_clear_values.color.float32[0] = (float)clear_r / 255;
		color_clear_values.color.float32[1] = (float)clear_g / 255;
		color_clear_values.color.float32[2] = (float)clear_b / 255;
		color_clear_values.color.float32[3] = (float)clear_a / 255;

		VkImageSubresourceRange range = vk::default_image_subresource_range();

		for (u32 i = 0; i < m_nb_targets; ++i)
		{
			VkImage color_image = m_fbo_surfaces[i];
			vkCmdClearColorImage(m_command_buffer, color_image, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, &color_clear_values.color, 1, &range);
		}
	}

	if (mask & 0x3)
		vkCmdClearDepthStencilImage(m_command_buffer, m_depth_buffer, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, &depth_stencil_clear_values.depthStencil, 1, &depth_range);

	end_command_buffer_recording();
	execute_command_buffer(false);

	//LOG_ERROR(RSX, ">>>> Clear surface finished");
}

bool VKGSRender::do_method(u32 cmd, u32 arg)
{
	switch (cmd)
	{
	case NV4097_CLEAR_SURFACE:
		clear_surface(arg);
		return true;
	default:
		return false;
	}
}

void VKGSRender::init_render_pass(VkFormat surface_format, int num_draw_buffers, int *draw_buffers)
{
	//TODO: Create buffers as requested by the game. Render to swapchain for now..
	/* Describe a render pass and framebuffer attachments */
	VkAttachmentDescription attachments[2];
	memset(&attachments, 0, sizeof attachments);

	attachments[0].format = surface_format;
	attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
	attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;							//Set to clear removes warnings about empty contents after flip; overwrites previous calls
	attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachments[0].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;	//PRESENT_SRC_KHR??
	attachments[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

	attachments[1].format = VK_FORMAT_D16_UNORM;								/* Depth buffer format. Should be more elegant than this */
	attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
	attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
	attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachments[1].initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
	attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

	VkAttachmentReference template_color_reference;
	template_color_reference.attachment = VK_ATTACHMENT_UNUSED;
	template_color_reference.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

	VkAttachmentReference depth_reference;
	depth_reference.attachment = 1;
	depth_reference.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

	//Fill in draw_buffers information...
	VkAttachmentDescription real_attachments[4];
	VkAttachmentReference color_references[4];

	for (int i = 0; i < num_draw_buffers; ++i)
	{
		real_attachments[i] = attachments[0];
		
		color_references[i] = template_color_reference;
		color_references[i].attachment = (draw_buffers)? draw_buffers[i]: i;
	}

	real_attachments[num_draw_buffers] = attachments[1];

	VkSubpassDescription subpass;
	subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.flags = 0;
	subpass.inputAttachmentCount = 0;
	subpass.pInputAttachments = nullptr;
	subpass.colorAttachmentCount = num_draw_buffers;
	subpass.pColorAttachments = color_references;
	subpass.pResolveAttachments = nullptr;
	subpass.pDepthStencilAttachment = &depth_reference;
	subpass.preserveAttachmentCount = 0;
	subpass.pPreserveAttachments = nullptr;

	VkRenderPassCreateInfo rp_info;
	rp_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	rp_info.pNext = NULL;
	rp_info.attachmentCount = num_draw_buffers+1;
	rp_info.pAttachments = real_attachments;
	rp_info.subpassCount = 1;
	rp_info.pSubpasses = &subpass;
	rp_info.dependencyCount = 0;
	rp_info.pDependencies = NULL;
	rp_info.flags = 0;

	CHECK_RESULT(vkCreateRenderPass((*m_device), &rp_info, NULL, &m_render_pass));
}

void VKGSRender::destroy_render_pass()
{
	vkDestroyRenderPass((*m_device), m_render_pass, nullptr);
	m_render_pass = nullptr;
}

bool VKGSRender::load_program()
{
	RSXVertexProgram vertex_program = get_current_vertex_program();
	RSXFragmentProgram fragment_program = get_current_fragment_program();

	//Load current program from buffer
	m_program = &m_prog_buffer.getGraphicPipelineState(vertex_program, fragment_program, nullptr);

	//TODO: Update constant buffers..
	//1. Update scale-offset matrix
	//2. Update vertex constants
	//3. Update fragment constants
	u8 *buf = (u8*)m_scale_offset_buffer.map(0, VK_WHOLE_SIZE);

	//TODO: Add case for this in RSXThread
	/**
	 * NOTE: While VK's coord system resembles GLs, the clip volume is no longer symetrical in z
	 * Its like D3D without the flip in y (depending on how you build the spir-v)
	 */
	{
		int clip_w = rsx::method_registers[NV4097_SET_SURFACE_CLIP_HORIZONTAL] >> 16;
		int clip_h = rsx::method_registers[NV4097_SET_SURFACE_CLIP_VERTICAL] >> 16;

		float scale_x = (float&)rsx::method_registers[NV4097_SET_VIEWPORT_SCALE] / (clip_w / 2.f);
		float offset_x = (float&)rsx::method_registers[NV4097_SET_VIEWPORT_OFFSET] - (clip_w / 2.f);
		offset_x /= clip_w / 2.f;

		float scale_y = (float&)rsx::method_registers[NV4097_SET_VIEWPORT_SCALE + 1] / (clip_h / 2.f);
		float offset_y = ((float&)rsx::method_registers[NV4097_SET_VIEWPORT_OFFSET + 1] - (clip_h / 2.f));
		offset_y /= clip_h / 2.f;

		float scale_z = (float&)rsx::method_registers[NV4097_SET_VIEWPORT_SCALE + 2];
		float offset_z = (float&)rsx::method_registers[NV4097_SET_VIEWPORT_OFFSET + 2];

		float one = 1.f;

		stream_vector(buf, (u32&)scale_x, 0, 0, (u32&)offset_x);
		stream_vector((char*)buf + 16, 0, (u32&)scale_y, 0, (u32&)offset_y);
		stream_vector((char*)buf + 32, 0, 0, (u32&)scale_z, (u32&)offset_z);
		stream_vector((char*)buf + 48, 0, 0, 0, (u32&)one);
	}

	m_scale_offset_buffer.unmap();

	buf = (u8*)m_vertex_constants_buffer.map(0, VK_WHOLE_SIZE);
	fill_vertex_program_constants_data(buf);
	m_vertex_constants_buffer.unmap();

	size_t fragment_constants_sz = m_prog_buffer.get_fragment_constants_buffer_size(fragment_program);
	buf = (u8*)m_fragment_constants_buffer.map(0, fragment_constants_sz);
	m_prog_buffer.fill_fragment_constans_buffer({ reinterpret_cast<float*>(buf), gsl::narrow<int>(fragment_constants_sz) }, fragment_program);
	m_fragment_constants_buffer.unmap();

	m_program->bind_uniform(vk::glsl::glsl_vertex_program, "ScaleOffsetBuffer", m_scale_offset_buffer);
	m_program->bind_uniform(vk::glsl::glsl_vertex_program, "VertexConstantsBuffer", m_vertex_constants_buffer);
	m_program->bind_uniform(vk::glsl::glsl_fragment_program, "FragmentConstantsBuffer", m_fragment_constants_buffer);

	return true;
}

void VKGSRender::init_buffers(bool skip_reading)
{
	u32 surface_format = rsx::method_registers[NV4097_SET_SURFACE_FORMAT];

	u32 clip_horizontal = rsx::method_registers[NV4097_SET_SURFACE_CLIP_HORIZONTAL];
	u32 clip_vertical = rsx::method_registers[NV4097_SET_SURFACE_CLIP_VERTICAL];

	u32 clip_width = clip_horizontal >> 16;
	u32 clip_height = clip_vertical >> 16;
	u32 clip_x = clip_horizontal;
	u32 clip_y = clip_vertical;

	VkFormat requested_format = vk::get_compatible_surface_format((rsx::surface_color_format)surface_format);

	if (dirty_frame)
	{
		//Prepare surface for new frame
		VkSemaphoreCreateInfo semaphore_info;
		semaphore_info.flags = 0;
		semaphore_info.pNext = nullptr;
		semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

		vkCreateSemaphore((*m_device), &semaphore_info, nullptr, &m_present_semaphore);

		VkFence nullFence = VK_NULL_HANDLE;
		CHECK_RESULT(vkAcquireNextImageKHR((*m_device), (*m_swap_chain), 0, m_present_semaphore, nullFence, &m_current_present_image));

		dirty_frame = false;
	}

	//Choose the number of outputs and initialize render pass descriptor accordingly
	rsx::surface_target fmt = rsx::to_surface_target(rsx::method_registers[NV4097_SET_SURFACE_COLOR_TARGET]);

	if (fmt != m_current_targets || m_render_pass == nullptr)
	{
		m_current_fbo = 0;

		u32 num_targets = 0;
		int draw_buffers[] = {0, 1, 2, 3};

		switch (fmt)
		{
		case rsx::surface_target::none: break;

		case rsx::surface_target::surface_a:
			num_targets = 1;
			break;

		case rsx::surface_target::surface_b:
			draw_buffers[0] = 1;
			draw_buffers[1] = 0;
			num_targets = 2;
			m_current_fbo = 1;
			break;

		case rsx::surface_target::surfaces_a_b:
			num_targets = 2;
			m_current_fbo = 1;
			break;

		case rsx::surface_target::surfaces_a_b_c:
			num_targets = 3;
			m_current_fbo = 2;
			break;

		case rsx::surface_target::surfaces_a_b_c_d:
			num_targets = 4;
			m_current_fbo = 3;
			break;

		default:
			LOG_ERROR(RSX, "Bad surface color target: %d", rsx::method_registers[NV4097_SET_SURFACE_COLOR_TARGET]);
			break;
		}

		if (m_render_pass)
			destroy_render_pass();

		init_render_pass(requested_format, num_targets, draw_buffers);
		m_current_targets = fmt;
		m_nb_targets = num_targets;
	}

	//TODO: if fbo is different from previous, recreate the fbo
	if (m_surface_format != requested_format)
	{
		u32 width = clip_width;
		u32 height = clip_height;

		for (vk::texture &tex : m_fbo_surfaces)
		{
			tex.destroy();
			tex.create((*m_device), requested_format, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT, width, height);
			vk::change_image_layout(m_command_buffer, tex, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
		}

		m_depth_buffer.destroy();
		m_depth_buffer.create((*m_device), VK_FORMAT_D16_UNORM, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, width, height);
		vk::change_image_layout(m_command_buffer, m_depth_buffer, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);

		for (int i = 0; i < 4; ++i)
		{
			vk::framebuffer &surf = m_framebuffers[i];
			VkImageView attachments[5];

			for (int j = 0; j <= i; ++j)
				attachments[j] = m_fbo_surfaces[j];

			attachments[i + 1] = m_depth_buffer;

			surf.destroy();
			surf.create((*m_device), m_render_pass, attachments, i + 2, width, height);
		}

		m_surface_format = requested_format;
		clear_surface(0xF3);
	}

	if (!skip_reading)
	{
		read_buffers();
	}

	set_viewport();
}

static const u32 mr_color_offset[rsx::limits::color_buffers_count] =
{
	NV4097_SET_SURFACE_COLOR_AOFFSET,
	NV4097_SET_SURFACE_COLOR_BOFFSET,
	NV4097_SET_SURFACE_COLOR_COFFSET,
	NV4097_SET_SURFACE_COLOR_DOFFSET
};

static const u32 mr_color_dma[rsx::limits::color_buffers_count] =
{
	NV4097_SET_CONTEXT_DMA_COLOR_A,
	NV4097_SET_CONTEXT_DMA_COLOR_B,
	NV4097_SET_CONTEXT_DMA_COLOR_C,
	NV4097_SET_CONTEXT_DMA_COLOR_D
};

static const u32 mr_color_pitch[rsx::limits::color_buffers_count] =
{
	NV4097_SET_SURFACE_PITCH_A,
	NV4097_SET_SURFACE_PITCH_B,
	NV4097_SET_SURFACE_PITCH_C,
	NV4097_SET_SURFACE_PITCH_D
};

void VKGSRender::read_buffers()
{
}

void VKGSRender::write_buffers()
{
}

void VKGSRender::begin_command_buffer_recording()
{
	VkCommandBufferInheritanceInfo inheritance_info;
	inheritance_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO;
	inheritance_info.pNext = nullptr;
	inheritance_info.renderPass = VK_NULL_HANDLE;
	inheritance_info.subpass = 0;
	inheritance_info.framebuffer = VK_NULL_HANDLE;
	inheritance_info.occlusionQueryEnable = VK_FALSE;
	inheritance_info.queryFlags = 0;
	inheritance_info.pipelineStatistics = 0;

	VkCommandBufferBeginInfo begin_infos;
	begin_infos.flags = 0;
	begin_infos.pInheritanceInfo = &inheritance_info;
	begin_infos.pNext = nullptr;
	begin_infos.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

	if (m_submit_fence)
	{
		vkWaitForFences(*m_device, 1, &m_submit_fence, VK_TRUE, ~0ULL);
		vkDestroyFence(*m_device, m_submit_fence, nullptr);
		m_submit_fence = nullptr;

		CHECK_RESULT(vkResetCommandBuffer(m_command_buffer, 0));
	}

	CHECK_RESULT(vkBeginCommandBuffer(m_command_buffer, &begin_infos));
	recording = true;
}

void VKGSRender::end_command_buffer_recording()
{
	recording = false;
	CHECK_RESULT(vkEndCommandBuffer(m_command_buffer));
}

void VKGSRender::execute_command_buffer(bool wait)
{
	if (recording)
		throw EXCEPTION("execute_command_buffer called before end_command_buffer_recording()!");

	if (m_submit_fence)
		throw EXCEPTION("Synchronization deadlock!");

	VkFenceCreateInfo fence_info;
	fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	fence_info.flags = 0;
	fence_info.pNext = nullptr;

	CHECK_RESULT(vkCreateFence(*m_device, &fence_info, nullptr, &m_submit_fence));

	VkPipelineStageFlags pipe_stage_flags = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
	VkCommandBuffer cmd = m_command_buffer;

	VkSubmitInfo infos;
	infos.commandBufferCount = 1;
	infos.pCommandBuffers = &cmd;
	infos.pNext = nullptr;
	infos.pSignalSemaphores = nullptr;
	infos.pWaitDstStageMask = &pipe_stage_flags;
	infos.signalSemaphoreCount = 0;	
	infos.waitSemaphoreCount = 0;
	infos.pWaitSemaphores = nullptr;
	infos.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;

	CHECK_RESULT(vkQueueSubmit(m_swap_chain->get_present_queue(), 1, &infos, m_submit_fence));
	CHECK_RESULT(vkQueueWaitIdle(m_swap_chain->get_present_queue()));
}

void VKGSRender::flip(int buffer)
{
	//LOG_NOTICE(Log::RSX, "flip(%d)", buffer);
	u32 buffer_width = gcm_buffers[buffer].width;
	u32 buffer_height = gcm_buffers[buffer].height;
	u32 buffer_pitch = gcm_buffers[buffer].pitch;

	rsx::tiled_region buffer_region = get_tiled_address(gcm_buffers[buffer].offset, CELL_GCM_LOCATION_LOCAL);

	areai screen_area = coordi({}, { (int)buffer_width, (int)buffer_height });

	coordi aspect_ratio;
	if (1) //enable aspect ratio
	{
		sizei csize = m_frame->client_size();
		sizei new_size = csize;

		const double aq = (double)buffer_width / buffer_height;
		const double rq = (double)new_size.width / new_size.height;
		const double q = aq / rq;

		if (q > 1.0)
		{
			new_size.height = int(new_size.height / q);
			aspect_ratio.y = (csize.height - new_size.height) / 2;
		}
		else if (q < 1.0)
		{
			new_size.width = int(new_size.width * q);
			aspect_ratio.x = (csize.width - new_size.width) / 2;
		}

		aspect_ratio.size = new_size;
	}
	else
	{
		aspect_ratio.size = m_frame->client_size();
	}

	//Check if anything is waiting in queue and submit it if possible..
	if (m_submit_fence)
	{
		CHECK_RESULT(vkWaitForFences((*m_device), 1, &m_submit_fence, VK_TRUE, ~0ULL));
		
		vkDestroyFence((*m_device), m_submit_fence, nullptr);
		m_submit_fence = nullptr;

		CHECK_RESULT(vkResetCommandBuffer(m_command_buffer, 0));
	}

	VkSwapchainKHR swap_chain = (VkSwapchainKHR)(*m_swap_chain);
	uint32_t next_image_temp = 0;

	VkPresentInfoKHR present;
	present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
	present.pNext = nullptr;
	present.swapchainCount = 1;
	present.pSwapchains = &swap_chain;
	present.pImageIndices = &m_current_present_image;
	present.pWaitSemaphores = &m_present_semaphore;
	present.waitSemaphoreCount = 1;

	if (m_render_pass)
	{
		begin_command_buffer_recording();

		if (m_present_semaphore)
		{
			//Blit contents to screen..
			VkImage image_to_flip = m_fbo_surfaces[0];
			VkImage target_image = m_swap_chain->get_swap_chain_image(m_current_present_image);

			//TODO SCALING
			vk::copy_scaled_image(m_command_buffer, image_to_flip, target_image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
									buffer_width, buffer_height, aspect_ratio.width, aspect_ratio.height, 1, VK_IMAGE_ASPECT_COLOR_BIT);
		}
		else
		{
			//No draw call was issued!
			//TODO: Properly clear the background to rsx value
			m_swap_chain->acquireNextImageKHR((*m_device), (*m_swap_chain), ~0ULL, VK_NULL_HANDLE, VK_NULL_HANDLE, &next_image_temp);

			VkImageSubresourceRange range = vk::default_image_subresource_range();
			VkClearColorValue clear_black = { 0 };
			vkCmdClearColorImage(m_command_buffer, m_swap_chain->get_swap_chain_image(next_image_temp), VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, &clear_black, 1, &range);
			
			present.pImageIndices = &next_image_temp;
			present.waitSemaphoreCount = 0;
		}

		end_command_buffer_recording();
		execute_command_buffer(false);

		CHECK_RESULT(m_swap_chain->queuePresentKHR(m_swap_chain->get_present_queue(), &present));
		CHECK_RESULT(vkQueueWaitIdle(m_swap_chain->get_present_queue()));
		
		if (m_present_semaphore)
		{
			vkDestroySemaphore((*m_device), m_present_semaphore, nullptr);
			m_present_semaphore = nullptr;
		}
	}

	m_draw_calls = 0;
	dirty_frame = true;
	m_frame->flip(m_context);
}
