#include "stdafx.h"
#include "VKHelpers.h"

namespace vk
{
	namespace glsl
	{
		program::program(vk::render_device &renderer)
		{
			init_pipeline();
			device = &renderer;
		}

		void program::init_pipeline()
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
			rs.cullMode = VK_CULL_MODE_NONE;
			rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
			rs.depthClampEnable = VK_FALSE;
			rs.rasterizerDiscardEnable = VK_FALSE;
			rs.depthBiasEnable = VK_FALSE;

			memset(&cb, 0, sizeof(cb));
			cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
			memset(att_state, 0, sizeof(att_state));
			att_state[0].colorWriteMask = 0xf;
			att_state[0].blendEnable = VK_FALSE;
			cb.attachmentCount = 1;
			cb.pAttachments = att_state;

			memset(&vp, 0, sizeof(vp));
			vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
			vp.viewportCount = 1;
			dynamic_state_descriptors[dynamic_state.dynamicStateCount++] = VK_DYNAMIC_STATE_VIEWPORT;
			vp.scissorCount = 1;
			dynamic_state_descriptors[dynamic_state.dynamicStateCount++] = VK_DYNAMIC_STATE_SCISSOR;

			memset(&ds, 0, sizeof(ds));
			ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
			ds.depthTestEnable = VK_FALSE;
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

		program& program::attach_device(vk::render_device &dev)
		{
			device = &dev;
			CHECK_RESULT(vkCreatePipelineCache(dev, &pipeline_cache_desc, NULL, &pipeline_cache));
		}

		program& program::attachFragmentProgram(VkShaderModule prog)
		{
			fs = prog;
			return *this;
		}

		program& program::attachVertexProgram(VkShaderModule prog)
		{
			vs = prog;
			return *this;
		}

		void program::make()
		{
			if (fs == nullptr || vs == nullptr)
				throw EXCEPTION("Missing shader stage!");

			shader_stages[0].module = vs;
			shader_stages[1].module = fs;

			CHECK_RESULT(vkCreatePipelineCache((*device), &pipeline_cache_desc, nullptr, &pipeline_cache));
		}

		void program::set_depth_compare_op(VkCompareOp op)
		{
			if (ds.depthCompareOp != op)
			{
				ds.depthCompareOp = op;
				dirty = true;
			}
		}

		void program::set_depth_write_mask(VkBool32 write_enable)
		{
			if (ds.depthWriteEnable != write_enable)
			{
				ds.depthWriteEnable = write_enable;
				dirty = true;
			}
		}

		void program::set_depth_test_enable(VkBool32 state)
		{
			if (ds.depthTestEnable != state)
			{
				ds.depthTestEnable = state;
				dirty = true;
			}
		}

		void program::set_primitive_topology(VkPrimitiveTopology topology)
		{
			if (ia.topology != topology)
			{
				ia.topology = topology;
				dirty = true;
			}
		}

