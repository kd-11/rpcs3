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
	m_thread_context.enable_debugging();

	std::vector<vk::physical_device> gpus = m_thread_context.enumerateDevices();
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
	execute_command_buffer();

	for (u32 i = 0; i < m_swap_chain->get_swap_image_count(); ++i)
	{
		VkImageView attachments[] = { m_swap_chain->get_swap_chain_image(i), m_depth_buffer };
		m_framebuffers[i].create((*m_device), m_render_pass, attachments, 2, m_frame->client_size().width, m_frame->client_size().height);
	}
}

VKGSRender::~VKGSRender()
{
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

	if (!load_program())
		return;

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
	if (!recording)
		begin_command_buffer_recording();

	init_buffers();

	LOG_ERROR(RSX, ">>> Draw call begin!");

	//TODO:
	//Begin renderPass
	//Bind pipeline (shaders)
	//Bind descriptor sets (shader data)
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
	LOG_ERROR(RSX, ">> Draw call end!");
	recording = false;
	CHECK_RESULT(vkEndCommandBuffer(m_command_buffer));
	execute_command_buffer();

	CHECK_RESULT(vkResetCommandBuffer(m_command_buffer, 0));
	begin_command_buffer_recording();

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

void VKGSRender::clear_surface(u32 mask)
{
	//TODO: Build clear commands into current renderpass descriptor set
	if (!recording) return;
	if (!(mask & 0xF3)) return;

	float depth_clear = 1.f;
	float color_clear[] = { 0.f, 0.f, 0.f, 0.f };
	u32   stencil_clear = 0.f;

	VkClearValue clear_values;
	VkImageSubresourceRange range = vk::default_image_subresource_range();

	if (mask & 0x1)
	{
		rsx::surface_depth_format surface_depth_format = rsx::to_surface_depth_format((rsx::method_registers[NV4097_SET_SURFACE_FORMAT] >> 5) & 0x7);
		u32 max_depth_value = get_max_depth_value(surface_depth_format);

		u32 clear_depth = rsx::method_registers[NV4097_SET_ZSTENCIL_CLEAR_VALUE] >> 8;
		float depth_clear = (float)clear_depth / max_depth_value;

		clear_values.depthStencil.depth = depth_clear;
		clear_values.depthStencil.stencil = stencil_clear;
	}

	if (mask & 0x2)
	{
		u8 clear_stencil = rsx::method_registers[NV4097_SET_ZSTENCIL_CLEAR_VALUE] & 0xff;
		u32 stencil_mask = rsx::method_registers[NV4097_SET_STENCIL_MASK];

		//TODO set stencil mask
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
		VkBool32 clear_red = (VkBool32)!!(mask & 0x20);
		VkBool32 clear_green = (VkBool32)!!(mask & 0x40);
		VkBool32 clear_blue = (VkBool32)!!(mask & 0x80);
		VkBool32 clear_alpha = (VkBool32)!!(mask & 0x10);

		clear_values.color.float32[0] = (float)clear_a / 255;
		clear_values.color.float32[1] = (float)clear_g / 255;
		clear_values.color.float32[2] = (float)clear_b / 255;
		clear_values.color.float32[3] = (float)clear_a / 255;

		VkImage color_image = m_swap_chain->get_swap_chain_image(m_current_present_image);
		vkCmdClearColorImage(m_command_buffer, color_image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, &clear_values.color, 1, &range);
	}

	if (mask & 0x3)
		vkCmdClearDepthStencilImage(m_command_buffer, m_depth_buffer, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, &clear_values.depthStencil, 1, &range);

	LOG_ERROR(RSX, ">>>> Clear surface finished");
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
	/* Describe a render pass and framebuffer attachments */
	VkAttachmentDescription attachments[2];
	attachments[0].format = m_swap_chain->get_surface_format();
	attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
	attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachments[0].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	attachments[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

	attachments[1].format = VK_FORMAT_D24_UNORM_S8_UINT;						/* Depth buffer format. Should be more elegant than this */
	attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
	attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
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

	if (m_submit_fence)
	{
		VkResult error = vkWaitForFences(*m_device, 1, &m_submit_fence, VK_TRUE, 1000000L);
		vkDestroyFence(*m_device, m_submit_fence, nullptr);
		m_submit_fence = nullptr;

		if (error)
			LOG_ERROR(RSX, "vkWaitForFences failed with error 0x%X", error);
	}

	CHECK_RESULT(vkBeginCommandBuffer(m_command_buffer, &begin_infos));
	recording = true;
}

void VKGSRender::execute_command_buffer()
{
	if (recording) throw;

	if (m_submit_fence)
	{
		VkResult error = vkWaitForFences((*m_device), 1, &m_submit_fence, VK_TRUE, 1000000L);
		vkDestroyFence((*m_device), m_submit_fence, nullptr);
		m_submit_fence = nullptr;

		if (error) LOG_ERROR(RSX, "vkWaitForFence failed with error 0x%X", error);
	}

	VkFenceCreateInfo fence_info;
	fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	fence_info.flags = 0;
	fence_info.pNext = nullptr;

	vkCreateFence(*m_device, &fence_info, nullptr, &m_submit_fence);

	VkPipelineStageFlags pipe_stage_flags = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
	VkCommandBuffer cmd = m_command_buffer;

	VkSubmitInfo infos;
	infos.commandBufferCount = 1;
	infos.pCommandBuffers = &cmd;
	infos.pNext = nullptr;
	infos.pSignalSemaphores = nullptr;
	infos.pWaitDstStageMask = &pipe_stage_flags;
	infos.signalSemaphoreCount = 0;
	
	if (0)
	{
		infos.waitSemaphoreCount = 1;
		infos.pWaitSemaphores = &m_present_semaphore;
	}
	else
	{
		infos.waitSemaphoreCount = 0;
		infos.pWaitSemaphores = nullptr;
	}
	
	infos.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;

	//TODO: Submit the command buffer...
	CHECK_RESULT(vkQueueSubmit(m_swap_chain->get_present_queue(), 1, &infos, m_submit_fence));
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

	execute_command_buffer();
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

	dirty_frame = true;
	m_frame->flip(m_context);
}
