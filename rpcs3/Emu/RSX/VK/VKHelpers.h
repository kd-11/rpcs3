#pragma once

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

	VKAPI_ATTR void *VKAPI_CALL mem_realloc(void *pUserData, void *pOriginal, size_t size, size_t alignment, VkSystemAllocationScope allocationScope);
	VKAPI_ATTR void *VKAPI_CALL mem_alloc(void *pUserData, size_t size, size_t alignment, VkSystemAllocationScope allocationScope);
	VKAPI_ATTR void VKAPI_CALL mem_free(void *pUserData, void *pMemory);

	VkAllocationCallbacks default_callbacks();

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
		vk::device *gpu;
		VkDevice dev;

	public:

		render_device()
		{
			dev = nullptr;
			gpu = nullptr;
		}

		render_device(vk::device &pdev, uint32_t graphics_queue_idx)
		{
			VkResult err;

			float queue_priorities[1] = { 0.f };
			gpu = &pdev;

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

			err = vkCreateDevice(*gpu, &device, nullptr, &dev);
			if (err != VK_SUCCESS) throw;
		}

		~render_device()
		{
		}

		void destroy()
		{
			if (dev && gpu)
			{
				VkAllocationCallbacks callbacks;
				callbacks.pfnAllocation = vk::mem_alloc;
				callbacks.pfnFree = vk::mem_free;
				callbacks.pfnReallocation = vk::mem_realloc;

				vkDestroyDevice(dev, &callbacks);
				dev = nullptr;
			}
		}

		operator VkDevice()
		{
			return dev;
		}
	};

	class swap_chain
	{
		vk::render_device dev;

		uint32_t m_present_queue;
		uint32_t m_graphics_queue;

		PFN_vkCreateSwapchainKHR createSwapchainKHR;
		PFN_vkDestroySwapchainKHR destroySwapchainKHR;
		PFN_vkGetSwapchainImagesKHR getSwapchainImagesKHR;
		PFN_vkAcquireNextImageKHR acquireNextImageKHR;
		PFN_vkQueuePresentKHR queuePresentKHR;

		VkQueue vk_graphics_queue;
		VkQueue vk_present_queue;

		/* WSI surface information */
		VkSurfaceKHR m_surface;
		VkFormat m_surface_format;
		VkColorSpaceKHR m_color_space;

	public:

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
			if ((VkDevice)dev)
			{
				dev.destroy();
			}
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

		vk::swap_chain& createSwapChain(HINSTANCE hInstance, HWND hWnd, vk::device &dev)
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

			return swap_chain(dev, presentQueueNodeIndex, graphicsQueueNodeIndex, format, surface, color_space);
		}
	};

	namespace glsl
	{
		class program
		{
			VkGraphicsPipelineCreateInfo pipeline;
			VkPipelineCacheCreateInfo pipelineCache;
			VkPipelineVertexInputStateCreateInfo vi;
			VkPipelineInputAssemblyStateCreateInfo ia;
			VkPipelineRasterizationStateCreateInfo rs;
			VkPipelineColorBlendStateCreateInfo cb;
			VkPipelineDepthStencilStateCreateInfo ds;
			VkPipelineViewportStateCreateInfo vp;
			VkPipelineMultisampleStateCreateInfo ms;
			VkDynamicState dynamicStateEnables[VK_DYNAMIC_STATE_RANGE_SIZE];
			VkPipelineDynamicStateCreateInfo dynamicState;

			VkShaderModule vs, fs;

		public:
			program()
			{
				memset(dynamicStateEnables, 0, sizeof dynamicStateEnables);
				memset(&dynamicState, 0, sizeof dynamicState);
				dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
				dynamicState.pDynamicStates = dynamicStateEnables;

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
				dynamicStateEnables[dynamicState.dynamicStateCount++] =
					VK_DYNAMIC_STATE_VIEWPORT;
				vp.scissorCount = 1;
				dynamicStateEnables[dynamicState.dynamicStateCount++] =
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
			}

			~program() {}

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
			}

			program& bind_vertex_program_location(const char *name, int location)
			{
			}

			void make()
			{

			}

			void use()
			{
			}
		};
	}
}