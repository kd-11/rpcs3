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
}

VKGSRender::VKGSRender() : GSRender(frame_type::Vulkan)
{
	shaders_cache.load(rsx::shader_language::glsl);

	HINSTANCE hInstance = NULL;
	HWND hWnd = (HWND)m_frame->handle();

	m_thread_context.createInstance("RPCS3");
	m_thread_context.makeCurrentInstance(1);

	std::vector<vk::device> gpus = m_thread_context.enumerateDevices();
	m_swap_chain = m_thread_context.createSwapChain(hInstance, hWnd, gpus[0]);

	m_device = (vk::render_device *)(&m_swap_chain->get_device());
	
	vk::set_current_thread_ctx(m_thread_context);
	vk::set_current_renderer(m_swap_chain->get_device());

	m_swap_chain->init_swapchain(m_frame->client_size().width, m_frame->client_size().height);
}

VKGSRender::~VKGSRender()
{
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

	if (!load_program())
		return;

	init_buffers();

	u32 color_mask = rsx::method_registers[NV4097_SET_COLOR_MASK];
	bool color_mask_b = !!(color_mask & 0xff);
	bool color_mask_g = !!((color_mask >> 8) & 0xff);
	bool color_mask_r = !!((color_mask >> 16) & 0xff);
	bool color_mask_a = !!((color_mask >> 24) & 0xff);

	u32 depth_mask = rsx::method_registers[NV4097_SET_DEPTH_MASK];
	u32 stencil_mask = rsx::method_registers[NV4097_SET_STENCIL_MASK];

	if (rsx::method_registers[NV4097_SET_DEPTH_TEST_ENABLE])
	{
		m_program->set_depth_state_enable(VK_TRUE);
		m_program->set_depth_compare_op(vk::compare_op(rsx::method_registers[NV4097_SET_DEPTH_FUNC]));
		m_program->set_depth_write_mask(rsx::method_registers[NV4097_SET_DEPTH_MASK]);
	}

	//TODO: Set up other render-state parameters into the program pipeline
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
	//TODO: build the draw command buffer and submit to graphics queue
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
}

void VKGSRender::on_init_thread()
{
	GSRender::on_init_thread();
}

void VKGSRender::on_exit()
{
}

void nv4097_clear_surface(u32 arg, VKGSRender* renderer)
{
	if ((arg & 0xf3) == 0)
	{
		//do nothing
		return;
	}
}

void VKGSRender::clear_surface(u32 mask)
{
	//TODO: Build clear commands into current renderpass descriptor set
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


	//Prepare surface for new frame
	VkSemaphoreCreateInfo semaphore_info;
	semaphore_info.flags = 0;
	semaphore_info.pNext = nullptr;
	semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

	vkCreateSemaphore((*m_device), &semaphore_info, nullptr, &vk_present_semaphore);

	VkFence nullFence = VK_NULL_HANDLE;
	vkAcquireNextImageKHR((*m_device), (*m_swap_chain), 0, vk_present_semaphore, nullFence, &current_present_image);

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

	//TODO

	//Clear old screen
	//Blit contents to surface
	
	VkSwapchainKHR swap_chain = (VkSwapchainKHR)(*m_swap_chain);
	
	VkPresentInfoKHR present;
	present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
	present.pNext = nullptr;
	present.swapchainCount = 1;
	present.pSwapchains = &swap_chain;
	present.pImageIndices = &current_present_image;
	present.pWaitSemaphores = &vk_present_semaphore;
	present.waitSemaphoreCount = 1;

	CHECK_RESULT(m_swap_chain->queuePresentKHR(m_swap_chain->get_present_queue(), &present));
	vkDestroySemaphore((*m_device), vk_present_semaphore, nullptr);

	m_frame->flip(m_context);
}
