#pragma once

#include "stdafx.h"
#include <exception>
#include <string>
#include <functional>
#include <vector>
#include <memory>
#include <unordered_map>

#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "VulkanAPI.h"
#include "../GCM.h"

namespace rsx
{
	class texture;
}

namespace vk
{
#define __vkcheck
#define CHECK_RESULT(expr) if (expr != VK_SUCCESS) throw EXCEPTION("Assertion failed!")

	VKAPI_ATTR void *VKAPI_CALL mem_realloc(void *pUserData, void *pOriginal, size_t size, size_t alignment, VkSystemAllocationScope allocationScope);
	VKAPI_ATTR void *VKAPI_CALL mem_alloc(void *pUserData, size_t size, size_t alignment, VkSystemAllocationScope allocationScope);
	VKAPI_ATTR void VKAPI_CALL mem_free(void *pUserData, void *pMemory);

	VkAllocationCallbacks default_callbacks();

	class context;
	class render_device;

	vk::context *get_current_thread_ctx();
	void set_current_thread_ctx(const vk::context &ctx);

	vk::render_device *get_current_renderer();
	void set_current_renderer(const vk::render_device &device);

	class texture
	{
	public:
		texture() {}
		~texture() {}

		void create();
		void remove();

		u32  vk_wrap_mode(u32 gcm_wrap_mode);
		float max_aniso(u32 gcm_aniso);

		void init(int index, rsx::texture &tex);
	};

	class buffer
	{
	public:
		buffer() {}
	};

	class surface
	{
	public:
		surface() {}
	};

	class device
	{
		VkPhysicalDevice dev;
		VkPhysicalDeviceProperties props;
		std::vector<VkQueueFamilyProperties> queue_props;
	public:

		void set_device(VkPhysicalDevice pdev)
		{
			dev = pdev;
			vkGetPhysicalDeviceProperties(pdev, &props);
		}

		std::string name()
		{
			return props.deviceName;
		}

		uint32_t get_queue_count()
		{
			if (queue_props.size())
				return queue_props.size();

			uint32_t count = 0;
			vkGetPhysicalDeviceQueueFamilyProperties(dev, &count, nullptr);

			return count;
		}

		VkQueueFamilyProperties get_queue_properties(uint32_t queue)
		{
			if (!queue_props.size())
			{
				uint32_t count = 0;
				vkGetPhysicalDeviceQueueFamilyProperties(dev, &count, nullptr);

				queue_props.resize(count);
				vkGetPhysicalDeviceQueueFamilyProperties(dev, &count, queue_props.data());
			}

			if (queue >= queue_props.size()) throw;
			return queue_props[queue];
		}

		operator VkPhysicalDevice()
		{
			return dev;
		}
	};

	class render_device
	{
		vk::device *pgpu;
		VkDevice dev;

	public:

		render_device()
		{
			dev = nullptr;
			pgpu = nullptr;
		}

		render_device(vk::device &pdev, uint32_t graphics_queue_idx)
		{
			VkResult err;

			float queue_priorities[1] = { 0.f };
			pgpu = &pdev;

			VkDeviceQueueCreateInfo queue;
			queue.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
			queue.pNext = NULL;
			queue.queueFamilyIndex = graphics_queue_idx;
			queue.queueCount = 1;
			queue.pQueuePriorities = queue_priorities;

			//Set up instance information
			const char *requested_extensions[] =
			{
				"VK_KHR_swapchain"
			};

			VkDeviceCreateInfo device;
			device.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
			device.pNext = NULL;
			device.queueCreateInfoCount = 1;
			device.pQueueCreateInfos = &queue;
			device.enabledLayerCount = 0;
			device.ppEnabledLayerNames = nullptr;
			device.enabledExtensionCount = 1;
			device.ppEnabledExtensionNames = requested_extensions;
			device.pEnabledFeatures = nullptr;

			err = vkCreateDevice(*pgpu, &device, nullptr, &dev);
			if (err != VK_SUCCESS) throw;
		}

