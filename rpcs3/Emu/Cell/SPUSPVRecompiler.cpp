#include "stdafx.h"
#include "SPUSPVRecompiler.h"

#include "Emu/IdManager.h"
#include "Emu/system_config.h"
#include "Crypto/sha1.h"

#include "Emu/RSX/rsx_utils.h"

#include "Emu/RSX/VK/vkutils/buffer_object.h"
#include "Emu/RSX/VK/vkutils/descriptors.h"
#include "Emu/RSX/VK/vkutils/memory.h"
#include "Emu/RSX/VK/VKProgramPipeline.h"
#include "Emu/RSX/VK/VKPipelineCompiler.h"
//#include "Emu/RSX/VK/VKGSRenderTypes.hpp"
#include "Emu/RSX/VK/VKGSRender.h"

#include <windows.h>
#pragma optimize("", off)

constexpr auto SPV_MAX_BLOCKS = 65536u;

template <>
void fmt_class_string<spv::exit_code>::format(std::string& out, u64 arg)
{
	format_enum(out, arg, [](spv::exit_code value)
	{
		switch (value)
		{
		case spv::exit_code::SUCCESS: return "SPU_SUCCESS";
		case spv::exit_code::HLT: return "SPU_HLT";
		case spv::exit_code::MFC_CMD: return "SPU_MFC_CMD";
		case spv::exit_code::RDCH3: return "SPU_RDCH_SigNotify1";
		default: break;
		}

		return unknown;
	});
}

namespace spv
{
	VkDescriptorSetLayout g_set_layout = VK_NULL_HANDLE;
	VkPipelineLayout g_pipeline_layout = VK_NULL_HANDLE;

	std::mutex g_init_mutex;

	static bool s_ctx_initialized = false;
	static u32 s_used_descriptors = 0;

	struct SPUSPV_block
	{
		std::unique_ptr<vk::glsl::shader> compute;
		std::unique_ptr<vk::glsl::program> prog;

		// PC
		int start_pc = -1;
		int end_pc = -1;

		// Flags
		bool is_dynamic_branch_block = false;
		bool issues_memory_op = false;

		SPUSPV_block(const std::string& block_src)
		{
			compute = std::make_unique<vk::glsl::shader>();
			compute->create(::glsl::glsl_compute_program, block_src);
			const auto handle = compute->compile();

			VkPipelineShaderStageCreateInfo shader_stage{};
			shader_stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
			shader_stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
			shader_stage.module = handle;
			shader_stage.pName = "main";

			VkComputePipelineCreateInfo info{};
			info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
			info.stage = shader_stage;
			info.layout = g_pipeline_layout;
			info.basePipelineIndex = -1;
			info.basePipelineHandle = VK_NULL_HANDLE;

			auto compiler = vk::get_pipe_compiler();
			prog = compiler->compile(info, g_pipeline_layout, vk::pipe_compiler::COMPILE_INLINE);
		}

		~SPUSPV_block()
		{
			prog.reset();
			compute->destroy();
		}
	};

#pragma pack(push, 1)
	struct TEB
	{
		u32 next_pc;   // Next PC address
		u32 exit_code; // Thread flags
		u32 lr;        // Link register (debug)
		u32 sp;        // Stack pointer (debug)
		u32 dr[16];     // General debug registers
		u32 fpscr[4];  // Unused
	};

	struct MFC_registers_t
	{
		int MFC_tag_mask;
		int MFC_tag_stat_count;
		int MFC_tag_stat_value;
		int MFC_tag_update;
		int MFC_tag_id;
		int MFC_lsa;
		int MFC_eal;
		int MFC_eah;
		int MFC_size;
		int MFC_cmd;
		int MFC_fence;
	};

	struct SPU_GPR
	{
		int vgpr[128][4];
	};
#pragma pack(pop)

	// Context for the logical SPU
	struct SPU_execution_context_t
	{
		std::unique_ptr<vk::buffer> gpr_block;     // 144 GPRs (128 base + 16 spill/temp)
		std::unique_ptr<vk::buffer> ls_block;      // LSA SRAM
		std::unique_ptr<vk::buffer> teb_block;     // Next PC, interrupts, etc
		std::unique_ptr<vk::buffer> gpu_constants; // Builtin constants lookup table

		bool flush_caches = true;
		bool init_constants = true;

		TEB *teb = nullptr;
		u8* ls = nullptr;
		SPU_GPR* gpr = nullptr;

		vk::descriptor_pool descriptor_pool;
		vk::command_pool command_pool;

		vk::command_buffer_chain<32> command_buffer_list;
		vk::descriptor_set descriptor_set;

		SPU_execution_context_t(const vk::render_device& dev)
		{
			// Descriptor pool + set
			std::vector<VkDescriptorPoolSize> pool_sizes =
			{
				{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4 }
			};
			descriptor_pool.create(dev, pool_sizes.data(), ::size32(pool_sizes), 1, 1);
			descriptor_set = descriptor_pool.allocate(g_set_layout, VK_FALSE, 0);

			// Command pool + execution ring buffer
			command_pool.create(dev, dev.get_transfer_queue_family());
			command_buffer_list.create(command_pool, vk::command_buffer::all);

			// Create our memory buffers
#if 0
			gpr_block = std::make_unique<vk::buffer>(dev, 256 * 16, dev.get_memory_mapping().device_local, 0, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, 0, vk::VMM_ALLOCATION_POOL_SYSTEM);
#else
			gpr_block = std::make_unique<vk::buffer>(dev, 256 * 16, dev.get_memory_mapping().device_bar,
				VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
				VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, 0, vk::VMM_ALLOCATION_POOL_SYSTEM);
			gpr = static_cast<SPU_GPR*>(gpr_block->map(0, 128 * 16));
#endif

			gpu_constants = std::make_unique<vk::buffer>(dev, 8192, dev.get_memory_mapping().device_local, 0, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, 0, vk::VMM_ALLOCATION_POOL_SYSTEM);

			ls_block = std::make_unique<vk::buffer>(dev, 256 * 1024, dev.get_memory_mapping().device_bar,
				VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
				VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 0, vk::VMM_ALLOCATION_POOL_SYSTEM);
			teb_block = std::make_unique<vk::buffer>(dev, 256, dev.get_memory_mapping().device_bar,
				VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
				VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 0, vk::VMM_ALLOCATION_POOL_SYSTEM);

			// Bind our standard interface. Only needs to be done once.
			std::vector<VkDescriptorBufferInfo> bindings;
			bindings.push_back({ .buffer = ls_block->value, .offset = 0, .range = ls_block->size() });
			bindings.push_back({ .buffer = gpr_block->value, .offset = 0, .range = gpr_block->size() });
			bindings.push_back({ .buffer = teb_block->value, .offset = 0, .range = teb_block->size() });
			bindings.push_back({ .buffer = gpu_constants->value, .offset = 0, .range = gpu_constants->size() });
			for (u32 i = 0; i < ::size32(bindings); ++i)
			{
				descriptor_set.push(bindings[i], VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, i);
			}
			descriptor_set.flush();

			// Map execution memory
			teb = static_cast<TEB*>(teb_block->map(0, sizeof(TEB)));
			ls = static_cast<u8*>(ls_block->map(0, ls_block->size()));
		}

		~SPU_execution_context_t()
		{
			sync();

			command_buffer_list.destroy();
			command_pool.destroy();
			descriptor_pool.destroy();

			if (teb)
			{
				teb_block->unmap();
				teb = nullptr;
			}

			if (ls)
			{
				ls_block->unmap();
				ls = nullptr;
			}
		}

