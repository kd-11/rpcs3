#pragma once
#include "Emu/RSX/GSRender.h"
#include "VKHelpers.h"
#include "VKTexture.h"

#define RSX_DEBUG 1

#include "VKProgramBuffer.h"

#pragma comment(lib, "vulkan-1.lib")

class VKGSRender : public GSRender
{
private:
	VKFragmentProgram m_fragment_prog;
	VKVertexProgram m_vertex_prog;

	vk::texture m_textures[rsx::limits::textures_count];
	vk::texture m_vertex_textures[rsx::limits::vertex_textures_count];

	vk::glsl::program *m_program;
	vk::context m_thread_context;

	rsx::surface_info m_surface;

	struct texture_buffer_pair
	{
		vk::texture *texture;
		vk::buffer *buffer;
	}
	m_gl_attrib_buffers[rsx::limits::vertex_count];

public:
	//vk::fbo draw_fbo;

private:
	VKProgramBuffer m_prog_buffer;

	vk::texture m_draw_tex_color[rsx::limits::color_buffers_count];
	vk::texture m_draw_tex_depth_stencil;

	vk::render_device *m_device;
	vk::swap_chain* m_swap_chain;
	vk::texture m_depth_buffer;
	//buffer

	vk::buffer m_scale_offset_buffer;
	vk::buffer m_vertex_constants_buffer;
	vk::buffer m_fragment_constants_buffer;

	vk::buffer m_index_buffer;

	//Vulkan internals
	u32 m_current_present_image = 0xFFFF;
	VkSemaphore m_present_semaphore = nullptr;
	VkFence m_submit_fence = nullptr;

	vk::command_pool m_command_buffer_pool;
	vk::command_buffer m_command_buffer;
	bool recording = false;
	bool dirty_frame = true;

	//Single render pass
	VkRenderPass m_render_pass;

	vk::framebuffer m_framebuffers[4];

public:
	VKGSRender();
	~VKGSRender();

private:
	static u32 enable(u32 enable, u32 cap);
	static u32 enable(u32 enable, u32 cap, u32 index);
	void clear_surface(u32 mask);
	void init_render_pass();
	void destroy_render_pass();
	void execute_command_buffer();
	void begin_command_buffer_recording();

public:
	bool load_program();
	void init_buffers(bool skip_reading = false);
	void read_buffers();
	void write_buffers();
	void set_viewport();

protected:
	void begin() override;
	void end() override;

	void on_init_thread() override;
	void on_exit() override;
	bool do_method(u32 id, u32 arg) override;
	void flip(int buffer) override;
};
