#pragma once

#include "VulkanAPI.h"
#include "Emu/RSX/Program/GLSLTypes.h"

#include "vkutils/descriptors.h"
#include "vkutils/shader_program.h"

#include <string>
#include <vector>
#include <variant>

namespace vk
{
	namespace glsl
	{
		enum program_input_type : u32
		{
			input_type_uniform_buffer = 0,
			input_type_texel_buffer,
			input_type_texture,
			input_type_storage_buffer,
			input_type_storage_texture,
			input_type_push_constant,

			input_type_max_enum
		};

		struct bound_sampler
		{
			VkFormat format = VK_FORMAT_UNDEFINED;
			VkImage image = VK_NULL_HANDLE;
			VkComponentMapping mapping{};
		};

		struct bound_buffer
		{
			VkFormat format = VK_FORMAT_UNDEFINED;
			VkBuffer buffer = nullptr;
			u64 offset = 0;
			u64 size = 0;
		};

		struct push_constant_ref
		{
			u32 offset = 0;
			u32 size = 0;
		};

		struct program_input
		{
			::glsl::program_domain domain;
			program_input_type type;

			using bound_data_t = std::variant<bound_buffer, bound_sampler, push_constant_ref>;
			bound_data_t bound_data;

			u32 set = 0;
			u32 location = umax;
			std::string name;

			inline bound_buffer& as_buffer() { return *std::get_if<bound_buffer>(&bound_data); }
			inline bound_sampler& as_sampler() { return *std::get_if<bound_sampler>(&bound_data); }
			inline push_constant_ref& as_push_constant() { return *std::get_if<push_constant_ref>(&bound_data); }

			inline const bound_buffer& as_buffer() const { return *std::get_if<bound_buffer>(&bound_data); }
			inline const bound_sampler& as_sampler() const { return *std::get_if<bound_sampler>(&bound_data); }
			inline const push_constant_ref& as_push_constant() const { return *std::get_if<push_constant_ref>(&bound_data); }

			static program_input make(
				::glsl::program_domain domain,
				const std::string& name,
				program_input_type type,
				u32 set,
				u32 location,
				const bound_data_t& data = bound_buffer{})
			{
				return program_input
				{
					.domain = domain,
					.type = type,
					.bound_data = data,
					.set = set,
					.location = location,
					.name = name
				};
			}
		};

		class shader : public vk::shader
		{
		public:
			shader() = default;
			~shader() = default;

			void create(::glsl::program_domain domain, const std::string& source);
			VkShaderModule compile();

			const std::string& get_source() const { return m_source; }

		protected:
			::glsl::program_domain type = ::glsl::program_domain::glsl_vertex_program;
			std::string m_source;
		};

		struct descriptor_array_ref_t
		{
			u32 first = 0;
			u32 count = 0;
		};

		struct descriptor_buffer_view_t
		{
			VkBufferView view;
			VkDeviceAddress va;
			VkDeviceSize length;
			VkFormat format;

			static inline
			descriptor_buffer_view_t make(const vk::buffer_view& buffer_view)
			{
				return descriptor_buffer_view_t {
					.view = buffer_view.value,
					.va = buffer_view.va,
					.length = buffer_view.info.range,
					.format = buffer_view.info.format
				};
			}
		};

		using descriptor_slot_t = std::variant<
			VkDescriptorImageInfo,
			VkDescriptorBufferInfo,
			descriptor_buffer_view_t,
			descriptor_array_ref_t>;

		class descriptor_table_t
		{
		public:
			using inputs_array_t = std::array<std::vector<program_input>, input_type_max_enum>;

			descriptor_table_t() = default;
			virtual ~descriptor_table_t() = default;

			void init(const vk::render_device& dev);
			void validate() const;
			virtual void destroy();

			void create_descriptor_set_layout();

			virtual VkDescriptorSet commit(const vk::command_buffer& cmd) = 0;

			template <typename T>
			inline void notify_descriptor_slot_updated(u32 slot, const T& data)
			{
				m_descriptors_dirty[slot] = true;
				m_descriptor_slots[slot] = data;
				m_any_descriptors_dirty = true;
			}

			template <typename T>
			std::pair<descriptor_array_ref_t, T*> allocate_scratch(u32 count)
			{
				// Only implemented for images
				descriptor_array_ref_t ref{};
				ref.first = m_scratch_images_array.size();
				ref.count = count;

				m_scratch_images_array.resize(ref.first + ref.count);
				return { ref, m_scratch_images_array.data() + ref.first };
			}

