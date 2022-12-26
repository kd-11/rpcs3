#pragma once

#include "../VulkanAPI.h"
#include "chip_class.h"
#include "pipeline_binding_table.h"
#include "memory.h"

#include <string>
#include <vector>
#include <unordered_map>

#define DESCRIPTOR_MAX_DRAW_CALLS 16384

namespace vk
{
	struct gpu_formats_support
	{
		bool d24_unorm_s8 : 1;
		bool d32_sfloat_s8 : 1;
		bool bgra8_linear : 1;
		bool argb8_linear : 1;
	};

	struct gpu_shader_types_support
	{
		bool allow_float64 : 1;
		bool allow_float16 : 1;
		bool allow_int8 : 1;
	};

	struct memory_type_mapping
	{
		memory_type_info host_visible_coherent;
		memory_type_info device_local;
		memory_type_info device_bar;

		u64 device_local_total_bytes;
		u64 host_visible_total_bytes;
		u64 device_bar_total_bytes;

		DECLARE_VKAPI_FUNC(vkGetMemoryHostPointerPropertiesEXT);
	};

	class physical_device
	{
		VkInstance parent = VK_NULL_HANDLE;
		VkPhysicalDevice dev = VK_NULL_HANDLE;
		VkPhysicalDeviceProperties props;
		VkPhysicalDeviceFeatures features;
		VkPhysicalDeviceMemoryProperties memory_properties;
		std::vector<VkQueueFamilyProperties> queue_props;

		mutable std::unordered_map<VkFormat, VkFormatProperties> format_properties;
		gpu_shader_types_support shader_types_support{};
		VkPhysicalDeviceDriverPropertiesKHR driver_properties{};

		bool stencil_export_support : 1 = false;
		bool conditional_render_support : 1 = false;
		bool external_memory_host_support : 1 = false;
		bool unrestricted_depth_range_support : 1 = false;
		bool surface_capabilities_2_support : 1 = false;
		bool debug_utils_support : 1 = false;
		bool sampler_mirror_clamped_support : 1 = false;
		bool descriptor_indexing_support : 1 = false;
		bool framebuffer_loops_support : 1 = false;
		bool barycoords_support : 1 = false;
		bool dynamic_rendering_support: 1 = false;

		u32 descriptor_max_draw_calls = DESCRIPTOR_MAX_DRAW_CALLS;
		u64 descriptor_update_after_bind_mask = 0;

		friend class render_device;
	private:
		void get_physical_device_features(bool allow_extensions);
		void get_physical_device_properties(bool allow_extensions);

	public:

		physical_device() = default;
		~physical_device() = default;

		void create(VkInstance context, VkPhysicalDevice pdev, bool allow_extensions);

		std::string get_name() const;

		driver_vendor get_driver_vendor() const;
		std::string get_driver_version() const;
		chip_class get_chip_class() const;

		u32 get_queue_count() const;

		// Device properties. These structs can be large so use with care.
		const VkQueueFamilyProperties& get_queue_properties(u32 queue);
		const VkPhysicalDeviceMemoryProperties& get_memory_properties() const;
		const VkPhysicalDeviceLimits& get_limits() const;

		operator VkPhysicalDevice() const;
		operator VkInstance() const;
	};

	class render_device
	{
		physical_device* pgpu = nullptr;
		memory_type_mapping memory_map{};
		gpu_formats_support m_formats_support{};
		pipeline_binding_table m_pipeline_binding_table{};
		std::unique_ptr<mem_allocator_base> m_allocator;
		VkDevice dev = VK_NULL_HANDLE;

		VkQueue m_graphics_queue = VK_NULL_HANDLE;
		VkQueue m_present_queue = VK_NULL_HANDLE;
		VkQueue m_transfer_queue = VK_NULL_HANDLE;

		u32 m_graphics_queue_family = 0;
		u32 m_present_queue_family = 0;
		u32 m_transfer_queue_family = 0;