		void program::init_descriptor_layout()
		{
			if (descriptor_layouts[0] != nullptr)
				throw EXCEPTION("Existing descriptors found!");

			if (descriptor_pool.valid())
				descriptor_pool.destroy();

			std::vector<VkDescriptorSetLayoutBinding> layout_bindings[2];
			std::vector<VkDescriptorPoolSize> sizes;

			program_input_type types[] = { input_type_uniform_buffer, input_type_texel_buffer, input_type_texture };
			program_domain stages[] = { glsl_vertex_program, glsl_fragment_program };

			VkDescriptorType vk_ids[] = { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER };
			VkShaderStageFlags vk_stages[] = { VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_FRAGMENT_BIT };

			for (auto &input : uniforms)
			{
				VkDescriptorSetLayoutBinding binding;
				binding.binding = input.location;
				binding.descriptorCount = 1;
				binding.descriptorType = vk_ids[(u32)input.type];
				binding.pImmutableSamplers = nullptr;
				binding.stageFlags = vk_stages[(u32)input.domain];

				layout_bindings[(u32)input.domain].push_back(binding);
			}

			for (int i = 0; i < 3; ++i)
			{
				u32 count = 0;
				for (auto &input : uniforms)
				{
					if (input.type == types[i])
						count++;
				}

				if (!count) continue;

				VkDescriptorPoolSize size;
				size.descriptorCount = count;
				size.type = vk_ids[i];

				sizes.push_back(size);
			}

			descriptor_pool.create((*device), sizes.data(), sizes.size());

			VkDescriptorSetLayoutCreateInfo infos;
			infos.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
			infos.pNext = nullptr;
			infos.flags = 0;
			infos.pBindings = layout_bindings[0].data();
			infos.bindingCount = layout_bindings[0].size();

			CHECK_RESULT(vkCreateDescriptorSetLayout((*device), &infos, nullptr, &descriptor_layouts[0]));

			infos.pBindings = layout_bindings[1].data();
			infos.bindingCount = layout_bindings[1].size();

			CHECK_RESULT(vkCreateDescriptorSetLayout((*device), &infos, nullptr, &descriptor_layouts[1]));

			VkPipelineLayoutCreateInfo layout_info;
			layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
			layout_info.pNext = nullptr;
			layout_info.setLayoutCount = 2;
			layout_info.pSetLayouts = descriptor_layouts;
			layout_info.flags = 0;
			layout_info.pPushConstantRanges = nullptr;
			layout_info.pushConstantRangeCount = 0;

			CHECK_RESULT(vkCreatePipelineLayout((*device), &layout_info, nullptr, &pipeline_layout));

			VkDescriptorSetAllocateInfo alloc_info;
			alloc_info.descriptorPool = descriptor_pool;
			alloc_info.descriptorSetCount = 2;
			alloc_info.pNext = nullptr;
			alloc_info.pSetLayouts = descriptor_layouts;
			alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;

			CHECK_RESULT(vkAllocateDescriptorSets((*device), &alloc_info, descriptor_sets));
		}

		void program::update_descriptors()
		{
			if (!descriptor_layouts[0])
				init_descriptor_layout();

			std::vector<VkWriteDescriptorSet> descriptor_writers;
			std::vector<VkDescriptorImageInfo> images(16);
			std::vector<VkDescriptorBufferInfo> buffers(16);
			std::vector<VkDescriptorBufferInfo> texel_buffers(16);
			std::vector<VkBufferView> texel_buffer_views(16);
			VkWriteDescriptorSet write;

			int image_index = 0;
			int buffer_index = 0;
			int texel_buffer_index = 0;

			for (auto &input : uniforms)
			{
				switch (input.type)
				{
				case input_type_texture:
				{
					auto &image = images[image_index++];
					image.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
					image.sampler = null_sampler();
					image.imageView = null_image_view();

					if (input.bound_value)
					{
						vk::texture *tex = (vk::texture *)input.bound_value;
						image.imageView = (*tex);
						image.sampler = (*tex);
					}

					memset(&write, 0, sizeof(write));
					write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
					write.pImageInfo = &image;
					write.descriptorCount = 1;

					break;
				}
				case input_type_uniform_buffer:
				{
					auto &buffer = buffers[buffer_index++];
					buffer.buffer = null_buffer();
					buffer.offset = 0;
					buffer.range = 0;

					if (input.bound_value)
					{
						vk::buffer *buf = (vk::buffer *)input.bound_value;
						buffer.buffer = (*buf);
						buffer.range = buf->size();
					}
					else
						throw EXCEPTION("UBO was not bound: %s", input.name);

					memset(&write, 0, sizeof(write));
					write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
					write.pBufferInfo = &buffer;
					write.descriptorCount = 1;
					break;
				}
				case input_type_texel_buffer:
				{
					auto &buffer_view = texel_buffer_views[texel_buffer_index];
					buffer_view = null_buffer_view();

					auto &buffer = texel_buffers[texel_buffer_index++];
					buffer.buffer = null_buffer();
					buffer.offset = 0;
					buffer.range = 0;

					if (input.bound_value)
					{
						vk::buffer *buf = (vk::buffer *)input.bound_value;
						buffer_view = (*buf);
						buffer.buffer = (*buf);
						buffer.range = buf->size();
					}
					else
						throw EXCEPTION("Texel buffer was not bound: %s", input.name);

					memset(&write, 0, sizeof(write));
					write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
					write.pTexelBufferView = &buffer_view;
					write.pBufferInfo = &buffer;
					write.descriptorCount = 1;
					break;
				}
				default:
					throw EXCEPTION("Unhandled input type!");
				}

				write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
				write.dstSet = descriptor_sets[input.domain];
				write.pNext = nullptr;

				write.dstBinding = input.location;
				descriptor_writers.push_back(write);
			}

			if (!descriptor_writers.size()) return;
			vkUpdateDescriptorSets((*device), descriptor_writers.size(), descriptor_writers.data(), 0, nullptr);
		}