		~render_device()
		{
		}

		void destroy()
		{
			if (dev && pgpu)
			{
				VkAllocationCallbacks callbacks;
				callbacks.pfnAllocation = vk::mem_alloc;
				callbacks.pfnFree = vk::mem_free;
				callbacks.pfnReallocation = vk::mem_realloc;

				vkDestroyDevice(dev, &callbacks);
				dev = nullptr;
			}
		}

		const vk::device& gpu()
		{
			return *pgpu;
		}

		operator VkDevice()
		{
			return dev;
		}
	};

	class swap_chain_image
	{
		VkImageView view;
		VkImage image;

	public:
		swap_chain_image()
		{
			image = nullptr;
			view = nullptr;
		}

		void create(vk::render_device &dev, VkImage &swap_image, VkFormat format)
		{
			VkImageViewCreateInfo color_image_view;
			
			color_image_view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
			color_image_view.pNext = nullptr;
			color_image_view.format = format;

			color_image_view.components.r = VK_COMPONENT_SWIZZLE_R;
			color_image_view.components.g = VK_COMPONENT_SWIZZLE_G;
			color_image_view.components.b = VK_COMPONENT_SWIZZLE_B;
			color_image_view.components.a = VK_COMPONENT_SWIZZLE_A;

			color_image_view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			color_image_view.subresourceRange.baseMipLevel = 0;
			color_image_view.subresourceRange.levelCount = 1;
			color_image_view.subresourceRange.baseArrayLayer = 0;
			color_image_view.subresourceRange.layerCount = 1;

			color_image_view.viewType = VK_IMAGE_VIEW_TYPE_2D;
			color_image_view.flags = 0;

			color_image_view.image = swap_image;
			vkCreateImageView(dev, &color_image_view, nullptr, &view);
		}

		void discard(vk::render_device &dev)
		{
			vkDestroyImageView(dev, view, nullptr);
		}
	};

	class swap_chain
	{
		vk::render_device dev;

		uint32_t m_present_queue = 0xFFFF;
		uint32_t m_graphics_queue = 0xFFFF;

		VkQueue vk_graphics_queue = nullptr;
		VkQueue vk_present_queue = nullptr;

		/* WSI surface information */
		VkSurfaceKHR m_surface = nullptr;
		VkFormat m_surface_format;
		VkColorSpaceKHR m_color_space;

		VkSwapchainKHR m_vk_swapchain = nullptr;
		std::vector<vk::swap_chain_image> m_swap_images;

	public:

		PFN_vkCreateSwapchainKHR createSwapchainKHR;
		PFN_vkDestroySwapchainKHR destroySwapchainKHR;
		PFN_vkGetSwapchainImagesKHR getSwapchainImagesKHR;
		PFN_vkAcquireNextImageKHR acquireNextImageKHR;
		PFN_vkQueuePresentKHR queuePresentKHR;

		swap_chain(vk::device &gpu, uint32_t _present_queue, uint32_t _graphics_queue, VkFormat format, VkSurfaceKHR surface, VkColorSpaceKHR color_space)
		{
			dev = render_device(gpu, _graphics_queue);

			createSwapchainKHR = (PFN_vkCreateSwapchainKHR)vkGetDeviceProcAddr(dev, "vkCreateSwapchainKHR");
			destroySwapchainKHR = (PFN_vkDestroySwapchainKHR)vkGetDeviceProcAddr(dev, "vkDestroySwapchainKHR");
			getSwapchainImagesKHR = (PFN_vkGetSwapchainImagesKHR)vkGetDeviceProcAddr(dev, "vkGetSwapchainImagesKHR");
			acquireNextImageKHR = (PFN_vkAcquireNextImageKHR)vkGetDeviceProcAddr(dev, "vkAcquireNextImageKHR");
			queuePresentKHR = (PFN_vkQueuePresentKHR)vkGetDeviceProcAddr(dev, "vkQueuePresentKHR");

			vkGetDeviceQueue(dev, _graphics_queue, 0, &vk_graphics_queue);

			m_present_queue = _present_queue;
			m_graphics_queue = _graphics_queue;
			m_surface = surface;
			m_color_space = color_space;
			m_surface_format = format;
		}