	public:
		// Exported device endpoints
		struct
		{
			DECLARE_VKAPI_FUNC(vkCmdBeginConditionalRenderingEXT);
			DECLARE_VKAPI_FUNC(vkCmdEndConditionalRenderingEXT);
			DECLARE_VKAPI_FUNC(vkSetDebugUtilsObjectNameEXT);
			DECLARE_VKAPI_FUNC(vkQueueInsertDebugUtilsLabelEXT);
			DECLARE_VKAPI_FUNC(vkCmdInsertDebugUtilsLabelEXT);
		}
		ext;

		struct
		{
			DECLARE_VKAPI_FUNC(vkCmdBeginRenderingKHR);
			DECLARE_VKAPI_FUNC(vkCmdEndRenderingKHR);
		}
		vk1_3;

	public:
		render_device() = default;
		~render_device() = default;

		void create(vk::physical_device& pdev, u32 graphics_queue_idx, u32 present_queue_idx, u32 transfer_queue_idx);
		void destroy();

		const VkFormatProperties get_format_properties(VkFormat format) const;

		bool get_compatible_memory_type(u32 typeBits, u32 desired_mask, u32* type_index) const;
		void rebalance_memory_type_usage();

		const physical_device& gpu() const { return *pgpu; }
		const memory_type_mapping& get_memory_mapping() const { return memory_map; }
		const gpu_formats_support& get_formats_support() const { return m_formats_support; }
		const pipeline_binding_table& get_pipeline_binding_table() const { return m_pipeline_binding_table; }
		const gpu_shader_types_support& get_shader_types_support() const { return pgpu->shader_types_support; }

		bool get_shader_stencil_export_support() const { return pgpu->stencil_export_support; }
		bool get_depth_bounds_support() const { return pgpu->features.depthBounds != VK_FALSE; }
		bool get_alpha_to_one_support() const { return pgpu->features.alphaToOne != VK_FALSE; }
		bool get_anisotropic_filtering_support() const { return pgpu->features.samplerAnisotropy != VK_FALSE; }
		bool get_wide_lines_support() const { return pgpu->features.wideLines != VK_FALSE; }
		bool get_conditional_render_support() const { return pgpu->conditional_render_support; }
		bool get_unrestricted_depth_range_support() const { return pgpu->unrestricted_depth_range_support; }
		bool get_external_memory_host_support() const { return pgpu->external_memory_host_support; }
		bool get_surface_capabilities_2_support() const { return pgpu->surface_capabilities_2_support; }
		bool get_debug_utils_support() const { return g_cfg.video.renderdoc_compatiblity && pgpu->debug_utils_support; }
		bool get_descriptor_indexing_support() const { return pgpu->descriptor_indexing_support; }
		bool get_framebuffer_loops_support() const { return pgpu->framebuffer_loops_support; }
		bool get_barycoords_support() const { return pgpu->barycoords_support; }
		bool get_dynamic_rendering_support() const { return pgpu->dynamic_rendering_support; }

		u64 get_descriptor_update_after_bind_support() const { return pgpu->descriptor_update_after_bind_mask; }
		u32 get_descriptor_max_draw_calls() const { return pgpu->descriptor_max_draw_calls; }

		VkQueue get_present_queue() const { return m_present_queue; }
		VkQueue get_graphics_queue() const { return m_graphics_queue; }
		VkQueue get_transfer_queue() const { return m_transfer_queue; }
		u32 get_graphics_queue_family() const { return m_graphics_queue_family; }
		u32 get_present_queue_family() const { return m_graphics_queue_family; }
		u32 get_transfer_queue_family() const { return m_transfer_queue_family; }

		mem_allocator_base* get_allocator() const { return m_allocator.get(); }

		operator VkDevice() const { return dev; }
	};

	memory_type_mapping get_memory_mapping(const physical_device& dev);
	gpu_formats_support get_optimal_tiling_supported_formats(const physical_device& dev);
	pipeline_binding_table get_pipeline_binding_table(const physical_device& dev);

	extern const render_device* g_render_device;
}
