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
	m_depth_buffer.create((*m_device), VK_FORMAT_D16_UNORM, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, m_frame->client_size().width, m_frame->client_size().height);

	//create command buffer...
	m_command_buffer_pool.create((*m_device));
	m_command_buffer.create(m_command_buffer_pool);

	//Create render pass
	init_render_pass();

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

	vk::change_image_layout(m_command_buffer, m_depth_buffer,
							VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
							VK_IMAGE_ASPECT_DEPTH_BIT);

	clear_surface(0xFFFFFFFF);
	
	CHECK_RESULT(vkEndCommandBuffer(m_command_buffer));
	execute_command_buffer(false);

	for (u32 i = 0; i < m_swap_chain->get_swap_image_count(); ++i)
	{
		VkImageView attachments[] = { m_swap_chain->get_swap_chain_image(i), m_depth_buffer };
		m_framebuffers[i].create((*m_device), m_render_pass, attachments, 2, m_frame->client_size().width, m_frame->client_size().height);
	}

	m_scale_offset_buffer.create((*m_device), 64);
	m_vertex_constants_buffer.create((*m_device), 512 * 16);
	m_fragment_constants_buffer.create((*m_device), 512 * 16);
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

	m_prog_buffer.clear();
	m_scale_offset_buffer.destroy();
	m_vertex_constants_buffer.destroy();
	m_fragment_constants_buffer.destroy();

	for (u32 i = 0; i < m_swap_chain->get_swap_image_count(); ++i)
		m_framebuffers[i].destroy();

	destroy_render_pass();

	m_command_buffer.destroy();
	m_command_buffer_pool.destroy();

	m_depth_buffer.destroy();
	m_swap_chain->destroy();
	m_thread_context.close();
	delete m_swap_chain;
}

u32 VKGSRender::enable(u32 condition, u32 cap)
{
	return condition;
}

u32 VKGSRender::enable(u32 condition, u32 cap, u32 index)
{
	return condition;
}

extern CellGcmContextData current_context;

void VKGSRender::begin()
{
	rsx::thread::begin();

	CHECK_RESULT(vkDeviceWaitIdle((*m_device)));

	if (!load_program())
		return;

/*	u32 color_mask = rsx::method_registers[NV4097_SET_COLOR_MASK];
	bool color_mask_b = !!(color_mask & 0xff);
	bool color_mask_g = !!((color_mask >> 8) & 0xff);
	bool color_mask_r = !!((color_mask >> 16) & 0xff);
	bool color_mask_a = !!((color_mask >> 24) & 0xff);

	u32 depth_mask = rsx::method_registers[NV4097_SET_DEPTH_MASK];
	u32 stencil_mask = rsx::method_registers[NV4097_SET_STENCIL_MASK];

	if (rsx::method_registers[NV4097_SET_DEPTH_TEST_ENABLE])
	{
		m_program->set_depth_test_enable(VK_TRUE);
		m_program->set_depth_compare_op(vk::compare_op(rsx::method_registers[NV4097_SET_DEPTH_FUNC]));
		m_program->set_depth_write_mask(rsx::method_registers[NV4097_SET_DEPTH_MASK]);
	}*/

	//TODO: Set up other render-state parameters into the program pipeline
	if (!recording)
		begin_command_buffer_recording();

	init_buffers();

	//LOG_ERROR(RSX, ">>> Draw call begin!");

/*	VkClearValue clear_values[2];
	clear_values[0].color.float32[0] = 0.2f;
	clear_values[1].depthStencil = { 1.0f, 0 };*/

	VkRenderPassBeginInfo rp_begin;
	rp_begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	rp_begin.pNext = NULL;
	rp_begin.renderPass = m_render_pass;
	rp_begin.framebuffer = m_framebuffers[m_current_present_image];
	rp_begin.renderArea.offset.x = 0;
	rp_begin.renderArea.offset.y = 0;
	rp_begin.renderArea.extent.width = m_frame->client_size().width;
	rp_begin.renderArea.extent.height = m_frame->client_size().height;
	rp_begin.clearValueCount = 0;
	rp_begin.pClearValues = nullptr;

	vkCmdBeginRenderPass(m_command_buffer, &rp_begin, VK_SUBPASS_CONTENTS_INLINE);

	VkViewport viewport;
	viewport.x = 0;
	viewport.y = 0;
	viewport.width = m_frame->client_size().width;
	viewport.height = m_frame->client_size().height;
	viewport.minDepth = 0.f;
	viewport.maxDepth = 1.f;

	vkCmdSetViewport(m_command_buffer, 0, 1, &viewport);

	VkRect2D scissor;
	scissor.extent.height = viewport.height;
	scissor.extent.width = viewport.width;
	scissor.offset.x = 0;
	scissor.offset.y = 0;

	vkCmdSetScissor(m_command_buffer, 0, 1, &scissor);
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
		if (!textures[i].enabled())
		{
			continue;
		}

		//TODO:
		//Attach a sampler for this texture to the corresponding descriptor
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

			auto &buffer = m_gl_attrib_buffers[index];

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
				const u32 data_size = element_size * vertex_draw_count;

				auto &buffer = m_gl_attrib_buffers[index];

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

					auto &buffer = m_gl_attrib_buffers[index];

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

	m_program->use(m_command_buffer, m_render_pass, 0);
	
	if (draw_command != rsx::draw_command::indexed)
		vkCmdDraw(m_command_buffer, vertex_draw_count, 1, 0, 0);
	else
		LOG_ERROR(RSX, "Indexed draw!");

	vkCmdEndRenderPass(m_command_buffer);

	LOG_ERROR(RSX, ">> Draw call end, %d!", m_draw_calls);
	recording = false;
	end_command_buffer_recording();
	execute_command_buffer(false);

	Sleep(10);

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

	u32 shader_window = rsx::method_registers[NV4097_SET_SHADER_WINDOW];

	rsx::window_origin shader_window_origin = rsx::to_window_origin((shader_window >> 12) & 0xf);

	//TODO set viewport rules into either program pipeline state or command buffer
	LOG_ERROR(RSX, "Set viewport call unhandled!");
}