		~swap_chain()
		{
		}

		void destroy()
		{
			if (VkDevice pdev = (VkDevice)dev)
			{
				if (m_vk_swapchain)
				{
					if (m_swap_images.size())
					{
						for (vk::swap_chain_image &img : m_swap_images)
							img.discard(dev);
					}
				}

				dev.destroy();
			}
		}

		void init_swapchain(u32 width, u32 height)
		{
			VkSwapchainKHR old_swapchain = m_vk_swapchain;

			uint32_t num_modes;
			vk::device& gpu = const_cast<vk::device&>(dev.gpu());
			__vkcheck vkGetPhysicalDeviceSurfacePresentModesKHR(gpu, m_surface, &num_modes, NULL);

			std::vector<VkPresentModeKHR> present_mode_descriptors(num_modes);
			__vkcheck vkGetPhysicalDeviceSurfacePresentModesKHR(gpu, m_surface, &num_modes, present_mode_descriptors.data());

			VkSurfaceCapabilitiesKHR surface_descriptors;
			__vkcheck vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu, m_surface, &surface_descriptors);

			VkExtent2D swapchainExtent;
			
			if (surface_descriptors.currentExtent.width == (uint32_t)-1)
			{
				swapchainExtent.width = width;
				swapchainExtent.height = height;
			}
			else
			{
				swapchainExtent = surface_descriptors.currentExtent;
				width = surface_descriptors.currentExtent.width;
				height = surface_descriptors.currentExtent.height;
			}

			VkPresentModeKHR swapchain_present_mode = VK_PRESENT_MODE_IMMEDIATE_KHR;
			uint32_t nb_swap_images = surface_descriptors.minImageCount + 1;

			if ((surface_descriptors.maxImageCount > 0) && (nb_swap_images > surface_descriptors.maxImageCount))
			{
				// Application must settle for fewer images than desired:
				nb_swap_images = surface_descriptors.maxImageCount;
			}

			VkSurfaceTransformFlagBitsKHR pre_transform = surface_descriptors.currentTransform;
			if (surface_descriptors.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)
				pre_transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;

			VkSwapchainCreateInfoKHR swap_info;
			swap_info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
			swap_info.pNext = nullptr;
			swap_info.surface = m_surface;
			swap_info.minImageCount = nb_swap_images;
			swap_info.imageFormat = m_surface_format;
			swap_info.imageColorSpace = m_color_space;

			swap_info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
			swap_info.preTransform = pre_transform;
			swap_info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
			swap_info.imageArrayLayers = 1;
			swap_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
			swap_info.queueFamilyIndexCount = 0;
			swap_info.pQueueFamilyIndices = nullptr;
			swap_info.presentMode = swapchain_present_mode;
			swap_info.oldSwapchain = old_swapchain;
			swap_info.clipped = true;

			swap_info.imageExtent.width = width;
			swap_info.imageExtent.height = height;

			createSwapchainKHR(dev, &swap_info, nullptr, &m_vk_swapchain);

			if (old_swapchain)
				destroySwapchainKHR(dev, old_swapchain, nullptr);

			nb_swap_images = 0;
			getSwapchainImagesKHR(dev, m_vk_swapchain, &nb_swap_images, nullptr);
			
			if (!nb_swap_images) throw;

			std::vector<VkImage> swap_images;
			swap_images.resize(nb_swap_images);
			getSwapchainImagesKHR(dev, m_vk_swapchain, &nb_swap_images, swap_images.data());

			m_swap_images.resize(nb_swap_images);
			for (u32 i = 0; i < nb_swap_images; ++i)
			{
				m_swap_images[i].create(dev, swap_images[i], m_surface_format);
			}
		}