		void program::destroy_descriptors()
		{
			if (pipeline_layout)
				vkDestroyPipelineLayout((*device), pipeline_layout, nullptr);

			if (descriptor_layouts[0])
				vkDestroyDescriptorSetLayout((*device), descriptor_layouts[0], nullptr);

			if (descriptor_layouts[1])
				vkDestroyDescriptorSetLayout((*device), descriptor_layouts[1], nullptr);
		}

		program& program::load_uniforms(program_domain domain, std::vector<__program_input>& inputs)
		{
			std::vector<__program_input> store = uniforms;
			uniforms.resize(0);

			for (auto &item : store)
			{
				if (item.domain != domain)
					uniforms.push_back(item);
			}

			for (auto &item : inputs)
				uniforms.push_back(item);

			return *this;
		}

		void program::use(vk::command_buffer& commands, VkRenderPass pass, u32 subpass)
		{
			update_descriptors();

			if (dirty)
			{
				if (pipeline_handle)
					vkDestroyPipeline((*device), pipeline_handle, nullptr);

				dynamic_state.pDynamicStates = dynamic_state_descriptors;
				cb.pAttachments = att_state;

				//Reconfigure this..
				pipeline.pVertexInputState = &vi;
				pipeline.pInputAssemblyState = &ia;
				pipeline.pRasterizationState = &rs;
				pipeline.pColorBlendState = &cb;
				pipeline.pMultisampleState = &ms;
				pipeline.pViewportState = &vp;
				pipeline.pDepthStencilState = &ds;
				pipeline.pStages = shader_stages;
				pipeline.pDynamicState = &dynamic_state;
				pipeline.layout = pipeline_layout;
				pipeline.basePipelineIndex = -1;
				pipeline.basePipelineHandle = VK_NULL_HANDLE;

				pipeline.renderPass = pass;

				CHECK_RESULT(vkCreateGraphicsPipelines((*device), nullptr, 1, &pipeline, NULL, &pipeline_handle));
				dirty = false;
			}

			vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_handle);
			vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 0, 2, descriptor_sets, 0, nullptr);
		}

		bool program::has_uniform(program_domain domain, std::string uniform_name)
		{
			for (auto &uniform : uniforms)
			{
				if (uniform.name == uniform_name &&
					uniform.domain == domain)
					return true;
			}

			return false;
		}

		bool program::bind_uniform(program_domain domain, std::string uniform_name, vk::texture &_texture)
		{
			for (auto &uniform : uniforms)
			{
				if (uniform.name == uniform_name &&
					uniform.domain == domain)
				{
					uniform.bound_value = &_texture;
					uniform.type = input_type_texture;

					return true;
				}
			}

			return false;
		}

		bool program::bind_uniform(program_domain domain, std::string uniform_name, vk::buffer &_buffer)
		{
			for (auto &uniform : uniforms)
			{
				if (uniform.name == uniform_name &&
					uniform.domain == domain)
				{
					uniform.bound_value = &_buffer;
					uniform.type = input_type_uniform_buffer;

					return true;
				}
			}

			return false;
		}

		bool program::bind_uniform(program_domain domain, std::string uniform_name, vk::buffer &_buffer, bool is_texel_store)
		{
			if (!is_texel_store)
			{
				return bind_uniform(domain, uniform_name, _buffer);
			}

			for (auto &uniform : uniforms)
			{
				if (uniform.name == uniform_name &&
					uniform.domain == domain)
				{
					uniform.bound_value = &_buffer;
					uniform.type = input_type_texel_buffer;

					return true;
				}
			}

			return false;
		}
	}
}