			operator bool() const { return !!m_device; }

			inline inputs_array_t& inputs() { return m_inputs; }
			inline const inputs_array_t& inputs() const { return m_inputs; }

			inline std::vector<program_input>&
			inputs(program_input_type type) { return m_inputs[type]; }

			inline const std::vector<program_input>&
			inputs(program_input_type type) const { return m_inputs[type]; }

			inline descriptor_slot_t&
			bind_slots(int index) { return m_descriptor_slots[index]; }

			inline const descriptor_slot_t&
			bind_slots(int index) const { return m_descriptor_slots[index]; }

			inline VkDescriptorSetLayout layout() const { return m_descriptor_set_layout; }

		protected:
			const vk::render_device* m_device = nullptr;
			inputs_array_t m_inputs;
			VkDescriptorSetLayout m_descriptor_set_layout = VK_NULL_HANDLE;
			rsx::simple_array<VkDescriptorPoolSize> m_descriptor_pool_sizes;
			rsx::simple_array<VkDescriptorType> m_descriptor_types;

			std::vector<descriptor_slot_t> m_descriptor_slots;
			std::vector<bool> m_descriptors_dirty;
			bool m_any_descriptors_dirty = false;

			rsx::simple_array< VkDescriptorImageInfo> m_scratch_images_array;
		};

		class descriptor_table_legacy_t : public descriptor_table_t
		{
		public:
			VkDescriptorSet commit(const vk::command_buffer& cmd) override;
			void destroy() override;

		protected:
			std::unique_ptr<vk::descriptor_pool> m_descriptor_pool;
			vk::descriptor_set m_descriptor_set{};

			void create_descriptor_pool();
			VkDescriptorSet allocate_descriptor_set();
		};

		class descriptor_table_buffer_t : public descriptor_table_t
		{
		public:
			VkDescriptorSet commit(const vk::command_buffer& cmd) override;
			void destroy() override;

		protected:
			std::unique_ptr<vk::buffer> m_bo;
			rsx::simple_array<u32> m_descriptor_offsets;

			void initialize_bo();
		};

		enum binding_set_index : u32
		{
			// For separate shader objects
			binding_set_index_vertex = 0,
			binding_set_index_fragment = 1,

			// Aliases
			binding_set_index_compute = 0,
			binding_set_index_unified = 0,

			// Meta
			binding_set_index_max_enum = 2,
		};

		class program
		{
		public:
			program(const vk::render_device& dev, const VkGraphicsPipelineCreateInfo& create_info, const std::vector<program_input> &vertex_inputs, const std::vector<program_input>& fragment_inputs);
			program(const vk::render_device& dev, const VkComputePipelineCreateInfo& create_info, const std::vector<program_input>& compute_inputs);
			program(const program&) = delete;
			program(program&& other) = delete;
			~program();

			program& link(bool separate_stages);
			program& bind(const vk::command_buffer& cmd, VkPipelineBindPoint bind_point);

			bool has_uniform(program_input_type type, const std::string &uniform_name);
			std::pair<u32, u32> get_uniform_location(::glsl::program_domain domain, program_input_type type, const std::string& uniform_name);

			void bind_uniform(const VkDescriptorImageInfo &image_descriptor, u32 set_id, u32 binding_point);
			void bind_uniform(const VkDescriptorBufferInfo &buffer_descriptor, u32 set_id, u32 binding_point);
			void bind_uniform(const vk::buffer_view& buffer_view, u32 set_id, u32 binding_point);

			void bind_uniform_array(const VkDescriptorImageInfo* image_descriptors, int count, u32 set_id, u32 binding_point);

			inline VkPipelineLayout layout() const { return m_pipeline_layout; }
			inline VkPipeline handle() const { return ensure(m_pipeline)->handle(); }

		protected:
			const vk::render_device &m_device;
			VkPipelineLayout m_pipeline_layout = VK_NULL_HANDLE;
			std::unique_ptr<vk::pipeline> m_pipeline;
			std::variant<VkGraphicsPipelineCreateInfo, VkComputePipelineCreateInfo> m_info;

			std::array<std::unique_ptr<descriptor_table_t>, binding_set_index_max_enum> m_sets;
			bool m_linked = false;
			bool m_use_descriptor_buffers = false;

			void init();
			void create_pipeline_layout();

			program& load_uniforms(const std::vector<program_input>& inputs);
		};
	}
}