		void execute(spu_thread& spu, const SPUSPV_block& block)
		{
			// TODO: CB itself can be pre-recorded and just submitted each time
			auto prep_ssbo = [](const vk::command_buffer& cmd, const vk::buffer& buf)
			{
				vk::insert_buffer_memory_barrier(cmd, buf.value, 0, buf.size(),
					VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
					VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
					VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
			};

			if (spu.pc == 0x6e8)
			{
				int a = 0;
				a++;
			}

			auto cmd = command_buffer_list.next();
			cmd->begin();

			if (flush_caches)
			{
				flush_caches = false;
				do_cache_sync(*cmd, spu);
			}

			if (init_constants)
			{
				init_constants = false;
				do_init_constants(*cmd);
				do_init_regs(*cmd, spu);
			}

			// Preamble
			// Execution ordering barriers
			prep_ssbo(*cmd, *gpr_block);
			prep_ssbo(*cmd, *ls_block);
			prep_ssbo(*cmd, *teb_block);
			
			// Dispatch
			descriptor_set.bind(*cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_pipeline_layout);
			vkCmdBindPipeline(*cmd, VK_PIPELINE_BIND_POINT_COMPUTE, block.prog->pipeline);
			vkCmdDispatch(*cmd, 1, 1, 1);

			cmd->end();
			cmd->eid_tag = umax;

			// Submit
			auto pdev = vk::g_render_device;
			vk::queue_submit_t submit_info = { pdev->get_transfer_queue(), nullptr };
			submit_info.skip_descriptor_sync = true;
			cmd->submit(submit_info, VK_TRUE);

			if (!block.is_dynamic_branch_block && !block.issues_memory_op)
			{
				sync();
				spu.pc = static_cast<u32>(block.end_pc);
				return;
			}

			// Wait for GPU
			sync();

			// Check TEB flags
			const auto exit_code = static_cast<spv::exit_code>(teb->exit_code);
			switch (exit_code)
			{
			case spv::exit_code::HLT:
			{
				// HALT
				// TODO: Unify with spu_thread::halt
				if (spu.get_type() >= spu_type::raw)
				{
					spu.state += cpu_flag::stop + cpu_flag::wait;
					spu.status_npc.atomic_op([&spu](auto& state)
					{
						state.status |= SPU_STATUS_STOPPED_BY_HALT;
						state.status &= ~SPU_STATUS_RUNNING;
						state.npc = spu.pc | +spu.interrupts_enabled;
					});

					spu.status_npc.notify_one();
					spu.int_ctrl[2].set(SPU_INT2_STAT_SPU_HALT_OR_STEP_INT);
				}

				return;
			}
			case spv::exit_code::MFC_CMD:
			{
				// TODO: Be smarter about this
				std::memcpy(spu._ptr<u8>(0), ls, 256 * 1024);

				auto regs = reinterpret_cast<MFC_registers_t*>(teb->dr);
				spu.ch_tag_mask = regs->MFC_tag_mask;
				spu.ch_tag_stat.set_value(regs->MFC_tag_stat_value, regs->MFC_tag_stat_count); // value, count
				spu.ch_tag_upd = regs->MFC_tag_update;
				spu.ch_mfc_cmd.tag = regs->MFC_tag_id;
				spu.ch_mfc_cmd.lsa = regs->MFC_lsa;
				spu.ch_mfc_cmd.eal = regs->MFC_eal;
				spu.ch_mfc_cmd.eah = regs->MFC_eah;
				spu.ch_mfc_cmd.size = regs->MFC_size;
				spu.ch_mfc_cmd.cmd = MFC(regs->MFC_cmd);
				spu.mfc_fence = regs->MFC_fence;

				spu.process_mfc_cmd();

				flush_caches = true;

				if (!block.is_dynamic_branch_block)
				{
					spu.pc = static_cast<u32>(block.end_pc);
					return;
				}

				break;
			}
			case spv::exit_code::RDCH3:
			{
				// TODO: This fall is correct as we're waiting, but channels are poorly supported in general
				const spu_opcode_t op = { *reinterpret_cast<const be_t<u32>*>(ls + spu.pc) };
				const s64 result = spu.get_ch_value(op.ra);
				gpr->vgpr[op.rt][3] = result;
				spu.pc = teb->next_pc + 4;
				return;
			}
			}

			// Next
			spu.pc = teb->next_pc;
		}

		void sync()
		{
			command_buffer_list.wait_all();
		}

		void do_cache_sync(const vk::command_buffer& cmd, spu_thread& spu)
		{
			std::memcpy(ls, spu._ptr<u8>(0), 256 * 1024);

			// Update MFC registers
			MFC_registers_t reg_upd;
			reg_upd.MFC_tag_mask = spu.ch_tag_mask;
			reg_upd.MFC_tag_stat_count = spu.ch_tag_stat.get_count();
			reg_upd.MFC_tag_stat_value = spu.ch_tag_stat.get_value();
			reg_upd.MFC_tag_update = spu.ch_tag_upd;
			reg_upd.MFC_tag_id = spu.ch_mfc_cmd.tag;
			reg_upd.MFC_lsa = spu.ch_mfc_cmd.lsa;
			reg_upd.MFC_eal = spu.ch_mfc_cmd.eal;
			reg_upd.MFC_eah = spu.ch_mfc_cmd.eah;
			reg_upd.MFC_size = spu.ch_mfc_cmd.size;
			reg_upd.MFC_cmd = spu.ch_mfc_cmd.cmd;
			reg_upd.MFC_fence = spu.mfc_fence;

			vk::insert_buffer_memory_barrier(cmd, gpr_block->value, 144 * 16, 48,
				VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
				VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
				VK_ACCESS_TRANSFER_WRITE_BIT);

			vkCmdUpdateBuffer(cmd, gpr_block->value, 144 * 16, sizeof(MFC_registers_t), &reg_upd);

			vk::insert_buffer_memory_barrier(cmd, gpr_block->value, 144 * 16, 48,
				VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
				VK_ACCESS_TRANSFER_WRITE_BIT,
				VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
		}

		void do_init_constants(const vk::command_buffer& cmd)
		{
			static std::mutex sanity_lock;
			std::lock_guard lock(sanity_lock);

			// Init constants. Can also be a texture.
			int qsh_lookup[256][4];
			// Set to all 1s
			for (int row = 0; row < 256; ++row)
			{
				for (int col = 0; col < 4; ++col)
				{
					qsh_lookup[row][col] = -1;
				}
			}

			// Create the qshl masks
			auto flip_bit = [](int* v, int bit)
			{
				int word = bit / 32;
				int bit_offset = bit % 32;

				if (word > 3)
				{
					// NOP
					return;
				}

				v[word] ^= (1 << bit_offset);
			};

			for (int row = 0; row < 128; ++row)
			{
				for (int bit = 0; bit < row; ++bit)
				{
					flip_bit(qsh_lookup[row], bit);
				}
			}

			// Create the qshr masks
			for (int row = 0; row < 128; ++row)
			{
				for (int bit = 0; bit < row; ++bit)
				{
					flip_bit(qsh_lookup[row + 128], 127 - bit);
				}
			}

			vkCmdUpdateBuffer(cmd, gpu_constants->value, 0, 256 * 16, qsh_lookup);

			vk::insert_buffer_memory_barrier(cmd, gpu_constants->value, 0, 256 * 16,
				VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
				VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
		}

		void do_init_regs(const vk::command_buffer& cmd, const spu_thread& spu)
		{
			u32 regs[128][4];
			for (int i = 0; i < 128; ++i)
			{
				regs[i][0] = spu.gpr[i]._u32[0];
				regs[i][1] = spu.gpr[i]._u32[1];
				regs[i][2] = spu.gpr[i]._u32[2];
				regs[i][3] = spu.gpr[i]._u32[3];
			}

			vkCmdUpdateBuffer(cmd, gpr_block->value, 0, 128 * 16, regs);

			vk::insert_buffer_memory_barrier(cmd, gpr_block->value, 0, gpr_block->size(),
				VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
				VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
		}
	};

	std::unordered_map<u32, std::unique_ptr<SPU_execution_context_t>> g_SPU_context;
	std::unordered_map<u64, std::unique_ptr<SPUSPV_block>> g_compiled_blocks;
	shared_mutex g_compiled_blocks_mutex;

	void init_context(const vk::render_device& dev)
	{
		std::lock_guard lock(g_init_mutex);
		if (s_ctx_initialized)
		{
			return;
		}

		// Create the shared layout
		std::vector<VkDescriptorSetLayoutBinding> bindings =
		{
			{
				.binding = 0,
				.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
				.descriptorCount = 1,
				.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
				.pImmutableSamplers = nullptr
			},
			{
				.binding = 1,
				.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
				.descriptorCount = 1,
				.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
				.pImmutableSamplers = nullptr
			},
			{
				.binding = 2,
				.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
				.descriptorCount = 1,
				.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
				.pImmutableSamplers = nullptr
			},
						{
				.binding = 3,
				.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
				.descriptorCount = 1,
				.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
				.pImmutableSamplers = nullptr
			},
		};

		VkDescriptorSetLayoutCreateInfo create_layout =
		{
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
			.pNext = nullptr,
			.flags = 0,
			.bindingCount = ::size32(bindings),
			.pBindings = bindings.data()
		};

		vkCreateDescriptorSetLayout(dev, &create_layout, nullptr, &g_set_layout);

		VkPipelineLayoutCreateInfo layout_info = {};
		layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
		layout_info.setLayoutCount = 1;
		layout_info.pSetLayouts = &g_set_layout;
		vkCreatePipelineLayout(dev, &layout_info, nullptr, &g_pipeline_layout);

		s_ctx_initialized = true;
	}

	void destroy_context(const vk::render_device& dev)
	{
		std::lock_guard lock(g_init_mutex);
		if (!s_ctx_initialized)
		{
			return;
		}

		vkDestroyPipelineLayout(dev, g_pipeline_layout, nullptr);
		vkDestroyDescriptorSetLayout(dev, g_set_layout, nullptr);
		g_SPU_context.clear();
		g_compiled_blocks.clear();

		s_ctx_initialized = false;
	}

	void spv_entry(spu_thread& spu, void* ls, u8* /*rip*/)
	{
		auto func = spu.jit->get_runtime().find(static_cast<u32*>(ls), spu.pc);
		if (!func)
		{
			func = spu.jit->compile(spu.jit->analyse(spu._ptr<u32>(0), spu.pc));
		}

		ensure(func);
		const auto hash = reinterpret_cast<u64>(func);

		spv::SPUSPV_block* block = nullptr;
		{
			reader_lock lock(spv::g_compiled_blocks_mutex);
			block = spv::g_compiled_blocks[hash].get();
		}
		ensure(block);

		auto& executor = spv::g_SPU_context[spu.id];
		if (!executor)
		{
			auto pdev = vk::g_render_device;
			executor = std::make_unique<spv::SPU_execution_context_t>(*pdev);
		}

		executor->execute(spu, *block);
	}
}

namespace
{
	const spu_decoder<spv_recompiler> s_spu_decoder;

	const auto println = [](const char* s)
	{
		char buf[512];
		snprintf(buf, 512, "[spvc] %s\n", s);
		rsx_log.error(buf);
		OutputDebugStringA(buf);
	};


}

std::unique_ptr<spu_recompiler_base> spu_recompiler_base::make_spv_recompiler()
{
	return std::make_unique<spv_recompiler>();
}

spv_recompiler::spv_recompiler()
{
}

void spv_recompiler::init()
{
	// Initialize if necessary
	if (!m_spurt)
	{
		m_spurt = &g_fxo->get<spu_runtime>();
	}

	auto pdev = vk::g_render_device;
	spv::init_context(*pdev);
}

spu_function_t spv_recompiler::compile(spu_program&& _func)
{
	const u32 start0 = _func.entry_point;

	const auto add_loc = m_spurt->add_empty(std::move(_func));

	if (!add_loc)
	{
		return nullptr;
	}

	if (add_loc->compiled)
	{
		return add_loc->compiled;
	}

	const spu_program& func = add_loc->data;

	if (func.entry_point != start0)
	{
		// Wait for the duplicate
		while (!add_loc->compiled)
		{
			add_loc->compiled.wait(nullptr);
		}

		return add_loc->compiled;
	}

	{
		sha1_context ctx;
		u8 output[20];

		sha1_starts(&ctx);
		sha1_update(&ctx, reinterpret_cast<const u8*>(func.data.data()), func.data.size() * 4);
		sha1_finish(&ctx, output);

		be_t<u64> hash_start;
		std::memcpy(&hash_start, output, sizeof(hash_start));
		m_hash_start = hash_start;
	}

	m_pos = func.lower_bound;
	u32 m_base = func.entry_point;
	m_size = ::size32(func.data) * 4;
	const u32 start = m_pos;
	const u32 end = start + m_size;

	// Clear
	c.reset();

	// Create block labels

	// Get bit mask of valid code words for a given range (up to 128 bytes)
	auto get_code_mask = [&](u32 starta, u32 enda) -> u32
	{
		u32 result = 0;

		for (u32 addr = starta, m = 1; addr < enda && m; addr += 4, m <<= 1)
		{
			// Filter out if out of range, or is a hole
			if (addr >= start && addr < end && func.data[(addr - start) / 4])
			{
				result |= m;
			}
		}

		return result;
	};

	// Check code
	u32 starta = start;

	// Skip holes at the beginning (giga only)
	for (u32 j = start; j < end; j += 4)
	{
		if (!func.data[(j - start) / 4])
		{
			starta += 4;
		}
		else
		{
			break;
		}
	}

	// TODO
	// Clear registers, etc

	if (m_pos != start)
	{
		// Jump to the entry point if necessary
	}

	for (u32 i = 0; i < func.data.size(); i++)
	{
		const u32 pos = start + i * 4;
		const u32 op = std::bit_cast<be_t<u32>>(func.data[i]);

		if (!op)
		{
			// Ignore hole
			if (m_pos + 1)
			{
				spu_log.error("Unexpected fallthrough to 0x%x", pos);
				// TODO
				// branch_fixed(spu_branch_target(pos));
				m_pos = -1;
			}

			continue;
		}

		// Update position
		m_pos = pos;

		// Execute recompiler function
		(this->*s_spu_decoder.decode(op))({ op });

		if (m_pos == umax)
		{
			// Early exit
			break;
		}
	}

	// Make fallthrough if necessary
	if (m_pos + 1)
	{
		// TODO
		// branch_fixed(spu_branch_target(end));
	}

	// Epilogue

	// Build instruction dispatch table

	// Compile and get function address
	auto compiled = c.compile();
	ensure(compiled->prog);
	{
		std::lock_guard lock(spv::g_compiled_blocks_mutex);
		if (spv::g_compiled_blocks.find(m_hash_start) == spv::g_compiled_blocks.end())
		{
			spv::g_compiled_blocks[m_hash_start] = std::move(compiled);
		}
	}
	spu_function_t fn = reinterpret_cast<spu_function_t>(m_hash_start);

	// Install compiled function pointer
	const bool added = !add_loc->compiled && add_loc->compiled.compare_and_swap_test(nullptr, fn);
	if (added)
	{
		add_loc->compiled.notify_all();
	}

	return fn;
}

void spv_recompiler::UNK(spu_opcode_t op)
{
	c.unimplemented("UNK");
}

void spv_recompiler::STOP(spu_opcode_t op)
{
	c.unimplemented("STOP");
}

void spv_recompiler::LNOP(spu_opcode_t op)
{
	// NOP
}

void spv_recompiler::SYNC(spu_opcode_t op)
{
	c.unimplemented("SYNC");
}

void spv_recompiler::DSYNC(spu_opcode_t op)
{
	c.unimplemented("DSYNC");
}

void spv_recompiler::MFSPR(spu_opcode_t op)
{
	c.unimplemented("MFSPR");
}

void spv_recompiler::RDCH(spu_opcode_t op)
{
	switch (op.ra)
	{
	case SPU_RdSigNotify1:
	{
		// TODO
		c.s_movsi(c.s_tmp0, spv_constant::make_si(0));
		c.s_brz(spv_constant::make_su(m_pos + 4), c.s_tmp0, spv::exit_code::RDCH3);
		return;
	}
	case MFC_RdTagStat:
	{
		c.s_loadsr(c.s_tmp0, c.mfc.ch_tag_stat_count);
		c.s_brz(spv_constant::make_su(m_pos), c.s_tmp0, spv::exit_code::HLT); // TODO
		c.s_loadsr(c.s_tmp1, c.mfc.ch_tag_stat_value);
		c.s_ins(op.rt, c.s_tmp1, 3);
		return;
	}
	}

	fmt::throw_exception("Unimplemented RDCH 0x%x", op.ra);
}

void spv_recompiler::RCHCNT(spu_opcode_t op)
{
	c.unimplemented("RCHCNT");
}

void spv_recompiler::SF(spu_opcode_t op)
{
	c.v_subs(op.rt, op.rb, op.ra);
}

void spv_recompiler::OR(spu_opcode_t op)
{
	c.v_or(op.rt, op.ra, op.rb);
}

void spv_recompiler::BG(spu_opcode_t op)
{
	c.unimplemented("BG");
}

void spv_recompiler::SFH(spu_opcode_t op)
{
	c.unimplemented("SFH");
}

void spv_recompiler::NOR(spu_opcode_t op)
{
	c.v_or(c.v_tmp0, op.ra, op.rb);
	c.v_xori(op.rt, c.v_tmp0, spv_constant::spread(0xffffffff).as_vi());
}

void spv_recompiler::ABSDB(spu_opcode_t op)
{
	c.unimplemented("ABSDB");
}

void spv_recompiler::ROT(spu_opcode_t op)
{
	c.unimplemented("ROT");
}

void spv_recompiler::ROTM(spu_opcode_t op)
{
	c.unimplemented("ROTM");
}

void spv_recompiler::ROTMA(spu_opcode_t op)
{
	c.unimplemented("ROTMA");
}

void spv_recompiler::SHL(spu_opcode_t op)
{
	const auto mask = spv_constant::spread(0x3f);
	c.v_andi(c.v_tmp0, op.rb, mask);
	c.v_shl(op.rt, op.ra, c.v_tmp0);
}

void spv_recompiler::ROTH(spu_opcode_t op)
{
	c.unimplemented("ROTH");
}

void spv_recompiler::ROTHM(spu_opcode_t op)
{
	c.unimplemented("ROTHM");
}

void spv_recompiler::ROTMAH(spu_opcode_t op)
{
	c.unimplemented("ROTMAH");
}

void spv_recompiler::SHLH(spu_opcode_t op)
{
	c.unimplemented("SHLH");
}

void spv_recompiler::ROTI(spu_opcode_t op)
{
	c.unimplemented("ROTI");
}

void spv_recompiler::ROTMI(spu_opcode_t op)
{
	c.v_shri(op.rt, op.ra, spv_constant::spread((0 - op.i7) & 0x3f).as_vi());
}

void spv_recompiler::ROTMAI(spu_opcode_t op)
{
	c.unimplemented("ROTMAI");
}

void spv_recompiler::SHLI(spu_opcode_t op)
{
	const auto distance = spv_constant::spread(static_cast<s32>(op.i7 & 0x3f));
	c.v_shli(op.rt, op.ra, distance);
}

void spv_recompiler::ROTHI(spu_opcode_t op)
{
	c.unimplemented("ROTHI");
}

void spv_recompiler::ROTHMI(spu_opcode_t op)
{
	c.unimplemented("ROTHMI");
}

void spv_recompiler::ROTMAHI(spu_opcode_t op)
{
	c.unimplemented("ROTMAHI");
}

void spv_recompiler::SHLHI(spu_opcode_t op)
{
	c.unimplemented("SHLHI");
}

void spv_recompiler::A(spu_opcode_t op)
{
	c.v_adds(op.rt, op.ra, op.rb);
}

void spv_recompiler::AND(spu_opcode_t op)
{
	c.v_or(op.rt, op.ra, op.rb);
}

void spv_recompiler::CG(spu_opcode_t op)
{
	c.v_xori(c.v_tmp0, op.ra, spv_constant::spread(0x7fffffff).as_vi());
	c.v_xori(c.v_tmp1, op.rb, spv_constant::spread(0x80000000).as_vi());
	c.v_cmpgts(op.rt, c.v_tmp1, c.v_tmp0);
}

void spv_recompiler::AH(spu_opcode_t op)
{
	c.unimplemented("AH");
}

void spv_recompiler::NAND(spu_opcode_t op)
{
	c.unimplemented("NAND");
}

void spv_recompiler::AVGB(spu_opcode_t op)
{
	c.unimplemented("AVGB");
}

void spv_recompiler::MTSPR(spu_opcode_t op)
{
	c.unimplemented("MTSPR");
}

void spv_recompiler::WRCH(spu_opcode_t op)
{
	switch (op.ra)
	{
	case SPU_WrSRR0:
	{
		c.s_xtr(c.s_tmp0, op.rt, 3);
		c.s_andi(c.s_tmp0, c.s_tmp0, spv_constant::make_si(0x3fffc));
		c.s_storsr(c.s_srr0, c.s_tmp0);
		return;
	}

	case SPU_WrOutIntrMbox:
	{
		// TODO:
		break;
	}

	case SPU_WrOutMbox:
	{
		// TODO:
		break;
	}

	case MFC_WrTagMask:
	{
		c.s_xtr(c.s_tmp1, op.rt, 3);
		c.s_storsr(c.mfc.ch_tag_mask, c.s_tmp1);
		c.s_call("MFC_write_tag_mask");
		return;
	}

	case MFC_WrTagUpdate:
	{
		c.s_xtr(c.s_tmp1, op.rt, 3);
		c.s_call("MFC_write_tag_update", { "sgpr[1]" });
		return;
	}

	case MFC_LSA:
	{
		c.s_xtr(c.s_tmp1, op.rt, 3);
		c.s_storsr(c.mfc.cmd_lsa, c.s_tmp1);
		return;
	}

	case MFC_EAH:
	{
		c.s_xtr(c.s_tmp1, op.rt, 3);
		c.s_storsr(c.mfc.cmd_eah, c.s_tmp1);
		return;
	}

	case MFC_EAL:
	{
		c.s_xtr(c.s_tmp1, op.rt, 3);
		c.s_storsr(c.mfc.cmd_eal, c.s_tmp1);
		return;
	}

	case MFC_Size:
	{
		c.s_xtr(c.s_tmp1, op.rt, 3);
		c.s_storsr(c.mfc.cmd_size, c.s_tmp1);
		return;
	}

	case MFC_TagID:
	{
		c.s_xtr(c.s_tmp1, op.rt, 3);
		c.s_andi(c.s_tmp1, c.s_tmp1, spv_constant::make_si(0x1f));
		c.s_storsr(c.mfc.cmd_tag_id, c.s_tmp1);
		return;
	}

	case MFC_Cmd:
	{
		c.s_xtr(c.s_tmp1, op.rt, 3);
		c.s_andi(c.s_tmp1, c.s_tmp1, spv_constant::make_si(0xff));
		c.s_storsr(c.mfc.cmd, c.s_tmp1);
		c.s_call("MFC_cmd");
		// TODO: Handle on GPU side
		c.s_bri(spv_constant::make_si(m_pos + 4));
		m_pos = -1;
		return;
	}

	case MFC_WrListStallAck:
	{
		// TODO
		break;
	}

	case SPU_WrDec:
	{
		// TODO
		break;
	}

	case SPU_WrEventMask:
	{
		// TODO
		break;
	}

	case SPU_WrEventAck:
	{
		// TODO
		break;
	}

	case SPU_Set_Bkmk_Tag:
	case SPU_PM_Start_Ev:
	case SPU_PM_Stop_Ev:
	{
		// TODO
		break;
	}
	}

	c.unimplemented(fmt::format("WRCH(%d)", op.ra));
}

void spv_recompiler::BIZ(spu_opcode_t op)
{
	c.unimplemented("BIZ");
}

void spv_recompiler::BINZ(spu_opcode_t op)
{
	c.unimplemented("BINZ");
}

void spv_recompiler::BIHZ(spu_opcode_t op)
{
	c.unimplemented("BIHZ");
}

void spv_recompiler::BIHNZ(spu_opcode_t op)
{
	c.unimplemented("BIHNZ");
}

void spv_recompiler::STOPD(spu_opcode_t op)
{
	c.unimplemented("STOPD");
}

void spv_recompiler::STQX(spu_opcode_t op)
{
	c.s_xtr(c.s_tmp0, op.ra, 3);
	c.s_xtr(c.s_tmp1, op.rb, 3);
	c.s_adds(c.s_tmp0, c.s_tmp0, c.s_tmp1);
	c.s_andi(c.s_tmp0, c.s_tmp0, spv_constant::make_si(0x3fff0));
	c.v_storq(c.s_tmp0, op.rt);
}

void spv_recompiler::BI(spu_opcode_t op)
{
	c.s_xtr(c.s_tmp0, op.ra, 3);
	c.s_br(c.s_tmp0);

	// TODO: Interrupt status
	m_pos = -1;
}

void spv_recompiler::BISL(spu_opcode_t op)
{
	c.v_movsi(op.rt, spv_constant::make_rvi(spu_branch_target(m_pos + 4)));
	c.s_xtr(c.s_tmp0, op.ra, 3);
	c.s_br(c.s_tmp0);

	// TODO: Interrupt status
	m_pos = -1;
}

void spv_recompiler::IRET(spu_opcode_t op)
{
	c.unimplemented("IRET");
}

void spv_recompiler::BISLED(spu_opcode_t op)
{
	c.unimplemented("BISLED");
}

void spv_recompiler::HBR(spu_opcode_t op)
{
	// NOP
}

void spv_recompiler::GB(spu_opcode_t op)
{
	c.v_shli(c.v_tmp0, op.ra, spv_constant::make_vi(24, 25, 26, 27));
	c.v_ori(c.v_tmp0, c.v_tmp0, spv_constant::make_vi(1 << 24, 1 << 25, 1 << 26, 1 << 27));
	c.s_hzor(c.s_tmp0, c.v_tmp0); // Horizontal sum = horizontal or
	c.v_movsi(op.rt, spv_constant::spread(0));
	c.s_ins(op.rt, c.s_tmp0, 3);
}

void spv_recompiler::GBH(spu_opcode_t op)
{
	c.unimplemented("GBH");
}

void spv_recompiler::GBB(spu_opcode_t op)
{
	c.unimplemented("GBB");
}

void spv_recompiler::FSM(spu_opcode_t op)
{
	c.unimplemented("FSM");
}

void spv_recompiler::FSMH(spu_opcode_t op)
{
	c.unimplemented("FSMH");
}

void spv_recompiler::FSMB(spu_opcode_t op)
{
	c.unimplemented("FSMB");
}

void spv_recompiler::FREST(spu_opcode_t op)
{
	c.v_rcpf(op.rt, op.ra);
}

void spv_recompiler::FRSQEST(spu_opcode_t op)
{
	c.v_andi(c.v_tmp0, op.ra, spv_constant::spread(0x7fffffff).as_vi());
	c.v_rsqf(op.rt, c.v_tmp0);
}

void spv_recompiler::LQX(spu_opcode_t op)
{
	c.s_xtr(c.s_tmp0, op.ra, 3);
	c.s_xtr(c.s_tmp1, op.rb, 3);
	c.s_adds(c.s_tmp0, c.s_tmp0, c.s_tmp1);
	c.s_andi(c.s_tmp0, c.s_tmp0, spv_constant::make_si(0x3fff0));
	c.v_loadq(op.rt, c.s_tmp0);
}

void spv_recompiler::ROTQBYBI(spu_opcode_t op)
{
	c.unimplemented("ROTQBYBI");
}

void spv_recompiler::ROTQMBYBI(spu_opcode_t op)
{
	c.unimplemented("ROTQMBYBI");
}

void spv_recompiler::SHLQBYBI(spu_opcode_t op)
{
	c.unimplemented("SHLQBYBI");
}

void spv_recompiler::CBX(spu_opcode_t op)
{
	c.unimplemented("CBX");
}

void spv_recompiler::CHX(spu_opcode_t op)
{
	c.unimplemented("CHX");
}

void spv_recompiler::CWX(spu_opcode_t op)
{
	c.unimplemented("CWX");
}

void spv_recompiler::CDX(spu_opcode_t op)
{
	c.unimplemented("CDX");
}

void spv_recompiler::ROTQBI(spu_opcode_t op)
{
	c.unimplemented("ROTQBI");
}

void spv_recompiler::ROTQMBI(spu_opcode_t op)
{
	c.unimplemented("ROTQMBI");
}

void spv_recompiler::SHLQBI(spu_opcode_t op)
{
	c.unimplemented("SHLQBI");
}

void spv_recompiler::ROTQBY(spu_opcode_t op)
{
	c.s_xtr(c.s_tmp0, op.rb, 3);
	c.s_andi(c.s_tmp0, c.s_tmp0, spv_constant::make_si(15));
	c.s_shli(c.s_tmp0, c.s_tmp0, spv_constant::make_si(3));   // Convert bytes to bits
	c.q_rotr(op.rt, op.ra, c.s_tmp0);
}

void spv_recompiler::ROTQMBY(spu_opcode_t op)
{
	c.unimplemented("ROTQMBY");
}

void spv_recompiler::SHLQBY(spu_opcode_t op)
{
	c.unimplemented("SHLQBY");
}

void spv_recompiler::ORX(spu_opcode_t op)
{
	c.unimplemented("ORX");
}

void spv_recompiler::CBD(spu_opcode_t op)
{
	c.unimplemented("CBD");
}

void spv_recompiler::CHD(spu_opcode_t op)
{
	c.unimplemented("CHD");
}

void spv_recompiler::CWD(spu_opcode_t op)
{
	const u8 sel0[16] = { 0x1F, 0x1E, 0x1D, 0x1C, 0x1B, 0x1A, 0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10 };
	const auto sel0_c = spv::vector_const_t{ sel0 }.as_vi();
	c.v_movsi(op.rt, sel0_c);
	c.s_xtr(c.s_tmp0, op.ra, 3);
	c.s_addsi(c.s_tmp0, c.s_tmp0, spv_constant::make_si(op.i7));
	c.s_xori(c.s_tmp0, c.s_tmp0, spv_constant::make_si(-1));
	c.s_andi(c.s_tmp0, c.s_tmp0, spv_constant::make_si(0xc));
	c.s_shli(c.s_tmp0, c.s_tmp0, spv_constant::make_si(2));
	c.s_ins(op.rt, spv_constant::make_si(0x00010203), c.s_tmp0);
}

void spv_recompiler::CDD(spu_opcode_t op)
{
	c.unimplemented("CDD");
}

void spv_recompiler::ROTQBII(spu_opcode_t op)
{
	c.unimplemented("ROTQBII");
}

void spv_recompiler::ROTQMBII(spu_opcode_t op)
{
	c.unimplemented("ROTQMBII");
}

void spv_recompiler::SHLQBII(spu_opcode_t op)
{
	c.unimplemented("SHLQBII");
}

void spv_recompiler::ROTQBYI(spu_opcode_t op)
{
	// TODO: Ellide mov
	const auto shift_distance = (op.i7 & 0xf) << 3;
	c.s_movsi(c.s_tmp0, spv_constant::make_si(shift_distance));
	c.q_rotr(op.rt, op.ra, c.s_tmp0);
}

void spv_recompiler::ROTQMBYI(spu_opcode_t op)
{
	// TODO: Ellide mov
	const auto shift_distance = ((0 - int(op.i7)) & 0xf) << 3;
	c.s_movsi(c.s_tmp0, spv_constant::make_si(shift_distance));
	c.q_shl(op.rt, op.ra, c.s_tmp0);
}

void spv_recompiler::SHLQBYI(spu_opcode_t op)
{
	const auto shift_distance = (op.i7 & 0xf) << 3;
	c.s_movsi(c.s_tmp0, spv_constant::make_si(shift_distance));
	c.q_shr(op.rt, op.ra, c.s_tmp0);
}

void spv_recompiler::NOP(spu_opcode_t op)
{
	// NOP
}

void spv_recompiler::CGT(spu_opcode_t op)
{
	c.unimplemented("CGT");
}

void spv_recompiler::XOR(spu_opcode_t op)
{
	c.unimplemented("XOR");
}

void spv_recompiler::CGTH(spu_opcode_t op)
{
	c.unimplemented("CGTH");
}

void spv_recompiler::EQV(spu_opcode_t op)
{
	c.unimplemented("EQV");
}

void spv_recompiler::CGTB(spu_opcode_t op)
{
	c.unimplemented("CGTB");
}

void spv_recompiler::SUMB(spu_opcode_t op)
{
	c.unimplemented("SUMB");
}

void spv_recompiler::HGT(spu_opcode_t op)
{
	c.unimplemented("HGT");
}

void spv_recompiler::CLZ(spu_opcode_t op)
{
	c.unimplemented("CLZ");
}

void spv_recompiler::XSWD(spu_opcode_t op)
{
	c.unimplemented("XSWD");
}

void spv_recompiler::XSHW(spu_opcode_t op)
{
	c.unimplemented("XSHW");
}

void spv_recompiler::CNTB(spu_opcode_t op)
{
	c.unimplemented("CNTB");
}

void spv_recompiler::XSBH(spu_opcode_t op)
{
	// s16 sext
	c.v_bfxi(c.v_tmp0, op.ra, spv_constant::make_si(7), spv_constant::make_si(1));
	c.v_mulsi(c.v_tmp0, c.v_tmp0, spv_constant::spread(0xffff0000).as_vi());
	c.v_andi(c.v_tmp1, op.ra, spv_constant::spread(0x0000ffff));
	c.v_and(op.rt, c.v_tmp0, c.v_tmp1);
}

void spv_recompiler::CLGT(spu_opcode_t op)
{
	const auto all_ones = spv_constant::spread(0xffffffff).as_vi();
	c.v_cmpgtu(c.v_tmp0, op.ra, op.rb);
	c.v_mulsi(op.rt, c.v_tmp0, all_ones);
}

void spv_recompiler::ANDC(spu_opcode_t op)
{
	c.unimplemented("ANDC");
}

void spv_recompiler::FCGT(spu_opcode_t op)
{
	c.v_cmpgtf(c.v_tmp0, op.ra, op.rb);
	c.v_mulsi(op.rt, c.v_tmp0, spv_constant::spread(0xffffffff).as_vi());
}

void spv_recompiler::DFCGT(spu_opcode_t op)
{
	c.unimplemented("DFCGT");
}

void spv_recompiler::FA(spu_opcode_t op)
{
	c.v_addf(op.rt, op.ra, op.rb);
}

void spv_recompiler::FS(spu_opcode_t op)
{
	// TODO: xfloat
	c.v_subf(op.rt, op.ra, op.rb);
}

void spv_recompiler::FM(spu_opcode_t op)
{
	// TODO - xfloat
	c.v_mulf(op.rt, op.ra, op.rb);
}

void spv_recompiler::CLGTH(spu_opcode_t op)
{
	c.unimplemented("CLGTH");
}

void spv_recompiler::ORC(spu_opcode_t op)
{
	c.unimplemented("ORC");
}

void spv_recompiler::FCMGT(spu_opcode_t op)
{
	c.unimplemented("FCMGT");
}

void spv_recompiler::DFCMGT(spu_opcode_t op)
{
	c.unimplemented("DFCMGT");
}

void spv_recompiler::DFA(spu_opcode_t op)
{
	c.unimplemented("DFA");
}

void spv_recompiler::DFS(spu_opcode_t op)
{
	c.unimplemented("DFS");
}

void spv_recompiler::DFM(spu_opcode_t op)
{
	c.unimplemented("DFM");
}

void spv_recompiler::CLGTB(spu_opcode_t op)
{
	c.unimplemented("CLGTB");
}

void spv_recompiler::HLGT(spu_opcode_t op)
{
	c.unimplemented("HLGT");
}

void spv_recompiler::DFMA(spu_opcode_t op)
{
	c.unimplemented("DFMA");
}

void spv_recompiler::DFMS(spu_opcode_t op)
{
	c.unimplemented("DFMS");
}

void spv_recompiler::DFNMS(spu_opcode_t op)
{
	c.unimplemented("DFNMS");
}

void spv_recompiler::DFNMA(spu_opcode_t op)
{
	c.unimplemented("DFNMA");
}

void spv_recompiler::CEQ(spu_opcode_t op)
{
	c.unimplemented("CEQ");
}

void spv_recompiler::MPYHHU(spu_opcode_t op)
{
	c.unimplemented("MPYHHU");
}

void spv_recompiler::ADDX(spu_opcode_t op)
{
	c.v_andi(c.v_tmp0, op.rt, spv_constant::spread(1));
	c.v_adds(c.v_tmp0, c.v_tmp0, op.ra);
	c.v_adds(op.rt, c.v_tmp0, op.rb);
}

void spv_recompiler::SFX(spu_opcode_t op)
{
	c.unimplemented("SFX");
}

void spv_recompiler::CGX(spu_opcode_t op)
{
	c.unimplemented("CGX");
}

void spv_recompiler::BGX(spu_opcode_t op)
{
	c.unimplemented("BGX");
}

void spv_recompiler::MPYHHA(spu_opcode_t op)
{
	c.unimplemented("MPYHHA");
}

void spv_recompiler::MPYHHAU(spu_opcode_t op)
{
	c.unimplemented("MPYHHAU");
}

void spv_recompiler::FSCRRD(spu_opcode_t op)
{
	c.unimplemented("FSCRRD");
}

void spv_recompiler::FESD(spu_opcode_t op)
{
	c.unimplemented("FESD");
}

void spv_recompiler::FRDS(spu_opcode_t op)
{
	c.unimplemented("FRDS");
}

void spv_recompiler::FSCRWR(spu_opcode_t op)
{
	c.unimplemented("FSCRWR");
}

void spv_recompiler::DFTSV(spu_opcode_t op)
{
	c.unimplemented("DFTSV");
}

void spv_recompiler::FCEQ(spu_opcode_t op)
{
	c.unimplemented("FCEQ");
}

void spv_recompiler::DFCEQ(spu_opcode_t op)
{
	c.unimplemented("DFCEQ");
}

void spv_recompiler::MPY(spu_opcode_t op)
{
	c.unimplemented("MPY");
}

void spv_recompiler::MPYH(spu_opcode_t op)
{
	c.v_andi(c.v_tmp0, op.rb, spv_constant::spread(0x0000ffff));
	c.v_shri(c.v_tmp1, op.ra, spv_constant::spread(16));
	c.v_muls(c.v_tmp0, c.v_tmp0, c.v_tmp1);
	c.v_shri(op.rt, c.v_tmp0, spv_constant::spread(16));
}

void spv_recompiler::MPYHH(spu_opcode_t op)
{
	c.unimplemented("MPYHH");
}

void spv_recompiler::MPYS(spu_opcode_t op)
{
	c.unimplemented("MPYS");
}

void spv_recompiler::CEQH(spu_opcode_t op)
{
	c.unimplemented("CEQH");
}

void spv_recompiler::FCMEQ(spu_opcode_t op)
{
	c.unimplemented("FCMEQ");
}

void spv_recompiler::DFCMEQ(spu_opcode_t op)
{
	c.unimplemented("DFCMEQ");
}

void spv_recompiler::MPYU(spu_opcode_t op)
{
	c.v_andi(c.v_tmp0, op.ra, spv_constant::spread(0x0000ffff));
	c.v_andi(c.v_tmp1, op.rb, spv_constant::spread(0x0000ffff));
	c.v_mulu(op.rt, c.v_tmp0, c.v_tmp1);
}

void spv_recompiler::CEQB(spu_opcode_t op)
{
	c.unimplemented("CEQB");
}

void spv_recompiler::FI(spu_opcode_t op)
{
	// TODO
	const auto mask_se = spv_constant::spread(0xff800000).as_vi(); // sign and exponent mask
	const auto mask_bf = spv_constant::spread(0xff800000).as_vi(); // base fraction mask
	const auto mask_sf = spv_constant::spread(0x000003ff).as_vi(); // step fraction mask
	const auto mask_yf = spv_constant::spread(0x0007ffff).as_vi(); // Y fraction mask (bits 13..31)

	c.v_andi(c.v_tmp0, op.rb, mask_bf);
	c.v_ori(c.v_tmp0, c.v_tmp0, spv_constant::spread(0x3f800000)); // base
	c.v_andi(c.v_tmp1, op.rb, mask_sf);
	c.v_mulfi(c.v_tmp1, c.v_tmp1, spv_constant::spread(std::exp2(-13.f))); // step
	c.v_andi(c.v_tmp2, op.ra, mask_yf);
	c.v_mulfi(c.v_tmp2, c.v_tmp2, spv_constant::spread(std::exp2(-19.f))); // y

	c.v_mulf(c.v_tmp1, c.v_tmp1, c.v_tmp2); // step * y
	c.v_subf(c.v_tmp2, c.v_tmp0, c.v_tmp1); // base - step * y
	c.v_andi(c.v_tmp2, c.v_tmp2, spv_constant::spread(~mask_se.value.i[0])); // ~mask_se & (base - step * y)
	c.v_andi(c.v_tmp1, op.rb, mask_se);     // b & se
	c.v_or(op.rt, c.v_tmp1, c.v_tmp2);      // OR((b & se), ~se & (base - step * y));
}

void spv_recompiler::HEQ(spu_opcode_t op)
{
	const auto value = spv_constant::make_si(op.si10);
	c.s_xtr(c.s_tmp0, op.ra, 3);
	c.s_xtr(c.s_tmp1, op.rb, 3);
	c.s_heq(c.s_tmp0, c.s_tmp1);
}

void spv_recompiler::CFLTS(spu_opcode_t op)
{
	c.unimplemented("CFLTS");
}

void spv_recompiler::CFLTU(spu_opcode_t op)
{
	const auto scale = g_spu_imm.scale[173 - op.i8]._f[0];
	c.v_mulfi(c.v_tmp0, op.ra, spv_constant::spread(scale));
	c.v_clampfi(c.v_tmp0, c.v_tmp0, spv_constant::spread(0.f), spv_constant::spread(4294967295.f));
	c.v_fcvtu(op.rt, c.v_tmp0);
}

void spv_recompiler::CSFLT(spu_opcode_t op)
{
	const auto scale = g_spu_imm.scale[op.i8 - 155]._f[0];
	c.v_scvtf(c.v_tmp0, op.ra);
	c.v_mulfi(op.rt, c.v_tmp0, spv_constant::spread(scale));
}

void spv_recompiler::CUFLT(spu_opcode_t op)
{
	const auto scale = g_spu_imm.scale[op.i8 - 155]._f[0];
	c.v_ucvtf(c.v_tmp0, op.ra);
	c.v_mulfi(op.rt, c.v_tmp0, spv_constant::spread(scale));
}

void spv_recompiler::BRZ(spu_opcode_t op)
{
	const u32 target = spu_branch_target(m_pos, op.i16);
	if (target == m_pos + 4)
	{
		return;
	}

	c.s_xtr(c.s_tmp0, op.rt, 3);
	c.s_brz(spv_constant::make_su(target), c.s_tmp0);
}

void spv_recompiler::STQA(spu_opcode_t op)
{
	c.unimplemented("STQA");
}

void spv_recompiler::BRNZ(spu_opcode_t op)
{
	const u32 target = spu_branch_target(m_pos, op.i16);
	if (target == m_pos + 4)
	{
		return;
	}

	c.s_xtr(c.s_tmp0, op.rt, 3);
	c.s_brnz(spv_constant::make_su(target), c.s_tmp0);
}

void spv_recompiler::BRHZ(spu_opcode_t op)
{
	const u32 target = spu_branch_target(m_pos, op.i16);
	if (target == m_pos + 4)
	{
		return;
	}

	c.s_xtr(c.s_tmp0, op.rt, 3);
	c.s_andi(c.s_tmp0, c.s_tmp0, spv_constant::make_si(0xffff));
	c.s_brz(spv_constant::make_su(target), c.s_tmp0);
}

void spv_recompiler::BRHNZ(spu_opcode_t op)
{
	c.unimplemented("BRHNZ");
}

void spv_recompiler::STQR(spu_opcode_t op)
{
	const auto lsa = spu_ls_target(m_pos, op.i16);
	c.v_storq(spv_constant::make_su(lsa), op.rt);
}

void spv_recompiler::BRA(spu_opcode_t op)
{
	c.unimplemented("BRA");
}

void spv_recompiler::LQA(spu_opcode_t op)
{
	c.unimplemented("LQA");
}

void spv_recompiler::BRASL(spu_opcode_t op)
{
	c.unimplemented("BRASL");
}

void spv_recompiler::BR(spu_opcode_t op)
{
	const auto target = spu_branch_target(m_pos, op.i16);
	c.s_bri(spv_constant::make_su(target));
	m_pos = -1;
}

void spv_recompiler::FSMBI(spu_opcode_t op)
{
	// Compile-time complexity, no need to micro-optimize the twiddle
	u8 result[16];
	for (int bit = 0; bit < 16; ++bit)
	{
		result[bit] = (op.i16 & (1 << bit)) ? 0xFF : 0;
	}

	c.v_movsi(op.rt, spv::vector_const_t(result).as_vi());
}

void spv_recompiler::BRSL(spu_opcode_t op)
{
	const auto target = spu_branch_target(m_pos, op.i16);
	const auto lr = spu_branch_target(m_pos + 4);

	c.v_movsi(op.rt, spv_constant::make_rvi(static_cast<s32>(lr)));
	c.s_bri(spv_constant::make_su(target));

	if (target != m_pos + 4)
	{
		// Fall out
		m_pos = -1;
	}
}

void spv_recompiler::LQR(spu_opcode_t op)
{
	const auto address = spv_constant::make_su(spu_ls_target(m_pos, op.i16));
	c.v_loadq(op.rt, address);
}

void spv_recompiler::IL(spu_opcode_t op)
{
	const auto var = spv_constant::spread(op.si16);
	c.v_movsi(op.rt, var);
}

void spv_recompiler::ILHU(spu_opcode_t op)
{
	c.v_movsi(op.rt, spv_constant::spread(op.i16 << 16).as_vi());
}

void spv_recompiler::ILH(spu_opcode_t op)
{
	c.unimplemented("ILH");
}

void spv_recompiler::IOHL(spu_opcode_t op)
{
	c.v_ori(op.rt, op.rt, spv_constant::spread(op.i16).as_vi());
}

void spv_recompiler::ORI(spu_opcode_t op)
{
	if (op.si10 == 0)
	{
		// MR
		c.v_movs(op.rt, op.ra);
		return;
	}

	const auto var = spv_constant::spread(op.si10);
	c.v_ori(op.rt, op.ra, var);
}

void spv_recompiler::ORHI(spu_opcode_t op)
{
	c.unimplemented("ORHI");
}

void spv_recompiler::ORBI(spu_opcode_t op)
{
	c.unimplemented("ORBI");
}

void spv_recompiler::SFI(spu_opcode_t op)
{
	c.v_subsi(op.rt, spv_constant::spread(op.si10), op.ra);
}

void spv_recompiler::SFHI(spu_opcode_t op)
{
	c.unimplemented("SFHI");
}

void spv_recompiler::ANDI(spu_opcode_t op)
{
	c.v_andi(op.rt, op.ra, spv_constant::spread(op.si10));
}

void spv_recompiler::ANDHI(spu_opcode_t op)
{
	c.unimplemented("ANDHI");
}

void spv_recompiler::ANDBI(spu_opcode_t op)
{
	c.unimplemented("ANDBI");
}

void spv_recompiler::AI(spu_opcode_t op)
{
	c.v_addsi(op.rt, op.ra, spv_constant::spread(op.si10));
}

void spv_recompiler::AHI(spu_opcode_t op)
{
	c.unimplemented("AHI");
}

void spv_recompiler::STQD(spu_opcode_t op)
{
	const auto si10 = spv_constant::make_si(op.si10 * 16);
	const auto address_mask = spv_constant::make_si(0x3fff0);
	c.s_xtr(c.s_tmp0, op.ra, 3);
	c.s_addsi(c.s_tmp0, c.s_tmp0, si10);
	c.s_andi(c.s_tmp0, c.s_tmp0, address_mask);
	c.v_storq(c.s_tmp0, op.rt);
}

void spv_recompiler::LQD(spu_opcode_t op)
{
	const auto offset = spv_constant::make_si(op.si10 * 16);
	const auto address_mask = spv_constant::make_si(0x3fff0);
	c.s_xtr(c.s_tmp0, op.ra, 3);
	c.s_addsi(c.s_tmp0, c.s_tmp0, offset);
	c.s_andi(c.s_tmp0, c.s_tmp0, address_mask);
	c.v_loadq(op.rt, c.s_tmp0);
}

void spv_recompiler::XORI(spu_opcode_t op)
{
	c.unimplemented("XORI");
}

void spv_recompiler::XORHI(spu_opcode_t op)
{
	c.unimplemented("XORHI");
}

void spv_recompiler::XORBI(spu_opcode_t op)
{
	c.unimplemented("XORBI");
}

void spv_recompiler::CGTI(spu_opcode_t op)
{
	const auto imm = spv_constant::spread(op.si10);
	const auto all_ones = spv_constant::spread(-1);
	c.v_cmpgtsi(c.v_tmp0, op.ra, imm);
	c.v_mulsi(op.rt, c.v_tmp0, all_ones);
}

void spv_recompiler::CGTHI(spu_opcode_t op)
{
	c.unimplemented("CGTHI");
}

void spv_recompiler::CGTBI(spu_opcode_t op)
{
	c.unimplemented("CGTBI");
}

void spv_recompiler::HGTI(spu_opcode_t op)
{
	c.unimplemented("HGTI");
}

void spv_recompiler::CLGTI(spu_opcode_t op)
{
	c.unimplemented("CLGTI");
}

void spv_recompiler::CLGTHI(spu_opcode_t op)
{
	c.unimplemented("CLGTHI");
}

void spv_recompiler::CLGTBI(spu_opcode_t op)
{
	c.unimplemented("CLGTBI");
}

void spv_recompiler::HLGTI(spu_opcode_t op)
{
	c.unimplemented("HLGTI");
}

void spv_recompiler::MPYI(spu_opcode_t op)
{
	c.unimplemented("MPYI");
}

void spv_recompiler::MPYUI(spu_opcode_t op)
{
	c.unimplemented("MPYUI");
}

void spv_recompiler::CEQI(spu_opcode_t op)
{
	const auto imm = spv_constant::spread(op.si10);
	const auto all_ones = spv_constant::spread(-1);
	c.v_cmpeqsi(c.v_tmp0, op.ra, imm);
	c.v_mulsi(op.rt, c.v_tmp0, all_ones);
}

void spv_recompiler::CEQHI(spu_opcode_t op)
{
	c.unimplemented("CEQHI");
}

void spv_recompiler::CEQBI(spu_opcode_t op)
{
	// Workaround as we don't have native v16 support
	const auto value = op.si10;
	const auto set1 = spv_constant::spread(value << 0);
	const auto set2 = spv_constant::spread(value << 8);
	const auto set3 = spv_constant::spread(value << 16);
	const auto set4 = spv_constant::spread(value << 24);
	const auto mask1 = spv_constant::spread(0xff << 0);
	const auto mask2 = spv_constant::spread(0xff << 8);
	const auto mask3 = spv_constant::spread(0xff << 16);
	const auto mask4 = spv_constant::spread(0xff << 24);

	// 4 comparisons
	c.v_andi(c.v_tmp0, op.ra, mask1);
	c.v_cmpeqsi(c.v_tmp0, c.v_tmp0, set1);
	c.v_mulsi(op.rt, c.v_tmp0, mask1);

	c.v_andi(c.v_tmp1, op.ra, mask2);
	c.v_cmpeqsi(c.v_tmp1, c.v_tmp1, set2);
	c.v_mulsi(c.v_tmp1, c.v_tmp1, mask2);
	c.v_or(op.rt, op.rt, c.v_tmp1);

	c.v_andi(c.v_tmp0, op.ra, mask3);
	c.v_cmpeqsi(c.v_tmp0, c.v_tmp0, set3);
	c.v_mulsi(c.v_tmp0, c.v_tmp0, mask3);
	c.v_or(op.rt, op.rt, c.v_tmp0);

	c.v_andi(c.v_tmp1, op.ra, mask4);
	c.v_cmpeqsi(c.v_tmp1, c.v_tmp1, set4);
	c.v_mulsi(c.v_tmp1, c.v_tmp1, mask4);
	c.v_or(op.rt, op.rt, c.v_tmp1);
}

void spv_recompiler::HEQI(spu_opcode_t op)
{
	const auto value = spv_constant::make_si(op.si10);
	c.s_xtr(c.s_tmp0, op.ra, 3);
	c.s_heqi(c.s_tmp0, value);
}

void spv_recompiler::HBRA(spu_opcode_t op)
{
	// NOP
}

void spv_recompiler::HBRR(spu_opcode_t op)
{
	// NOP
}

void spv_recompiler::ILA(spu_opcode_t op)
{
	const auto var = spv_constant::spread(op.i18).as_vi();
	c.v_movsi(op.rt, var);
}

void spv_recompiler::SELB(spu_opcode_t op)
{
	c.v_xori(c.v_tmp0, op.rc, spv_constant::spread(0));
	c.v_and(c.v_tmp1, op.rc, op.rb);
	c.v_and(c.v_tmp0, c.v_tmp0, op.ra);
	c.v_or(op.rt, c.v_tmp0, c.v_tmp1);
}

void spv_recompiler::SHUFB(spu_opcode_t op)
{
	// TODO: If we already know the CW, we can redirect to shufw which is much faster
	c.q_shufb(op.rt, op.ra, op.rb, op.rc);
}

void spv_recompiler::MPYA(spu_opcode_t op)
{
	c.unimplemented("MPYA");
}

void spv_recompiler::FNMS(spu_opcode_t op)
{
	// TODO: xfloat
	// TODO: native FMA
	c.v_mulf(c.v_tmp0, op.ra, op.rb);
	c.v_subf(op.rt, op.rc, c.v_tmp0);
}

void spv_recompiler::FMA(spu_opcode_t op)
{
	// TODO: xfloat
	// TODO: native FMA
	c.v_mulf(c.v_tmp0, op.ra, op.rb);
	c.v_addf(op.rt, op.rc, c.v_tmp0);
}

void spv_recompiler::FMS(spu_opcode_t op)
{
	c.unimplemented("FMS");
}

namespace spv_constant
{
	spv::vector_const_t make_vu(u32 x, u32 y, u32 z, u32 w)
	{
		spv::vector_const_t result;
		result.m_type = spv::constant_type::UINT;
		result.m_width = 4;
		result.value.u[0] = x;
		result.value.u[1] = y;
		result.value.u[2] = z;
		result.value.u[3] = w;
		return result;
	}

	spv::vector_const_t make_vi(s32 x, s32 y, s32 z, s32 w)
	{
		spv::vector_const_t result;
		result.m_type = spv::constant_type::INT;
		result.m_width = 4;
		result.value.i[0] = x;
		result.value.i[1] = y;
		result.value.i[2] = z;
		result.value.i[3] = w;
		return result;
	}

	spv::vector_const_t make_vf(f32 x, f32 y, f32 z, f32 w)
	{
		spv::vector_const_t result;
		result.m_type = spv::constant_type::FLOAT;
		result.m_width = 4;
		result.value.f[0] = x;
		result.value.f[1] = y;
		result.value.f[2] = z;
		result.value.f[3] = w;
		return result;
	}

	spv::vector_const_t make_vh(u16 xl, u16 yl, u16 zl, u16 wl, u16 xh, u16 yh, u16 zh, u16 wh)
	{
		spv::vector_const_t result;
		result.m_type = spv::constant_type::HALF;
		result.m_width = 8;
		result.value.h[0] = xl;
		result.value.h[1] = yl;
		result.value.h[2] = zl;
		result.value.h[3] = wl;
		result.value.h[4] = xh;
		result.value.h[5] = yh;
		result.value.h[6] = zh;
		result.value.h[7] = wh;
		return result;
	}

	spv::scalar_const_t make_su(u32 x)
	{
		spv::scalar_const_t result;
		result.m_type = spv::constant_type::UINT;
		result.value.u = x;
		return result;
	}

	spv::scalar_const_t make_si(s32 x)
	{
		spv::scalar_const_t result;
		result.m_type = spv::constant_type::INT;
		result.value.i = x;
		return result;
	}

	spv::scalar_const_t make_sf(f32 x)
	{
		spv::scalar_const_t result;
		result.m_type = spv::constant_type::FLOAT;
		result.value.f = x;
		return result;
	}

	spv::scalar_const_t make_sh(u16 x)
	{
		spv::scalar_const_t result;
		result.m_type = spv::constant_type::HALF;
		result.value.u = x;
		return result;
	}

	spv::vector_const_t spread(u32 imm)
	{
		return make_vu(imm, imm, imm, imm);
	}

	spv::vector_const_t spread(s32 imm)
	{
		return make_vi(imm, imm, imm, imm);
	}

	spv::vector_const_t spread(f32 imm)
	{
		return make_vf(imm, imm, imm, imm);
	}

	spv::vector_const_t spread(u16 imm)
	{
		return make_vh(imm, imm, imm, imm, imm, imm, imm, imm);
	}

	spv::vector_const_t spread(u8 imm)
	{
		const auto v = s32(imm);
		return spread(v | v << 8 | v << 16 | v << 24);
	}
};

// Management
void spv_emitter::reset()
{
	m_block.clear();
	m_v_const_array.clear();
	m_s_const_array.clear();
	m_compiler_config = {};
}

std::unique_ptr<spv::SPUSPV_block> spv_emitter::compile()
{
	std::stringstream shaderbuf;
	// Declare common
	shaderbuf <<
		#include "SPIRV/SPV_Header.glsl"
	;

	if (m_compiler_config.uses_shufb)
	{
		// Sad case for shufb: GPUs support permutation instructions on both NV and AMD but glsl does not expose this.
		// TODO: Find a workaround or use SYCL
		shaderbuf <<
			#include "SPIRV/QSHUFB.glsl"
		;
	}

	if (m_compiler_config.uses_qrotl32)
	{
		shaderbuf <<
			#include "SPIRV/QROTL32.glsl"
		;
	}

	if (m_compiler_config.uses_qrotr32)
	{
		shaderbuf <<
			#include "SPIRV/QROTR32.glsl"
		;
	}

	if (m_compiler_config.uses_qrotl)
	{
		shaderbuf <<
			#include "SPIRV/QROTL.glsl"
		;
	}

	if (m_compiler_config.uses_qrotr)
	{
		shaderbuf <<
			#include "SPIRV/QROTR.glsl"
		;
	}

	if (m_compiler_config.uses_qshl)
	{
		shaderbuf <<
			#include "SPIRV/QSHL.glsl"
		;
	}

	if (m_compiler_config.uses_qshr)
	{
		shaderbuf <<
			#include "SPIRV/QSHR.glsl"
		;
	}

	if (m_compiler_config.uses_qshl32)
	{
		shaderbuf <<
			#include "SPIRV/QSHL32.glsl"
		;
	}

	if (m_compiler_config.uses_qshr32)
	{
		shaderbuf <<
			#include "SPIRV/QSHR32.glsl"
		;
	}

	if (m_compiler_config.uses_mfc)
	{
		// LAZY!!!!
		shaderbuf <<
			#include "SPIRV/MFC_write.glsl"
		;
	}

	// TODO: Cache/mirror registers

	// Constants
	for (usz idx = 0; idx < m_v_const_array.size(); ++idx)
	{
		// TODO: Non-base-type support
		const auto& const_ = m_v_const_array[idx];
		const auto [declared_type, array_size, initializer] = [](const spv::vector_const_t& const_) -> std::tuple<std::string_view, std::string_view, std::string>
		{
			switch (const_.m_type)
			{
				case spv::constant_type::FLOAT:
				{
					const auto initializer = fmt::format(
						"vec4(%f, %f, %f, %f)",
						const_.value.f[0], const_.value.f[1], const_.value.f[2], const_.value.f[3]);
					return { "vec4", "",  initializer };
				}
				case spv::constant_type::INT:
				{
					const auto initializer = fmt::format(
						"ivec4(%d, %d, %d, %d)",
						const_.value.i[0], const_.value.i[1], const_.value.i[2], const_.value.i[3]);
					return { "ivec4", "", initializer };
				}
				case spv::constant_type::UINT:
				{
					const auto initializer = fmt::format(
						"uvec4(%d, %d, %d, %d)",
						const_.value.u[0], const_.value.u[1], const_.value.u[2], const_.value.u[3]);
					return { "uvec4", "", initializer };
				}
				case spv::constant_type::HALF:
				{
					// TODO: Width optimizations
					const auto initializer = fmt::format(
						"{ vec4(%f, %f, %f, %f), vec4(%f, %f, %f, %f) };\n",
						rsx::decode_fp16(const_.value.h[0]), rsx::decode_fp16(const_.value.h[1]), rsx::decode_fp16(const_.value.h[2]), rsx::decode_fp16(const_.value.h[3]),
						rsx::decode_fp16(const_.value.h[4]), rsx::decode_fp16(const_.value.h[5]), rsx::decode_fp16(const_.value.h[6]), rsx::decode_fp16(const_.value.h[7]));
					return { "vec4", "[2]", initializer};
				}
				case spv::constant_type::SHORT:
				{
					const auto initializer = fmt::format(
						"{ ivec4(%d, %d, %d, %d), ivec4(%d, %d, %d, %d) };\n",
						const_.value.h[0], const_.value.h[1], const_.value.h[2], const_.value.h[3],
						const_.value.h[4], const_.value.h[5], const_.value.h[6], const_.value.h[7]);
					return { "ivec4", "[2]", initializer };
				}
				case spv::constant_type::USHORT:
				{
					const auto initializer = fmt::format(
						"{ uvec4(%u, %u, %u, %u), uvec4(%u, %u, %u, %u) };\n",
						const_.value.h[0], const_.value.h[1], const_.value.h[2], const_.value.h[3],
						const_.value.h[4], const_.value.h[5], const_.value.h[6], const_.value.h[7]);
					return { "uvec4", "[2]", initializer };
				}
				case spv::constant_type::BYTE:
				{
					const auto initializer = fmt::format(
						"{ ivec4(%d, %d, %d, %d), ivec4(%d, %d, %d, %d), ivec4(%d, %d, %d, %d), ivec4(%d, %d, %d, %d) };\n",
						const_.value.b[0], const_.value.b[1], const_.value.b[2], const_.value.b[3],
						const_.value.b[4], const_.value.b[5], const_.value.b[6], const_.value.b[7],
						const_.value.b[8], const_.value.b[9], const_.value.b[10], const_.value.b[11],
						const_.value.b[12], const_.value.b[13], const_.value.b[14], const_.value.b[15]);
					return { "ivec4", "[4]", initializer };
				}
				case spv::constant_type::UBYTE:
				{
					const auto initializer = fmt::format(
						"{ uvec4(%u, %u, %u, %u), uvec4(%u, %u, %u, %u), uvec4(%u, %u, %u, %u), uvec4(%u, %u, %u, %u) };\n",
						const_.value.b[0], const_.value.b[1], const_.value.b[2], const_.value.b[3],
						const_.value.b[4], const_.value.b[5], const_.value.b[6], const_.value.b[7],
						const_.value.b[8], const_.value.b[9], const_.value.b[10], const_.value.b[11],
						const_.value.b[12], const_.value.b[13], const_.value.b[14], const_.value.b[15]);
					return { "uvec4", "[4]", initializer };
				}
				default:
				{
					fmt::throw_exception("Unreachable");
				}
			};
		}(const_);

		shaderbuf << fmt::format(
			"const %s v_const_%d%s = %s;\n",
			declared_type, idx, array_size, initializer);
	}

	for (usz idx = 0; idx < m_s_const_array.size(); ++idx)
	{
		// TODO: Non-base-type support
		const auto& const_ = m_s_const_array[idx];
		const auto [declared_type, value] = [](const spv::scalar_const_t& const_) -> std::tuple<std::string_view,  std::string>
		{
			switch (const_.m_type)
			{
			case spv::constant_type::FLOAT:
			{
				return { "float", std::to_string(const_.value.f) };
			}
			case spv::constant_type::INT:
			{
				return { "int", std::to_string(const_.value.i) };
			}
			case spv::constant_type::UINT:
			{
				return { "uint", std::to_string(const_.value.u) };
			}
			case spv::constant_type::HALF:
			{
				return { "float", std::to_string(rsx::decode_fp16(const_.value.h)) };
			}
			case spv::constant_type::SHORT:
			{
				return { "int", std::to_string(const_.value.h) };
			}
			case spv::constant_type::USHORT:
			{
				return { "uint", std::to_string(const_.value.h) };
			}
			case spv::constant_type::BYTE:
			{
				return { "int", std::to_string(const_.value.b) };
			}
			case spv::constant_type::UBYTE:
			{
				return { "uint", std::to_string(const_.value.b) };
			}
			default:
			{
				fmt::throw_exception("Unreachable");
			}
			};
		}(const_);

		shaderbuf << fmt::format(
			"const %s s_const_%d = %s;\n",
			declared_type, idx, value);
	}

	// Body
	shaderbuf <<
		"\n"
		"void execute()\n"
		"{\n" <<
			m_block <<
		"}\n\n";

	// Entry
	// TODO: Retire/flush cache registers
	shaderbuf <<
		"void main()\n"
		"{\n"
		"	exit_code = 0;\n"
		"	execute();\n"
		"	lr = vgpr[0].w;\n"
		"	sp = vgpr[1].w;\n"
		"	dr[11] = vgpr[80][0];\n"
		"	dr[12] = vgpr[80][1];\n"
		"	dr[13] = vgpr[80][2];\n"
		"	dr[14] = vgpr[80][3];\n"
		"	dr[15] = sgpr[0];\n"
		"}\n\n"; //qshl_mask_lookup

	m_block = shaderbuf.str();
	auto result = std::make_unique<spv::SPUSPV_block>(m_block);
	result->is_dynamic_branch_block = has_dynamic_branch_target();
	result->issues_memory_op = has_memory_dependency();
	result->end_pc = get_pc();
	return result;
}

// Arithmetic ops
void spv_emitter::v_addsi(spv::vector_register_t dst, spv::vector_register_t op0, const spv::vector_const_t& op1)
{
	m_block += fmt::format(
		"vgpr[%d] = vgpr[%d] + %s;\n",
		dst.vgpr_index, op0.vgpr_index, get_const_name(op1));
}

void spv_emitter::v_adds(spv::vector_register_t dst, spv::vector_register_t op0, spv::vector_register_t op1)
{
	m_block += fmt::format(
		"vgpr[%d] = vgpr[%d] + vgpr[%d];\n",
		dst.vgpr_index, op0.vgpr_index, op1.vgpr_index);
}

void spv_emitter::v_subs(spv::vector_register_t dst, spv::vector_register_t op0, spv::vector_register_t op1)
{
	m_block += fmt::format(
		"vgpr[%d] = vgpr[%d] - vgpr[%d];\n",
		dst.vgpr_index, op0.vgpr_index, op1.vgpr_index);
}

void spv_emitter::v_subsi(spv::vector_register_t dst, spv::vector_register_t op0, const spv::vector_const_t& op1)
{
	m_block += fmt::format(
		"vgpr[%d] = vgpr[%d] - %s;\n",
		dst.vgpr_index, op0.vgpr_index, get_const_name(op1));
}

void spv_emitter::v_subsi(spv::vector_register_t dst, const spv::vector_const_t& op0, spv::vector_register_t op1)
{
	m_block += fmt::format(
		"vgpr[%d] = %s - vgpr[%d];\n",
		dst.vgpr_index, get_const_name(op0), op1.vgpr_index);
}

void spv_emitter::v_addf(spv::vector_register_t dst, spv::vector_register_t op0, spv::vector_register_t op1)
{
	m_block += fmt::format(
		"vgpr[%d] = floatBitsToInt(intBitsToFloat(vgpr[%d]) + intBitsToFloat(vgpr[%d]));\n",
		dst.vgpr_index, op0.vgpr_index, op1.vgpr_index);
}

void spv_emitter::v_subf(spv::vector_register_t dst, spv::vector_register_t op0, spv::vector_register_t op1)
{
	m_block += fmt::format(
		"vgpr[%d] = floatBitsToInt(intBitsToFloat(vgpr[%d]) - intBitsToFloat(vgpr[%d]));\n",
		dst.vgpr_index, op0.vgpr_index, op1.vgpr_index);
}

void spv_emitter::v_mulsi(spv::vector_register_t dst, spv::vector_register_t op0, const spv::vector_const_t& op1)
{
	m_block += fmt::format(
		"vgpr[%d] = vgpr[%d] * %s;\n",
		dst.vgpr_index, op0.vgpr_index, get_const_name(op1));
}

void spv_emitter::v_muls(spv::vector_register_t dst, spv::vector_register_t op0, spv::vector_register_t op1)
{
	m_block += fmt::format(
		"vgpr[%d] = vgpr[%d] * vgpr[%d];\n",
		dst.vgpr_index, op0.vgpr_index, op1.vgpr_index);
}

void spv_emitter::v_mulu(spv::vector_register_t dst, spv::vector_register_t op0, spv::vector_register_t op1)
{
	m_block += fmt::format(
		"vgpr[%d] = ivec4(uvec4(vgpr[%d]) * uvec4(vgpr[%d]));\n",
		dst.vgpr_index, op0.vgpr_index, op1.vgpr_index);
}

void spv_emitter::v_mulfi(spv::vector_register_t dst, spv::vector_register_t op0, const spv::vector_const_t& op1)
{
	m_block += fmt::format(
		"vgpr[%d] = floatBitsToInt(intBitsToFloat(vgpr[%d]) * %s);\n",
		dst.vgpr_index, op0.vgpr_index, get_const_name(op1));
}

void spv_emitter::v_mulf(spv::vector_register_t dst, spv::vector_register_t op0, spv::vector_register_t op1)
{
	m_block += fmt::format(
		"vgpr[%d] = floatBitsToInt(intBitsToFloat(vgpr[%d]) * intBitsToFloat(vgpr[%d]));\n",
		dst.vgpr_index, op0.vgpr_index, op1.vgpr_index);
}

void spv_emitter::v_rcpf(spv::vector_register_t dst, spv::vector_register_t op0)
{
	m_block += fmt::format(
		"vgpr[%d] = floatBitsToInt(1.f / intBitsToFloat(vgpr[%d]));\n",
		dst.vgpr_index, op0.vgpr_index);
}

void spv_emitter::v_rsqf(spv::vector_register_t dst, spv::vector_register_t op0)
{
	m_block += fmt::format(
		"vgpr[%d] = floatBitsToInt(inversesqrt(intBitsToFloat(vgpr[%d])));\n",
		dst.vgpr_index, op0.vgpr_index);
}

void spv_emitter::v_addui(spv::vector_register_t dst, spv::vector_register_t op0, const spv::vector_const_t& op1)
{
	m_block += fmt::format(
		"vgpr[%d] = ivec4(uvec4(vgpr[%d]) + %s);\n",
		dst.vgpr_index, op0.vgpr_index, get_const_name(op1.as_vu()));
}

void spv_emitter::s_addsi(spv::scalar_register_t dst, spv::scalar_register_t op0, const spv::scalar_const_t& op1)
{
	m_block += fmt::format(
		"sgpr[%d] = sgpr[%d] + %s;\n",
		dst.sgpr_index, op0.sgpr_index, get_const_name(op1));
}

void spv_emitter::s_adds(spv::scalar_register_t dst, spv::scalar_register_t op0, spv::scalar_register_t op1)
{
	m_block += fmt::format(
		"sgpr[%d] = sgpr[%d] + sgpr[%d];\n",
		dst.sgpr_index, op0.sgpr_index, op1.sgpr_index);
}

void spv_emitter::s_subsi(spv::scalar_register_t dst, spv::scalar_register_t op0, const spv::scalar_const_t& op1)
{
	m_block += fmt::format(
		"sgpr[%d] = sgpr[%d] - %s;\n",
		dst.sgpr_index, op0.sgpr_index, get_const_name(op1));
}

void spv_emitter::s_subsi(spv::scalar_register_t dst, const spv::scalar_const_t& op0, spv::scalar_register_t op1)
{
	m_block += fmt::format(
		"sgpr[%d] = %s - %s;\n",
		dst.sgpr_index, get_const_name(op0), op1.sgpr_index);
}

void spv_emitter::s_dp4s(spv::scalar_register_t dst, spv::vector_register_t op0, spv::vector_register_t op1)
{
	m_block += fmt::format(
		"sgpr[%d] = int(dot(vgpr[%d], vgpr[%d]));\n",
		dst.sgpr_index, op0.vgpr_index, op1.vgpr_index);
}

void spv_emitter::s_dp4si(spv::scalar_register_t dst, spv::vector_register_t op0, const spv::vector_const_t& op1)
{
	m_block += fmt::format(
		"sgpr[%d] = int(dot(vgpr[%d], %s));\n",
		dst.sgpr_index, op0.vgpr_index, get_const_name(op1));
}

// Comparison
void spv_emitter::v_cmpeqsi(spv::vector_register_t dst, spv::vector_register_t op0, const spv::vector_const_t& op1)
{
	m_block += fmt::format(
		"vgpr[%d] = ivec4(equal(vgpr[%d], %s));\n",
		dst.vgpr_index, op0.vgpr_index, get_const_name(op1));
}

void spv_emitter::v_cmpgtsi(spv::vector_register_t dst, spv::vector_register_t op0, const spv::vector_const_t& op1)
{
	m_block += fmt::format(
		"vgpr[%d] = ivec4(greaterThan(vgpr[%d], %s));\n",
		dst.vgpr_index, op0.vgpr_index, get_const_name(op1));
}

void spv_emitter::v_cmpgts(spv::vector_register_t dst, spv::vector_register_t op0, spv::vector_register_t op1)
{
	m_block += fmt::format(
		"vgpr[%d] = ivec4(greaterThan(vgpr[%d], vgpr[%d]));\n",
		dst.vgpr_index, op0.vgpr_index, op1.vgpr_index);
}

void spv_emitter::v_cmpgtu(spv::vector_register_t dst, spv::vector_register_t op0, spv::vector_register_t op1)
{
	m_block += fmt::format(
		"vgpr[%d] = ivec4(greaterThan(uvec4(vgpr[%d]), uvec4(vgpr[%d])));\n",
		dst.vgpr_index, op0.vgpr_index, op1.vgpr_index);
}

void spv_emitter::v_cmpgtf(spv::vector_register_t dst, spv::vector_register_t op0, spv::vector_register_t op1)
{
	m_block += fmt::format(
		"vgpr[%d] = ivec4(greaterThan(intBitsToFloat(vgpr[%d]), intBitsToFloat(vgpr[%d])));\n",
		dst.vgpr_index, op0.vgpr_index, op1.vgpr_index);
}

void spv_emitter::v_clampfi(spv::vector_register_t dst, spv::vector_register_t op0, const spv::vector_const_t& min, const spv::vector_const_t& max)
{
	m_block += fmt::format(
		"vgpr[%d] = ivec4(clamp(intBitsToFloat(vgpr[%d]), %s, %s));\n",
		dst.vgpr_index, op0.vgpr_index, get_const_name(min), get_const_name(max));
}

// Movs
void spv_emitter::v_movsi(spv::vector_register_t dst, const spv::vector_const_t& src)
{
	m_block += fmt::format(
		"vgpr[%d] = %s;\n",
		dst.vgpr_index, get_const_name(src));
}

void spv_emitter::v_movs(spv::vector_register_t dst, spv::vector_register_t src)
{
	m_block += fmt::format(
		"vgpr[%d] = vgpr[%d];\n",
		dst.vgpr_index, src.vgpr_index);
}

void spv_emitter::v_movfi(spv::vector_register_t dst, const spv::vector_const_t& src)
{
	m_block += fmt::format(
		"vgpr[%d] = floatBitsToInt(%s);\n",
		dst.vgpr_index, get_const_name(src));
}

void spv_emitter::s_movsi(spv::scalar_register_t dst_reg, const spv::scalar_const_t& src)
{
	m_block += fmt::format(
		"sgpr[%d] = %s;\n",
		dst_reg.sgpr_index, get_const_name(src));
}

void spv_emitter::s_storsr(spv::scalar_register_t dst_reg, spv::scalar_register_t src)
{
	m_block += fmt::format(
		"%s = sgpr[%d];\n",
		get_sysreg_name(dst_reg), src.sgpr_index);
}

void spv_emitter::s_loadsr(spv::scalar_register_t dst_reg, spv::scalar_register_t src)
{
	m_block += fmt::format(
		"sgpr[%d] = %s;\n",
		dst_reg.sgpr_index, get_sysreg_name(src));
}

void spv_emitter::v_storq(spv::scalar_register_t lsa, spv::vector_register_t src_reg)
{
	m_block += fmt::format(
		"ls[sgpr[%d] >> 4] = _bswap(vgpr[%d]);\n",
		lsa.sgpr_index, src_reg.vgpr_index);
}

void spv_emitter::v_storq(spv::scalar_const_t lsa, spv::vector_register_t src_reg)
{
	m_block += fmt::format(
		"ls[%s >> 4] = _bswap(vgpr[%d]);\n",
		get_const_name(lsa), src_reg.vgpr_index);
}

void spv_emitter::v_loadq(spv::vector_register_t dst_reg, spv::scalar_register_t lsa)
{
	m_block += fmt::format(
		"vgpr[%d] = _bswap(ls[sgpr[%d] >> 4]);\n"
		"dr[3] = sgpr[%d];\n",
		dst_reg.vgpr_index, lsa.sgpr_index, lsa.sgpr_index);
}

void spv_emitter::v_loadq(spv::vector_register_t dst_reg, spv::scalar_const_t lsa)
{
	m_block += fmt::format(
		"vgpr[%d] = _bswap(ls[%s >> 4]);\n",
		dst_reg.vgpr_index, get_const_name(lsa));
}

void spv_emitter::v_sprd(spv::vector_register_t dst_reg, spv::scalar_register_t src_reg)
{
	m_block += fmt::format(
		"vgpr[%d] = ivec4(sgpr[%d]);\n",
		dst_reg.vgpr_index, src_reg.sgpr_index);
}

void spv_emitter::v_sprd(spv::vector_register_t dst_reg, spv::vector_register_t src_reg, int component)
{
	m_block += fmt::format(
		"vgpr[%d] = ivec4(vgpr[%d][%d]);\n",
		dst_reg.vgpr_index, src_reg.vgpr_index, component);
}

// Cast
void spv_emitter::v_fcvtu(spv::vector_register_t dst, spv::vector_register_t src)
{
	m_block += fmt::format(
		"vgpr[%d] = ivec4(intBitsToFloat(vgpr[%d]));\n",
		dst.vgpr_index, src.vgpr_index);
}

void spv_emitter::v_scvtf(spv::vector_register_t dst, spv::vector_register_t src)
{
	m_block += fmt::format(
		"vgpr[%d] = floatBitsToInt(vec4(vgpr[%d]));\n",
		dst.vgpr_index, src.vgpr_index);
}

void spv_emitter::v_ucvtf(spv::vector_register_t dst, spv::vector_register_t src)
{
	m_block += fmt::format(
		"vgpr[%d] = floatBitsToInt(vec4(uvec4(vgpr[%d])));\n",
		dst.vgpr_index, src.vgpr_index);
}

// Bitwise
void spv_emitter::v_andi(spv::vector_register_t dst, spv::vector_register_t op0, const spv::vector_const_t op1)
{
	m_block += fmt::format(
		"vgpr[%d] = vgpr[%d] & %s;\n",
		dst.vgpr_index, op0.vgpr_index, get_const_name(op1));
}

void spv_emitter::v_and(spv::vector_register_t dst, spv::vector_register_t op0, spv::vector_register_t op1)
{
	m_block += fmt::format(
		"vgpr[%d] = vgpr[%d] & vgpr[%d];\n",
		dst.vgpr_index, op0.vgpr_index, op1.vgpr_index);
}

void spv_emitter::v_bfxi(spv::vector_register_t dst, spv::vector_register_t op0, const spv::scalar_const_t& op1, const spv::scalar_const_t& op2)
{
	m_block += fmt::format(
		"vgpr[%d] = _vbfe(vgpr[%d], %s, %s);\n",
		dst.vgpr_index, op0.vgpr_index, get_const_name(op1), get_const_name(op2));
}

void spv_emitter::v_bfx(spv::vector_register_t dst, spv::vector_register_t op0, spv::scalar_register_t op1, spv::scalar_register_t op2)
{
	m_block += fmt::format(
		"vgpr[%d] = _vbfe(vgpr[%d], sgpr[%d], sgpr[%d]);\n",
		dst.vgpr_index, op0.vgpr_index, op1.sgpr_index, op2.sgpr_index);
}

void spv_emitter::v_shli(spv::vector_register_t dst, spv::vector_register_t op0, const spv::vector_const_t& op1)
{
	m_block += fmt::format(
		"vgpr[%d] = vgpr[%d] << %s;\n",
		dst.vgpr_index, op0.vgpr_index, get_const_name(op1));
}

void spv_emitter::v_shl(spv::vector_register_t dst, spv::vector_register_t op0, spv::vector_register_t op1)
{
	m_block += fmt::format(
		"vgpr[%d] = vgpr[%d] << vgpr[%d];\n",
		dst.vgpr_index, op0.vgpr_index, op1.vgpr_index);
}

void spv_emitter::v_shri(spv::vector_register_t dst, spv::vector_register_t op0, const spv::vector_const_t& op1)
{
	m_block += fmt::format(
		"vgpr[%d] = vgpr[%d] >> %s;\n",
		dst.vgpr_index, op0.vgpr_index, get_const_name(op1));
}

void spv_emitter::v_xori(spv::vector_register_t dst, spv::vector_register_t op0, const spv::vector_const_t& op1)
{
	m_block += fmt::format(
		"vgpr[%d] = vgpr[%d] ^ %s;\n",
		dst.vgpr_index, op0.vgpr_index, get_const_name(op1));
}

void spv_emitter::v_ori(spv::vector_register_t dst, spv::vector_register_t op0, const spv::vector_const_t& op1)
{
	m_block += fmt::format(
		"vgpr[%d] = vgpr[%d] | %s;\n",
		dst.vgpr_index, op0.vgpr_index, get_const_name(op1));
}

void spv_emitter::v_or(spv::vector_register_t dst, spv::vector_register_t op0, spv::vector_register_t op1)
{
	m_block += fmt::format(
		"vgpr[%d] = vgpr[%d] | vgpr[%d];\n",
		dst.vgpr_index, op0.vgpr_index, op1.vgpr_index);
}

void spv_emitter::v_shufwi(spv::vector_register_t dst, spv::vector_register_t src, const std::array<int, 4>& shuffle)
{
	const char* mask_components[4] = { "x", "y", "z", "w" };

	std::string mask = "";
	for (const auto& index : shuffle)
	{
		ensure(index < 4);
		mask += mask_components[index];
	}

	m_block += fmt::format(
		"vgpr[%d] = vgpr[%d].%s;\n",
		dst.vgpr_index, src.vgpr_index, mask);
}

void spv_emitter::s_andi(spv::scalar_register_t dst, spv::scalar_register_t op0, const spv::scalar_const_t& op1)
{
	m_block += fmt::format(
		"sgpr[%d] = sgpr[%d] & %s;\n",
		dst.sgpr_index, op0.sgpr_index, get_const_name(op1));
}

void spv_emitter::s_xori(spv::scalar_register_t dst, spv::scalar_register_t op0, const spv::scalar_const_t& op1)
{
	m_block += fmt::format(
		"sgpr[%d] = sgpr[%d] ^ %s;\n",
		dst.sgpr_index, op0.sgpr_index, get_const_name(op1));
}

void spv_emitter::s_shli(spv::scalar_register_t dst, spv::scalar_register_t op0, const spv::scalar_const_t& op1)
{
	m_block += fmt::format(
		"sgpr[%d] = sgpr[%d] << %s;\n",
		dst.sgpr_index, op0.sgpr_index, get_const_name(op1));
}

void spv_emitter::s_hzor(spv::scalar_register_t dst, spv::vector_register_t op0)
{
	m_block += fmt::format(
		"sgpr[%d] = (vgpr[%d].x | vgpr[%d].y | vgpr[%d].z | vgpr[%d].w);\n",
		dst.sgpr_index, op0.vgpr_index, op0.vgpr_index, op0.vgpr_index, op0.vgpr_index);
}

void spv_emitter::s_xtr(spv::scalar_register_t dst, spv::vector_register_t src, int component)
{
	m_block += fmt::format(
		"sgpr[%d] = vgpr[%d][%d];\n",
		dst.sgpr_index, src.vgpr_index, component);
}

void spv_emitter::s_ins(spv::vector_register_t dst, spv::scalar_register_t src, int component)
{
	m_block += fmt::format(
		"vgpr[%d][%d] = sgpr[%d];\n",
		dst.vgpr_index, component, src.sgpr_index);
}

void spv_emitter::s_ins(spv::vector_register_t dst, const spv::scalar_const_t& src, spv::scalar_register_t select)
{
	m_block += fmt::format(
		"vgpr[%d][sgpr[%d]] = %s;\n",
		dst.vgpr_index, select.sgpr_index, get_const_name(src));
}

void spv_emitter::q_shufb(spv::vector_register_t dst, spv::vector_register_t op0, spv::vector_register_t op1, spv::vector_register_t op2)
{
	m_block += fmt::format(
		"vgpr[%d] = _dqshufb(vgpr[%d], vgpr[%d], vgpr[%d]);\n",
		dst.vgpr_index, op0.vgpr_index, op1.vgpr_index, op2.vgpr_index);

	m_compiler_config.enable_shufb();
}

// Flow control
void spv_emitter::s_bri(const spv::scalar_const_t& target)
{
	m_block += fmt::format(
		"pc = %s;\n",
		get_const_name(target.as_u()));

	m_compiler_config.set_pc(target.value.i);
}

void spv_emitter::s_br(spv::scalar_register_t target)
{
	m_block += fmt::format(
		"pc = sgpr[%d];\n",
		target.sgpr_index);

	m_compiler_config.enable_dynamic_branch();
}

void spv_emitter::s_brz(const spv::scalar_const_t& target, spv::scalar_register_t cond, spv::exit_code exit_code)
{
	m_block += fmt::format(
		"if (sgpr[%d] == 0)\n"
		"{\n"
		"	pc = %s;\n"
		"	exit_code = %s;\n"
		"	return;\n"
		"}\n",
		cond.sgpr_index, get_const_name(target.as_u()), exit_code);

	m_compiler_config.enable_dynamic_branch();
}

void spv_emitter::s_brnz(const spv::scalar_const_t& target, spv::scalar_register_t cond)
{
	m_block += fmt::format(
		"if (sgpr[%d] != 0)\n"
		"{\n"
		"	pc = %s;\n"
		"	return;\n"
		"}\n",
		cond.sgpr_index, get_const_name(target.as_u()));

	m_compiler_config.enable_dynamic_branch();
}

void spv_emitter::s_heq(spv::scalar_register_t op1, spv::scalar_register_t op2)
{
	m_block += fmt::format(
		"if (sgpr[%d] == sgpr[%d])\n"
		"{\n"
		"	exit_code = SPU_HLT;\n"
		"	return;\n"
		"}\n",
		op1.sgpr_index, op2.sgpr_index);

	m_compiler_config.enable_dynamic_branch();
}

void spv_emitter::s_heqi(spv::scalar_register_t op1, const spv::scalar_const_t& op2)
{
	m_block += fmt::format(
		"if (sgpr[%d] == %s)\n"
		"{\n"
		"	exit_code = SPU_HLT;\n"
		"	return;\n"
		"}\n",
		op1.sgpr_index, get_const_name(op2));

	m_compiler_config.enable_dynamic_branch();
}

void spv_emitter::s_exit(const spv::exit_code& code)
{
	m_block += fmt::format(
		"exit_code = %s;\n"
		"return;\n",
		code);
}

void spv_emitter::s_call(const std::string_view& function, const std::vector<std::string_view>& args)
{
	std::string argsstr = "";
	for (const auto& arg : args)
	{
		if (argsstr.length() > 0)
		{
			argsstr += ", ";
		}
		argsstr += arg;
	}

	m_block += fmt::format(
		"%s(%s);\n",
		function, argsstr);

	if (function.starts_with("MFC"))
	{
		m_compiler_config.enable_mfc();
	}
}

void spv_emitter::q_rotl(spv::vector_register_t dst, spv::vector_register_t op0, spv::scalar_register_t op1)
{
	m_block += fmt::format(
		"vgpr[%d] = _qrotl(vgpr[%d], sgpr[%d]);\n",
		dst.vgpr_index, op0.vgpr_index, op1.sgpr_index);

	m_compiler_config.enable_qrotl();
}

void spv_emitter::q_rotr(spv::vector_register_t dst, spv::vector_register_t op0, spv::scalar_register_t op1)
{
	m_block += fmt::format(
		"vgpr[%d] = _qrotr(vgpr[%d], sgpr[%d]);\n",
		dst.vgpr_index, op0.vgpr_index, op1.sgpr_index);

	m_compiler_config.enable_qrotr();
}

void spv_emitter::q_shl(spv::vector_register_t dst, spv::vector_register_t op0, spv::scalar_register_t op1)
{
	m_block += fmt::format(
		"vgpr[%d] = _qshl(vgpr[%d], sgpr[%d]);\n",
		dst.vgpr_index, op0.vgpr_index, op1.sgpr_index);

	m_compiler_config.enable_qshl();
}

void spv_emitter::q_shr(spv::vector_register_t dst, spv::vector_register_t op0, spv::scalar_register_t op1)
{
	m_block += fmt::format(
		"vgpr[%d] = _qshr(vgpr[%d], sgpr[%d]);\n",
		dst.vgpr_index, op0.vgpr_index, op1.sgpr_index);

	m_compiler_config.enable_qshr();
}

void spv_emitter::unimplemented(const std::string_view& name)
{
	m_block += fmt::format(
		"%s();\n",
		name);

	println(name.data());
}

std::string spv_emitter::get_const_name(const spv::vector_const_t& const_)
{
	usz index = umax;
	for (usz i = 0; i < m_v_const_array.size(); ++i)
	{
		const auto& this_const = m_v_const_array[i];
		if (this_const.m_type == const_.m_type &&
			!std::memcmp(&this_const.value, &const_.value, sizeof(const_.value)))
		{
			index = i;
			break;
		}
	}

	if (index == umax)
	{
		index = m_v_const_array.size();
		m_v_const_array.push_back(const_);
	}

	return "v_const_" + std::to_string(index);
}

std::string spv_emitter::get_const_name(const spv::scalar_const_t& const_)
{
	usz index = umax;
	for (usz i = 0; i < m_s_const_array.size(); ++i)
	{
		const auto& this_const = m_s_const_array[i];
		if (this_const.m_type == const_.m_type &&
			!std::memcmp(&this_const.value, &const_.value, sizeof(const_.value)))
		{
			index = i;
			break;
		}
	}

	if (index == umax)
	{
		index = m_s_const_array.size();
		m_s_const_array.push_back(const_);
	}

	return "s_const_" + std::to_string(index);
}

std::string_view spv_emitter::get_sysreg_name(const spv::scalar_register_t& reg)
{
	ensure(reg.sgpr_index >= s_srr0.sgpr_index);
	const auto index = reg.sgpr_index - s_srr0.sgpr_index;
	ensure(index < m_system_register_names_s.size());

	return m_system_register_names_s[index];
}