		const vk::render_device& get_device()
		{
			return dev;
		}

		const VkQueue& get_present_queue()
		{
			return vk_graphics_queue;
		}

		operator const VkSwapchainKHR()
		{
			return m_vk_swapchain;
		}
	};

	class context
	{
	private:
		std::vector<VkInstance> m_vk_instances;
		VkInstance m_instance;

	public:

		context()
		{
			m_instance = nullptr;
		}

		~context()
		{
			if (m_instance || m_vk_instances.size())
				close();
		}

		void close()
		{
			if (!m_vk_instances.size()) return;

			VkAllocationCallbacks callbacks;
			callbacks.pfnAllocation = vk::mem_alloc;
			callbacks.pfnFree = vk::mem_free;
			callbacks.pfnReallocation = vk::mem_realloc;

			for (VkInstance &inst : m_vk_instances)
			{
				vkDestroyInstance(inst, &callbacks);
			}

			m_instance = nullptr;
			m_vk_instances.resize(0);
		}

		uint32_t createInstance(const char *app_name)
		{
			//Initialize a vulkan instance
			VkApplicationInfo app;

			app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
			app.pNext = nullptr;
			app.pApplicationName = app_name;
			app.applicationVersion = 0;
			app.pEngineName = app_name;
			app.engineVersion = 0;
			app.apiVersion = (1, 0, 0);

			//Set up instance information
			const char *requested_extensions[] =
			{
				"VK_KHR_surface",
				"VK_KHR_win32_surface",
			};

			VkInstanceCreateInfo instance_info;
			instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
			instance_info.pNext = nullptr;
			instance_info.pApplicationInfo = &app;
			instance_info.enabledLayerCount = 0;
			instance_info.ppEnabledLayerNames = nullptr;
			instance_info.enabledExtensionCount = 2;
			instance_info.ppEnabledExtensionNames = requested_extensions;

			//Set up memory allocation callbacks
			VkAllocationCallbacks callbacks;
			callbacks.pfnAllocation = vk::mem_alloc;
			callbacks.pfnFree = vk::mem_free;
			callbacks.pfnReallocation = vk::mem_realloc;

			VkInstance instance;
			VkResult error = vkCreateInstance(&instance_info, &callbacks, &instance);

			if (error != VK_SUCCESS) throw;

			m_vk_instances.push_back(instance);
			return m_vk_instances.size();
		}

		void makeCurrentInstance(uint32_t instance_id)
		{
			if (!instance_id || instance_id > m_vk_instances.size())
				throw;

			instance_id--;
			m_instance = m_vk_instances[instance_id];
		}

		VkInstance getCurrentInstance()
		{
			return m_instance;
		}

		VkInstance getInstanceById(uint32_t instance_id)
		{
			if (!instance_id || instance_id > m_vk_instances.size())
				throw;

			instance_id--;
			return m_vk_instances[instance_id];
		}

		std::vector<vk::device> enumerateDevices()
		{
			VkResult error;
			uint32_t num_gpus;

			error = vkEnumeratePhysicalDevices(m_instance, &num_gpus, nullptr);
			if (error != VK_SUCCESS) throw;

			std::vector<VkPhysicalDevice> pdevs(num_gpus);
			error = vkEnumeratePhysicalDevices(m_instance, &num_gpus, pdevs.data());

			if (error != VK_SUCCESS) throw;

			std::vector<vk::device> devices(num_gpus);
			for (int i = 0; i < num_gpus; ++i)
			{
				devices[i].set_device(pdevs[i]);
			}

			return devices;
		}

		vk::swap_chain* createSwapChain(HINSTANCE hInstance, HWND hWnd, vk::device &dev)
		{
			VkWin32SurfaceCreateInfoKHR createInfo;
			createInfo.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
			createInfo.pNext = NULL;
			createInfo.flags = 0;
			createInfo.hinstance = hInstance;
			createInfo.hwnd = hWnd;

			VkSurfaceKHR surface;
			VkResult err = vkCreateWin32SurfaceKHR(m_instance, &createInfo, NULL, &surface);

			uint32_t device_queues = dev.get_queue_count();
			std::vector<VkBool32> supportsPresent(device_queues);

			for (int index = 0; index < device_queues; index++)
			{
				vkGetPhysicalDeviceSurfaceSupportKHR(dev, index, surface, &supportsPresent[index]);
			}

			// Search for a graphics and a present queue in the array of queue
			// families, try to find one that supports both
			uint32_t graphicsQueueNodeIndex = UINT32_MAX;
			uint32_t presentQueueNodeIndex = UINT32_MAX;

			for (int i = 0; i < device_queues; i++)
			{
				if ((dev.get_queue_properties(i).queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0)
				{
					if (graphicsQueueNodeIndex == UINT32_MAX)
						graphicsQueueNodeIndex = i;

					if (supportsPresent[i] == VK_TRUE)
					{
						graphicsQueueNodeIndex = i;
						presentQueueNodeIndex = i;

						break;
					}
				}
			}

			if (presentQueueNodeIndex == UINT32_MAX)
			{
				// If didn't find a queue that supports both graphics and present, then
				// find a separate present queue.
				for (uint32_t i = 0; i < device_queues; ++i)
				{
					if (supportsPresent[i] == VK_TRUE)
					{
						presentQueueNodeIndex = i;
						break;
					}
				}
			}

			// Generate error if could not find both a graphics and a present queue
			if (graphicsQueueNodeIndex == UINT32_MAX || presentQueueNodeIndex == UINT32_MAX)
				throw;

			if (graphicsQueueNodeIndex != presentQueueNodeIndex)
				throw;

			// Get the list of VkFormat's that are supported:
			uint32_t formatCount;
			err = vkGetPhysicalDeviceSurfaceFormatsKHR(dev, surface, &formatCount, nullptr);
			if (err != VK_SUCCESS) throw;

			std::vector<VkSurfaceFormatKHR> surfFormats(formatCount);
			err = vkGetPhysicalDeviceSurfaceFormatsKHR(dev, surface, &formatCount, surfFormats.data());
			if (err != VK_SUCCESS) throw;

			VkFormat format;
			VkColorSpaceKHR color_space;

			if (formatCount == 1 && surfFormats[0].format == VK_FORMAT_UNDEFINED)
			{
				format = VK_FORMAT_B8G8R8A8_UNORM;
			}
			else
			{
				if (!formatCount) throw;
				format = surfFormats[0].format;
			}

			color_space = surfFormats[0].colorSpace;

			return new swap_chain(dev, presentQueueNodeIndex, graphicsQueueNodeIndex, format, surface, color_space);
		}
	};

	namespace glsl
	{
		class program
		{
			VkGraphicsPipelineCreateInfo pipeline;
			VkPipelineCacheCreateInfo pipeline_cache_desc;
			VkPipelineCache pipeline_cache;
			VkPipelineVertexInputStateCreateInfo vi;
			VkPipelineInputAssemblyStateCreateInfo ia;
			VkPipelineRasterizationStateCreateInfo rs;
			VkPipelineColorBlendStateCreateInfo cb;
			VkPipelineDepthStencilStateCreateInfo ds;
			VkPipelineViewportStateCreateInfo vp;
			VkPipelineMultisampleStateCreateInfo ms;
			VkDynamicState dynamic_state_descriptors[VK_DYNAMIC_STATE_RANGE_SIZE];
			VkPipelineDynamicStateCreateInfo dynamic_state;

			VkPipelineShaderStageCreateInfo shader_stages[2];
			VkRenderPass render_pass = nullptr;
			VkShaderModule vs, fs;
			VkPipeline pipeline_handle = nullptr;

			vk::render_device *device = nullptr;
			bool dirty;

		public:
			program()
			{
				memset(dynamic_state_descriptors, 0, sizeof dynamic_state_descriptors);
				memset(&dynamic_state, 0, sizeof dynamic_state);
				dynamic_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
				dynamic_state.pDynamicStates = dynamic_state_descriptors;

				memset(&pipeline, 0, sizeof(pipeline));
				pipeline.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
				pipeline.layout = nullptr;

				memset(&vi, 0, sizeof(vi));
				vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

				memset(&ia, 0, sizeof(ia));
				ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
				ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

				memset(&rs, 0, sizeof(rs));
				rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
				rs.polygonMode = VK_POLYGON_MODE_FILL;
				rs.cullMode = VK_CULL_MODE_BACK_BIT;
				rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
				rs.depthClampEnable = VK_FALSE;
				rs.rasterizerDiscardEnable = VK_FALSE;
				rs.depthBiasEnable = VK_FALSE;

				memset(&cb, 0, sizeof(cb));
				cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
				VkPipelineColorBlendAttachmentState att_state[1];
				memset(att_state, 0, sizeof(att_state));
				att_state[0].colorWriteMask = 0xf;
				att_state[0].blendEnable = VK_FALSE;
				cb.attachmentCount = 1;
				cb.pAttachments = att_state;

				memset(&vp, 0, sizeof(vp));
				vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
				vp.viewportCount = 1;
				dynamic_state_descriptors[dynamic_state.dynamicStateCount++] =
					VK_DYNAMIC_STATE_VIEWPORT;
				vp.scissorCount = 1;
				dynamic_state_descriptors[dynamic_state.dynamicStateCount++] =
					VK_DYNAMIC_STATE_SCISSOR;

				memset(&ds, 0, sizeof(ds));
				ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
				ds.depthTestEnable = VK_TRUE;
				ds.depthWriteEnable = VK_TRUE;
				ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
				ds.depthBoundsTestEnable = VK_FALSE;
				ds.back.failOp = VK_STENCIL_OP_KEEP;
				ds.back.passOp = VK_STENCIL_OP_KEEP;
				ds.back.compareOp = VK_COMPARE_OP_ALWAYS;
				ds.stencilTestEnable = VK_FALSE;
				ds.front = ds.back;

				memset(&ms, 0, sizeof(ms));
				ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
				ms.pSampleMask = NULL;
				ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

				fs = nullptr;
				vs = nullptr;
				dirty = true;

				pipeline.stageCount = 2;
				memset(&shader_stages, 0, 2 * sizeof(VkPipelineShaderStageCreateInfo));

				shader_stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
				shader_stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
				shader_stages[0].module = nullptr;
				shader_stages[0].pName = "main";

				shader_stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
				shader_stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
				shader_stages[1].module = nullptr;
				shader_stages[1].pName = "main";

				memset(&pipeline_cache_desc, 0, sizeof(pipeline_cache_desc));
				pipeline_cache_desc.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
			}

			program(vk::render_device &renderer)
			{
				memset(dynamic_state_descriptors, 0, sizeof dynamic_state_descriptors);
				memset(&dynamic_state, 0, sizeof dynamic_state);
				dynamic_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
				dynamic_state.pDynamicStates = dynamic_state_descriptors;

				memset(&pipeline, 0, sizeof(pipeline));
				pipeline.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
				pipeline.layout = nullptr;

				memset(&vi, 0, sizeof(vi));
				vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

				memset(&ia, 0, sizeof(ia));
				ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
				ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

				memset(&rs, 0, sizeof(rs));
				rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
				rs.polygonMode = VK_POLYGON_MODE_FILL;
				rs.cullMode = VK_CULL_MODE_BACK_BIT;
				rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
				rs.depthClampEnable = VK_FALSE;
				rs.rasterizerDiscardEnable = VK_FALSE;
				rs.depthBiasEnable = VK_FALSE;

				memset(&cb, 0, sizeof(cb));
				cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
				VkPipelineColorBlendAttachmentState att_state[1];
				memset(att_state, 0, sizeof(att_state));
				att_state[0].colorWriteMask = 0xf;
				att_state[0].blendEnable = VK_FALSE;
				cb.attachmentCount = 1;
				cb.pAttachments = att_state;

				memset(&vp, 0, sizeof(vp));
				vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
				vp.viewportCount = 1;
				dynamic_state_descriptors[dynamic_state.dynamicStateCount++] =
					VK_DYNAMIC_STATE_VIEWPORT;
				vp.scissorCount = 1;
				dynamic_state_descriptors[dynamic_state.dynamicStateCount++] =
					VK_DYNAMIC_STATE_SCISSOR;

				memset(&ds, 0, sizeof(ds));
				ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
				ds.depthTestEnable = VK_TRUE;
				ds.depthWriteEnable = VK_TRUE;
				ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
				ds.depthBoundsTestEnable = VK_FALSE;
				ds.back.failOp = VK_STENCIL_OP_KEEP;
				ds.back.passOp = VK_STENCIL_OP_KEEP;
				ds.back.compareOp = VK_COMPARE_OP_ALWAYS;
				ds.stencilTestEnable = VK_FALSE;
				ds.front = ds.back;

				memset(&ms, 0, sizeof(ms));
				ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
				ms.pSampleMask = NULL;
				ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

				fs = nullptr;
				vs = nullptr;
				dirty = true;

				pipeline.stageCount = 2;
				memset(&shader_stages, 0, 2 * sizeof(VkPipelineShaderStageCreateInfo));

				shader_stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
				shader_stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
				shader_stages[0].module = nullptr;
				shader_stages[0].pName = "main";

				shader_stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
				shader_stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
				shader_stages[1].module = nullptr;
				shader_stages[1].pName = "main";

				memset(&pipeline_cache_desc, 0, sizeof(pipeline_cache_desc));
				pipeline_cache_desc.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;

				device = &renderer;
				__vkcheck vkCreatePipelineCache(renderer, &pipeline_cache_desc, NULL, &pipeline_cache);
			}

			~program() {}

			program& attach_device(vk::render_device &dev)
			{
				device = &dev;
			}

			program& attachFragmentProgram(VkShaderModule prog)
			{
				fs = prog;
				return *this;
			}

			program& attachVertexProgram(VkShaderModule prog)
			{
				vs = prog;
				return *this;
			}

			program& bind_fragment_data_location(const char *name, int location)
			{
				return *this;
			}

			program& bind_vertex_program_location(const char *name, int location)
			{
				return *this;
			}

			void set_depth_state_enable(VkBool32 state)
			{
				if (ds.depthTestEnable != state)
				{
					ds.depthTestEnable = state;
					dirty = true;
				}
			}

			void set_depth_compare_op(VkCompareOp op)
			{
				if (ds.depthCompareOp != op)
				{
					ds.depthCompareOp = op;
					dirty = true;
				}
			}

			void set_depth_write_mask(VkBool32 write_enable)
			{
				if (ds.depthWriteEnable != write_enable)
				{
					ds.depthWriteEnable = write_enable;
					dirty = true;
				}
			}

			void make()
			{
			}

			void use()
			{
				if (dirty)
				{
					if (pipeline_handle)
						vkDestroyPipeline((*device), pipeline_handle, nullptr);

					//Reconfigure this..
					pipeline.pVertexInputState = &vi;
					pipeline.pInputAssemblyState = &ia;
					pipeline.pRasterizationState = &rs;
					pipeline.pColorBlendState = &cb;
					pipeline.pMultisampleState = &ms;
					pipeline.pViewportState = &vp;
					pipeline.pDepthStencilState = &ds;
					pipeline.pStages = shader_stages;
					pipeline.renderPass = render_pass;
					pipeline.pDynamicState = &dynamic_state;

					pipeline.renderPass = render_pass;

					__vkcheck vkCreateGraphicsPipelines((*device), pipeline_cache, 1, &pipeline, NULL, &pipeline_handle);
				}
			}
		};
	}
}