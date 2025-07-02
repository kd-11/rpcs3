#pragma once

#include "../VulkanAPI.h"
#include "device.h"
#include "memory.h"

namespace vk
{
	struct buffer
	{
		VkBuffer value;
		VkDeviceAddress va = 0;
		VkBufferCreateInfo info = {};
		std::unique_ptr<vk::memory_block> memory;

		buffer(const vk::render_device& dev,
			  u64 size,
			  const memory_type_info& memory_type,
			  u32 access_flags,
			  VkBufferUsageFlags usage,
			  VkBufferCreateFlags flags,
			  vmm_allocation_pool allocation_pool);

		buffer(const vk::render_device& dev,
			   VkBufferUsageFlags usage,
			   void* host_pointer,
			   u64 size);

		~buffer();

		void* map(u64 offset, u64 size);
		void unmap();
		u32 size() const;

		buffer(const buffer&) = delete;
		buffer(buffer&&) = delete;

	private:
		VkDevice m_device;

		void init_va();
	};

	struct buffer_view
	{
		VkBufferView value;
		VkDeviceAddress va = 0;
		VkBufferViewCreateInfo info = {};

		buffer_view(const vk::render_device& dev,
					const vk::buffer& buffer,
					VkFormat format,
					VkDeviceSize offset,
					VkDeviceSize size);

		~buffer_view();

		buffer_view(const buffer_view&) = delete;
		buffer_view(buffer_view&&)      = delete;

		bool in_range(u32 address, u32 size, u32& offset) const;

	private:
		VkDevice m_device;
	};

	struct buffer_reference
	{
		VkDeviceAddress va = 0;
		VkBuffer value = VK_NULL_HANDLE;
		VkDeviceSize offset = 0;
		VkDeviceSize range = 0;

		buffer_reference() = default;

		buffer_reference(const vk::buffer* buf, VkDeviceSize offset, VkDeviceSize range)
			: va(buf->va + offset)
			, value(buf->value)
			, offset(offset)
			, range(range == VK_WHOLE_SIZE ? buf->size() : range)
		{};

		buffer_reference(const vk::buffer& buf, VkDeviceSize offset, VkDeviceSize range)
			: buffer_reference(&buf, offset, range)
		{};

		buffer_reference(const std::unique_ptr<vk::buffer>& buf, VkDeviceSize offset, VkDeviceSize range)
			: buffer_reference(buf.get(), offset, range)
		{};
	};
}