void VKGSRender::on_init_thread()
{
	GSRender::on_init_thread();

	for (auto &attrib_buffer : m_gl_attrib_buffers)
	{
		attrib_buffer.create((*m_device), 65536, VK_FORMAT_R32G32B32A32_SFLOAT, VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT);
	}
}

void VKGSRender::on_exit()
{
	for (auto &attrib_buffer : m_gl_attrib_buffers)
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
	float color_clear[] = { 0.f, 0.f, 0.f, 0.f };
	u32   stencil_clear = 0.f;

	VkClearValue clear_values;
	VkImageSubresourceRange depth_range = vk::default_image_subresource_range();
	depth_range.aspectMask = 0;

	if (mask & 0x1)
	{
		rsx::surface_depth_format surface_depth_format = rsx::to_surface_depth_format((rsx::method_registers[NV4097_SET_SURFACE_FORMAT] >> 5) & 0x7);
		u32 max_depth_value = get_max_depth_value(surface_depth_format);

		u32 clear_depth = rsx::method_registers[NV4097_SET_ZSTENCIL_CLEAR_VALUE] >> 8;
		float depth_clear = (float)clear_depth / max_depth_value;

		depth_range.aspectMask |= VK_IMAGE_ASPECT_DEPTH_BIT;
		clear_values.depthStencil.depth = depth_clear;
		clear_values.depthStencil.stencil = stencil_clear;
	}

	if (mask & 0x2)
	{
		u8 clear_stencil = rsx::method_registers[NV4097_SET_ZSTENCIL_CLEAR_VALUE] & 0xff;
		u32 stencil_mask = rsx::method_registers[NV4097_SET_STENCIL_MASK];

		//TODO set stencil mask
		depth_range.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
		clear_values.depthStencil.stencil = stencil_mask;
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

		clear_values.color.float32[0] = (float)clear_r / 255;
		clear_values.color.float32[1] = (float)clear_g / 255;
		clear_values.color.float32[2] = (float)clear_b / 255;
		clear_values.color.float32[3] = (float)clear_a / 255;

		VkImageSubresourceRange range = vk::default_image_subresource_range();
		VkImage color_image = m_swap_chain->get_swap_chain_image(m_current_present_image);
		vkCmdClearColorImage(m_command_buffer, color_image, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, &clear_values.color, 1, &range);
	}

	if (mask & 0x3)
		vkCmdClearDepthStencilImage(m_command_buffer, m_depth_buffer, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, &clear_values.depthStencil, 1, &depth_range);

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

void VKGSRender::init_render_pass()
{
	//TODO: Create buffers as requested by the game. Render to swapchain for now..
	/* Describe a render pass and framebuffer attachments */
	VkAttachmentDescription attachments[2];
	attachments[0].format = m_swap_chain->get_surface_format();
	attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
	attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;							//Set to clear removes warnings about empty contents after flip; overwrites previous calls
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

	VkAttachmentReference color_reference;
	color_reference.attachment = 0;
	color_reference.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

	VkAttachmentReference depth_reference;
	depth_reference.attachment = 1;
	depth_reference.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

	VkSubpassDescription subpass;
	subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.flags = 0;
	subpass.inputAttachmentCount = 0;
	subpass.pInputAttachments = nullptr;
	subpass.colorAttachmentCount = 1;
	subpass.pColorAttachments = &color_reference;
	subpass.pResolveAttachments = nullptr;
	subpass.pDepthStencilAttachment = &depth_reference;
	subpass.preserveAttachmentCount = 0;
	subpass.pPreserveAttachments = nullptr;

	VkRenderPassCreateInfo rp_info;
	rp_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	rp_info.pNext = NULL;
	rp_info.attachmentCount = 2;
	rp_info.pAttachments = attachments;
	rp_info.subpassCount = 1;
	rp_info.pSubpasses = &subpass;
	rp_info.dependencyCount = 0;
	rp_info.pDependencies = NULL;

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

	//COORDINATE TRANSFORMS ARE ODD WITH THIS ONE
	//COMPUTE TRANSFORM HERE FOR EXPERIMENTS
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

		vk::change_image_layout(m_command_buffer, m_swap_chain->get_swap_chain_image(m_current_present_image),
								VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
								VK_IMAGE_ASPECT_COLOR_BIT);

		dirty_frame = false;
	}

	//TODO: if !fbo exists, or fbo is different from previous, recreate the fbo

	if (!skip_reading)
	{
		read_buffers();
	}

	set_viewport();

	switch (rsx::to_surface_target(rsx::method_registers[NV4097_SET_SURFACE_COLOR_TARGET]))
	{
	case rsx::surface_target::none: break;

	case rsx::surface_target::surface_a:
		break;

	case rsx::surface_target::surface_b:
		break;

	case rsx::surface_target::surfaces_a_b:
		break;

	case rsx::surface_target::surfaces_a_b_c:
		break;

	case rsx::surface_target::surfaces_a_b_c_d:
		break;

	default:
		LOG_ERROR(RSX, "Bad surface color target: %d", rsx::method_registers[NV4097_SET_SURFACE_COLOR_TARGET]);
		break;
	}
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

/*	if (m_submit_fence)
	{
		VkResult error = vkDeviceWaitIdle((*m_device));//vkWaitForFences(*m_device, 1, &m_submit_fence, VK_TRUE, ~0LL);
		vkDestroyFence(*m_device, m_submit_fence, nullptr);
		m_submit_fence = nullptr;

		if (error)
			LOG_ERROR(RSX, "vkWaitForFences failed with error 0x%X", error);

		vkResetCommandBuffer(m_command_buffer, 0);
	}*/

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
	if (recording) throw;

/*	if (m_submit_fence)
	{
		VkResult error = vkDeviceWaitIdle((*m_device));//vkWaitForFences((*m_device), 1, &m_submit_fence, VK_TRUE, 1000000L);
		vkDestroyFence((*m_device), m_submit_fence, nullptr);
		m_submit_fence = nullptr;

		if (error) LOG_ERROR(RSX, "vkWaitForFence failed with error 0x%X", error);
	}

	vkDeviceWaitIdle((*m_device));

	VkFenceCreateInfo fence_info;
	fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	fence_info.flags = 0;
	fence_info.pNext = nullptr;

	vkCreateFence(*m_device, &fence_info, nullptr, &m_submit_fence);*/

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
	CHECK_RESULT(vkResetCommandBuffer(m_command_buffer, 0));
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

	begin_command_buffer_recording();

	VkImageMemoryBarrier barrier = { (VkStructureType)0 };
	barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barrier.pNext = NULL;
	barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
	barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

	barrier.image = m_swap_chain->get_swap_chain_image(m_current_present_image);
	VkImageMemoryBarrier *pmemory_barrier = &barrier;
	vkCmdPipelineBarrier(m_command_buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, pmemory_barrier);

	end_command_buffer_recording();
	execute_command_buffer(false);
	
	//TODO

	//Clear old screen
	//Blit contents to surface

	VkSwapchainKHR swap_chain = (VkSwapchainKHR)(*m_swap_chain);
	
	VkPresentInfoKHR present;
	present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
	present.pNext = nullptr;
	present.swapchainCount = 1;
	present.pSwapchains = &swap_chain;
	present.pImageIndices = &m_current_present_image;
	present.pWaitSemaphores = &m_present_semaphore;
	present.waitSemaphoreCount = 1;

	CHECK_RESULT(m_swap_chain->queuePresentKHR(m_swap_chain->get_present_queue(), &present));
	
	CHECK_RESULT(vkQueueWaitIdle(m_swap_chain->get_present_queue()));
	vkDestroySemaphore((*m_device), m_present_semaphore, nullptr);
	m_present_semaphore = nullptr;

	m_draw_calls = 0;
	dirty_frame = true;
	m_frame->flip(m_context);
}
