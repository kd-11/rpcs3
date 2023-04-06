#include "stdafx.h"
#include "SPUSPVRecompiler.h"
#include "SPIRV/Runtime.h"

#include "Emu/IdManager.h"
#include "Emu/system_config.h"
#include "Crypto/sha1.h"

#include "Emu/RSX/VK/vkutils/buffer_object.h"
#include "Emu/RSX/VK/vkutils/descriptors.h"
#include "Emu/RSX/VK/vkutils/memory.h"
#include "Emu/RSX/VK/VKProgramPipeline.h"
#include "Emu/RSX/VK/VKPipelineCompiler.h"
//#include "Emu/RSX/VK/VKGSRenderTypes.hpp"
#include "Emu/RSX/VK/VKGSRender.h"

#include <windows.h>
#pragma optimize("", off)

#define SPU_DEBUG 0
#define SPU_PERF_TRACE 0

constexpr auto SPV_MAX_BLOCKS = 65536u;

extern fs::file g_tty;
extern atomic_t<s64> g_tty_size;

atomic_t<u32> g_leader;

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
		case spv::exit_code::STOP_AND_SIGNAL: return "SPU_STOP_AND_SIGNAL";
		default: break;
		}

		return unknown;
	});
}

namespace
{
	const spu_decoder<spv_recompiler> s_spu_decoder;

	const auto println = [](const char* s)
	{
		spu_log.error("[spvc] %s", s);
	};

	const auto tty_write = [](const std::string& msg)
	{
		g_tty_size -= (1ll << 48);
		g_tty.write(msg);
		g_tty_size += (1ll << 48) + msg.length();
	};
}

namespace spv
{
	VkDescriptorSetLayout g_set_layout = VK_NULL_HANDLE;
	VkPipelineLayout g_pipeline_layout = VK_NULL_HANDLE;

	std::mutex g_init_mutex;

	static bool s_ctx_initialized = false;
	static u32 s_used_descriptors = 0;

	// Context for the logical SPU
	struct SPU_execution_context_t
	{
		std::unique_ptr<vk::buffer> gpr_block;       // 144 GPRs (128 base + 16 spill/temp)
		std::unique_ptr<vk::buffer> ls_block;        // LS SRAM
		std::unique_ptr<vk::buffer> ls_mirror_block; // LS SRAM
		std::unique_ptr<vk::buffer> teb_block;       // Next PC, interrupts, etc
		std::unique_ptr<vk::buffer> gpu_constants;   // Builtin constants lookup table

		bool flush_caches = true;
		bool flush_regs = true;
		bool init_constants = true;

		std::pair<u32, u32> flush_cache_region = {0 , 256 * 1024};
		std::pair<u32, u32> flush_regs_region = { 0, 128 };

		TEB *teb = nullptr;
		u8* ls = nullptr;
		SPU_GPR* gpr = nullptr;

		vk::descriptor_pool descriptor_pool;
		vk::command_pool command_pool;

		vk::command_buffer_chain<32> command_buffer_list;
		vk::descriptor_set descriptor_set;

#if SPU_PERF_TRACE
		bool is_leader = (0u == g_leader++);
#else
		const bool is_leader = false;
#endif

		SPU_execution_context_t(const vk::render_device& dev)
		{
			// Descriptor pool + set
			std::vector<VkDescriptorPoolSize> pool_sizes =
			{
				{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 5 }
			};
			descriptor_pool.create(dev, pool_sizes.data(), ::size32(pool_sizes), 1, 1);
			descriptor_set = descriptor_pool.allocate(g_set_layout, VK_FALSE, 0);

			// Command pool + execution ring buffer
			command_pool.create(dev, dev.get_transfer_queue_family());
			command_buffer_list.create(command_pool, vk::command_buffer::all);

			// Create our memory buffers
#if SPU_DEBUG
			gpr_block = std::make_unique<vk::buffer>(dev, 256 * 16, dev.get_memory_mapping().device_bar,
				VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
				VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, 0, vk::VMM_ALLOCATION_POOL_SYSTEM);
			gpr = static_cast<SPU_GPR*>(gpr_block->map(0, 128 * 16));
#else
			gpr_block = std::make_unique<vk::buffer>(dev, 256 * 16, dev.get_memory_mapping().device_local, 0, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, 0, vk::VMM_ALLOCATION_POOL_SYSTEM);
#endif

			gpu_constants = std::make_unique<vk::buffer>(dev, 8192, dev.get_memory_mapping().device_local, 0, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, 0, vk::VMM_ALLOCATION_POOL_SYSTEM);

			ls_block = std::make_unique<vk::buffer>(dev, 256 * 1024, dev.get_memory_mapping().device_local, 0, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, 0, vk::VMM_ALLOCATION_POOL_SYSTEM);

			ls_mirror_block = std::make_unique<vk::buffer>(dev, 256 * 1024, dev.get_memory_mapping().device_bar,
				VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
				VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, 0, vk::VMM_ALLOCATION_POOL_SYSTEM);

			teb_block = std::make_unique<vk::buffer>(dev, 256, dev.get_memory_mapping().host_visible_coherent,
				VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
				VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 0, vk::VMM_ALLOCATION_POOL_SYSTEM);

			// Bind our standard interface. Only needs to be done once.
			std::vector<VkDescriptorBufferInfo> bindings;
			bindings.push_back({ .buffer = ls_block->value, .offset = 0, .range = ls_block->size() });
			bindings.push_back({ .buffer = ls_mirror_block->value, .offset = 0, .range = ls_mirror_block->size() });
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
			ls = static_cast<u8*>(ls_mirror_block->map(0, ls_mirror_block->size()));
		}

		~SPU_execution_context_t()
		{
			command_buffer_list.wait_all();

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
				ls_mirror_block->unmap();
				ls = nullptr;
			}

			if (gpr)
			{
				gpr_block->unmap();
				gpr = nullptr;
			}
		}

		void execute(spu_thread& spu, const executable& block)
		{
			// TODO: CB itself can be pre-recorded and just submitted each time
			auto prep_ssbo = [](const vk::command_buffer& cmd, const vk::buffer& buf)
			{
				vk::insert_buffer_memory_barrier(cmd, buf.value, 0, buf.size(),
					VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
					VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
					VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
			};

			u64 tstamp;
			if (is_leader)
			{
				println("begin execute...");
				tstamp = rsx::uclock();
			}

			auto cmd = command_buffer_list.next();
			cmd->begin();

			if (flush_caches)
			{
				flush_caches = false;
				do_cache_sync(*cmd, spu);
			}

			if (flush_regs)
			{
				flush_regs = false;
				do_sync_regs(*cmd, spu);
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

			if (is_leader)
			{
				const auto now = rsx::uclock();
				println("Job submitted.");
				spu_log.error("Submit took %llu us", now - tstamp);
				tstamp = now;
			}

			if (!block.is_dynamic_branch_block && !block.issues_memory_op)
			{
				//u32 next_pc = block.end_pc;
				//sync(&next_pc);
				if (is_leader)
				{
					println("Jump to next");
				}
				spu.pc = static_cast<u32>(block.end_pc);
				return;
			}

			if (is_leader)
			{
				println("Wait...");
			}

			// Wait for GPU
			sync(&teb->next_pc);

			if (is_leader)
			{
				const auto now = rsx::uclock();
				spu_log.error("GPU wait took %llu us. Current=0x%x Next=0x%x Code=%s", now - tstamp, spu.pc, teb->next_pc, static_cast<spv::exit_code>(teb->exit_code));
				tstamp = now;
			}

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

				if (is_leader)
				{
					const auto now = rsx::uclock();
					spu_log.error("Post-exit (%s) took %llu us", exit_code, now - tstamp);
				}
				return;
			}
			case spv::exit_code::MFC_CMD:
			{
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

				// TODO: Be smarter about this
				std::memcpy(spu._ptr<u8>(regs->MFC_lsa), ls + regs->MFC_lsa, regs->MFC_size);

				spu.process_mfc_cmd();

				flush_caches = true;
				flush_cache_region = { regs->MFC_lsa, regs->MFC_size };

				if (is_leader)
				{
					const auto now = rsx::uclock();
					spu_log.error("Post-exit (%s) took %llu us", exit_code, now - tstamp);
				}

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
				const spu_opcode_t op = { *spu._ptr<const be_t<u32>>(teb->dr[0]) };
				const s64 result = spu.get_ch_value(op.ra);
				spu.gpr[op.rt]._u32[3] = result;
				spu.gpr[op.rt]._u32[2] = 0;
				spu.gpr[op.rt]._u32[1] = 0;
				spu.gpr[op.rt]._u32[0] = 0;
				spu.pc = teb->next_pc + 4;

				flush_regs_region = { op.rt, 1 };
				flush_regs = true;

				if (is_leader)
				{
					const auto now = rsx::uclock();
					spu_log.error("Post-exit (%s) took %llu us", exit_code, now - tstamp);
				}
				return;
			}
			}

			// Next
			spu.pc = teb->next_pc;
		}

		void sync(volatile u32 volatile* p_next_pc)
		{
			command_buffer_list.wait_all();

#if SPU_DEBUG
			dump(*p_next_pc);
#endif
		}

		void dump(u32 next_pc)
		{
			// Dump regs
			const auto banner = fmt::format(" [%x] -- DUMP REGS --", next_pc);
			println(banner.data());
			for (int i = 0; i < 128; ++i)
			{
				const auto regstr = fmt::format("r%d %x %x %x %x (%e %e %e %e)",
					i, gpr->vgpr[i][0], gpr->vgpr[i][1], gpr->vgpr[i][2], gpr->vgpr[i][3],
					std::bit_cast<f32>(gpr->vgpr[i][0]), std::bit_cast<f32>(gpr->vgpr[i][1]),
					std::bit_cast<f32>(gpr->vgpr[i][2]), std::bit_cast<f32>(gpr->vgpr[i][3]));
				println(regstr.data());
			}

			tty_write(fmt::format("pc = 0x%x, r17 = 0x%x 0x%x 0x%x 0x%x, r85 = 0x%x 0x%x 0x%x 0x%x\n",
				next_pc, gpr->vgpr[17][0], gpr->vgpr[17][1], gpr->vgpr[17][2], gpr->vgpr[17][3],
				gpr->vgpr[85][0], gpr->vgpr[85][1], gpr->vgpr[85][2], gpr->vgpr[85][3]));
		}

		void do_cache_sync(const vk::command_buffer& cmd, spu_thread& spu)
		{
			// NOTE: We're coming in from a CPU sync, so no need for source barriers
			{
				const auto [offset, length] = flush_cache_region;

				if (length > 8192)
				{
					const VkBufferCopy region =
					{
						.srcOffset = offset,
						.dstOffset = offset,
						.size = length
					};

					std::memcpy(ls + offset, spu._ptr<u8>(offset), length);
					vkCmdCopyBuffer(cmd, ls_mirror_block->value, ls_block->value, 1, &region);
				}
				else
				{
					vkCmdUpdateBuffer(cmd, ls_block->value, offset, length, spu._ptr<u8>(offset));
				}

				// vk::insert_buffer_memory_barrier(cmd, ls_block->value, offset, length,
					// VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
					// VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
			}

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

			vkCmdUpdateBuffer(cmd, gpr_block->value, 144 * 16, sizeof(MFC_registers_t), &reg_upd);

			// vk::insert_buffer_memory_barrier(cmd, gpr_block->value, 144 * 16, 48,
				// VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
				// VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
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

		void do_sync_regs(const vk::command_buffer& cmd, const spu_thread& spu)
		{
			const auto [offset, count] = flush_regs_region;
			vkCmdUpdateBuffer(cmd, gpr_block->value, offset * 16, count * 16, &spu.gpr[offset]._u32[0]);

			vk::insert_buffer_memory_barrier(cmd, gpr_block->value, offset * 16, count * 16,
				VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
				VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
		}
	};

	std::unordered_map<u32, std::unique_ptr<SPU_execution_context_t>> g_SPU_context;
	std::unordered_map<u64, std::unique_ptr<executable>> g_compiled_blocks;
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
			{
				.binding = 4,
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

		spv::executable* block = nullptr;
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

void spv_recompiler::compile_block(const spu_program& func)
{
	m_pos = func.lower_bound;
	m_size += ::size32(func.data) * 4;
	const u32 start = m_pos;

	m_jump_targets.push_back(start);

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

		// Verify that we're not trampling ourselves
		if (!c.push_label(pos))
		{
			spu_log.error("Circled back to our code....");
		}

		// Execute recompiler function
		(this->*s_spu_decoder.decode(op))({ op });

		if (m_pos == umax || i == func.data.size() - 1)
		{
#if !SPU_DEBUG
			// Is this the end?
			const auto new_pos = m_pos == umax ? c.get_pc() : m_pos + 4;
			if (!c.has_memory_dependency() && new_pos != umax)
			{
				if (m_jump_targets.size() < 16 && !m_jump_targets.includes(new_pos))
				{
					// Merge
					spu_program next_func = analyse(func.ls, new_pos);
					compile_block(next_func);
				}
			}
#endif

			break;
		}
	}
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

	if (false)
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
	else
	{
		m_hash_start = func.lower_bound;
	}

	// Clear
	c.reset();

	m_size = 0;
	m_jump_targets.clear();

	compile_block(func);

	// Compile and get function address
	auto compiled = c.assemble(spv::g_pipeline_layout);
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
	// TODO
	c.s_movsi(c.s_tmp0, spv::constants::make_si(op.opcode));
	c.s_storsr(c.s_dr0, c.s_tmp0);
	c.s_movsi(c.s_tmp0, spv::constants::make_si(0));
	c.s_brz(spv::constants::make_si(m_pos + 4), c.s_tmp0, spv::exit_code::STOP_AND_SIGNAL);
	c.signal_mem();
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
		c.s_movsi(c.s_tmp0, spv::constants::make_si(m_pos));
		c.s_storsr(c.s_dr0, c.s_tmp0);
		c.s_movsi(c.s_tmp0, spv::constants::make_si(0));
		c.s_brz(spv::constants::make_su(m_pos + 4), c.s_tmp0, spv::exit_code::RDCH3);
		c.signal_mem();
		m_pos = -1;
		return;
	}
	case MFC_RdTagStat:
	{
		c.s_loadsr(c.s_tmp0, c.mfc.ch_tag_stat_count);
		c.s_brz(spv::constants::make_su(m_pos), c.s_tmp0, spv::exit_code::HLT); // TODO
		c.s_loadsr(c.s_tmp1, c.mfc.ch_tag_stat_value);
		c.v_movsi(op.rt, spv::constants::spread(0));
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
	c.v_cmpgtu(c.v_tmp0, op.ra, op.rb);
	c.v_xori(op.rt, c.v_tmp0, spv::constants::spread(-1));
}

void spv_recompiler::SFH(spu_opcode_t op)
{
	c.unimplemented("SFH");
}

void spv_recompiler::NOR(spu_opcode_t op)
{
	if (op.ra == op.rb)
	{
		// NOT
		c.v_xori(op.rt, op.ra, spv::constants::spread(0xffffffff).as_vi());
		return;
	}

	c.v_or(c.v_tmp0, op.ra, op.rb);
	c.v_xori(op.rt, c.v_tmp0, spv::constants::spread(0xffffffff).as_vi());
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
	const auto mask = spv::constants::spread(0x3f);
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
	c.v_shri(op.rt, op.ra, spv::constants::spread((0 - op.i7) & 0x3f).as_vi());
}

void spv_recompiler::ROTMAI(spu_opcode_t op)
{
	c.unimplemented("ROTMAI");
}

void spv_recompiler::SHLI(spu_opcode_t op)
{
	const auto distance = spv::constants::spread(static_cast<s32>(op.i7 & 0x3f));
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
	c.v_and(op.rt, op.ra, op.rb);
}

void spv_recompiler::CG(spu_opcode_t op)
{
	c.v_xori(c.v_tmp0, op.ra, spv::constants::spread(0x7fffffff).as_vi());
	c.v_xori(c.v_tmp1, op.rb, spv::constants::spread(0x80000000).as_vi());
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
		c.s_andi(c.s_tmp0, c.s_tmp0, spv::constants::make_si(0x3fffc));
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
		c.s_movsi(c.s_tmp0, spv::constants::make_si(m_pos));
		c.s_storsr(c.s_dr0, c.s_tmp0);
		c.s_movsi(c.s_tmp0, spv::constants::make_si(0));
		c.s_brz(spv::constants::make_si(m_pos + 4), c.s_tmp0, spv::exit_code::HLT);
		c.signal_mem();
		m_pos = -1;
		return;
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
		c.s_andi(c.s_tmp1, c.s_tmp1, spv::constants::make_si(0x1f));
		c.s_storsr(c.mfc.cmd_tag_id, c.s_tmp1);
		return;
	}

	case MFC_Cmd:
	{
		c.s_xtr(c.s_tmp1, op.rt, 3);
		c.s_andi(c.s_tmp1, c.s_tmp1, spv::constants::make_si(0xff));
		c.s_storsr(c.mfc.cmd, c.s_tmp1);
		c.s_call("MFC_cmd");
		// TODO: Handle on GPU side
		c.s_bri(spv::constants::make_si(m_pos + 4));
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
	c.s_andi(c.s_tmp0, c.s_tmp0, spv::constants::make_si(0x3fff0));
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
	c.v_movsi(op.rt, spv::constants::make_rvi(spu_branch_target(m_pos + 4)));
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
	c.v_shli(c.v_tmp0, op.ra, spv::constants::make_vi(24, 25, 26, 27));
	c.v_andi(c.v_tmp0, c.v_tmp0, spv::constants::make_vi(1 << 24, 1 << 25, 1 << 26, 1 << 27));
	c.s_hzor(c.s_tmp0, c.v_tmp0); // Horizontal sum = horizontal or
	c.v_movsi(op.rt, spv::constants::spread(0));
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
	c.v_rcpf(c.v_tmp0, op.ra);
	c.v_andi(op.rt, c.v_tmp0, spv::constants::spread(0xfffff000).as_vi());
}

void spv_recompiler::FRSQEST(spu_opcode_t op)
{
	c.v_andi(c.v_tmp0, op.ra, spv::constants::spread(0x7fffffff).as_vi());
	c.v_rsqf(op.rt, c.v_tmp0);
}

void spv_recompiler::LQX(spu_opcode_t op)
{
	c.s_xtr(c.s_tmp0, op.ra, 3);
	c.s_xtr(c.s_tmp1, op.rb, 3);
	c.s_adds(c.s_tmp0, c.s_tmp0, c.s_tmp1);
	c.s_andi(c.s_tmp0, c.s_tmp0, spv::constants::make_si(0x3fff0));
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
	c.s_andi(c.s_tmp0, c.s_tmp0, spv::constants::make_si(15));
	c.s_shli(c.s_tmp0, c.s_tmp0, spv::constants::make_si(3));   // Convert bytes to bits
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
	c.s_addsi(c.s_tmp0, c.s_tmp0, spv::constants::make_si(op.i7));
	c.s_xori(c.s_tmp0, c.s_tmp0, spv::constants::make_si(-1));
	c.s_andi(c.s_tmp0, c.s_tmp0, spv::constants::make_si(0xc));
	c.s_shri(c.s_tmp0, c.s_tmp0, spv::constants::make_si(2));
	c.s_ins(op.rt, spv::constants::make_si(0x00010203), c.s_tmp0);
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
	c.s_movsi(c.s_tmp0, spv::constants::make_si(shift_distance));
	c.q_rotr(op.rt, op.ra, c.s_tmp0);
}

void spv_recompiler::ROTQMBYI(spu_opcode_t op)
{
	// TODO: Ellide mov
	const auto shift_distance = ((0 - int(op.i7)) & 0xf) << 3;
	c.s_movsi(c.s_tmp0, spv::constants::make_si(shift_distance));
	c.q_shl(op.rt, op.ra, c.s_tmp0);
}

void spv_recompiler::SHLQBYI(spu_opcode_t op)
{
	const auto shift_distance = (op.i7 & 0xf) << 3;
	c.s_movsi(c.s_tmp0, spv::constants::make_si(shift_distance));
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
	c.v_bfxi(c.v_tmp0, op.ra, spv::constants::make_si(7), spv::constants::make_si(1));
	c.v_bfxi(c.v_tmp1, op.ra, spv::constants::make_si(23), spv::constants::make_si(1));
	c.v_mulsi(c.v_tmp0, c.v_tmp0, spv::constants::spread(0x0000ff00).as_vi());
	c.v_mulsi(c.v_tmp1, c.v_tmp1, spv::constants::spread(0xff000000).as_vi());
	c.v_andi(c.v_tmp2, op.ra, spv::constants::spread(0x00ff00ff));
	c.v_or(c.v_tmp1, c.v_tmp1, c.v_tmp2);
	c.v_or(op.rt, c.v_tmp0, c.v_tmp1);
}

void spv_recompiler::CLGT(spu_opcode_t op)
{
	const auto all_ones = spv::constants::spread(0xffffffff).as_vi();
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
	c.v_mulsi(op.rt, c.v_tmp0, spv::constants::spread(0xffffffff).as_vi());
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
	c.v_andi(c.v_tmp0, op.rt, spv::constants::spread(1));
	c.v_adds(c.v_tmp0, c.v_tmp0, op.ra);
	c.v_adds(op.rt, c.v_tmp0, op.rb);
}

void spv_recompiler::SFX(spu_opcode_t op)
{
	c.v_xori(c.v_tmp0, op.rt, spv::constants::spread(-1));
	c.v_andi(c.v_tmp0, c.v_tmp0, spv::constants::spread(1));
	c.v_subs(c.v_tmp1, op.rb, op.ra);
	c.v_subs(op.rt, c.v_tmp1, c.v_tmp0);
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
	c.v_andi(c.v_tmp0, op.rb, spv::constants::spread(0x0000ffff));
	c.v_shri(c.v_tmp1, op.ra, spv::constants::spread(16));
	c.v_muls(c.v_tmp0, c.v_tmp0, c.v_tmp1);
	c.v_shri(op.rt, c.v_tmp0, spv::constants::spread(16));
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
	c.v_andi(c.v_tmp0, op.ra, spv::constants::spread(0x0000ffff));
	c.v_andi(c.v_tmp1, op.rb, spv::constants::spread(0x0000ffff));
	c.v_mulu(op.rt, c.v_tmp0, c.v_tmp1);
}

void spv_recompiler::CEQB(spu_opcode_t op)
{
	c.unimplemented("CEQB");
}

void spv_recompiler::FI(spu_opcode_t op)
{
	// TODO
	const auto mask_se = spv::constants::spread(0xff800000).as_vi(); // sign and exponent mask
	const auto mask_bf = spv::constants::spread(0x007ffc00).as_vi(); // base fraction mask
	const auto mask_sf = spv::constants::spread(0x000003ff).as_vi(); // step fraction mask
	const auto mask_yf = spv::constants::spread(0x0007ffff).as_vi(); // Y fraction mask (bits 13..31)

	c.v_andi(c.v_tmp0, op.rb, mask_bf);
	c.v_ori(c.v_tmp0, c.v_tmp0, spv::constants::spread(0x3f800000)); // base
	c.v_andi(c.v_tmp1, op.rb, mask_sf);
	c.v_mulfi(c.v_tmp1, c.v_tmp1, spv::constants::spread(std::exp2(-13.f))); // step
	c.v_andi(c.v_tmp2, op.ra, mask_yf);
	c.v_mulfi(c.v_tmp2, c.v_tmp2, spv::constants::spread(std::exp2(-19.f))); // y

	c.v_mulf(c.v_tmp1, c.v_tmp1, c.v_tmp2); // step * y
	c.v_subf(c.v_tmp2, c.v_tmp0, c.v_tmp1); // base - step * y
	c.v_andi(c.v_tmp2, c.v_tmp2, spv::constants::spread(~mask_se.value.i[0])); // ~mask_se & (base - step * y)
	c.v_andi(c.v_tmp1, op.rb, mask_se);     // b & se
	c.v_or(op.rt, c.v_tmp1, c.v_tmp2);      // OR((b & se), ~se & (base - step * y));
}

void spv_recompiler::HEQ(spu_opcode_t op)
{
	const auto value = spv::constants::make_si(op.si10);
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
	c.v_mulfi(c.v_tmp0, op.ra, spv::constants::spread(scale));
	c.v_clampfi(c.v_tmp0, c.v_tmp0, spv::constants::spread(0.f), spv::constants::spread(4294967295.f));
	c.v_fcvtu(op.rt, c.v_tmp0);
}

void spv_recompiler::CSFLT(spu_opcode_t op)
{
	const auto scale = g_spu_imm.scale[op.i8 - 155]._f[0];
	c.v_scvtf(c.v_tmp0, op.ra);
	c.v_mulfi(op.rt, c.v_tmp0, spv::constants::spread(scale));
}

void spv_recompiler::CUFLT(spu_opcode_t op)
{
	const auto scale = g_spu_imm.scale[op.i8 - 155]._f[0];
	c.v_ucvtf(c.v_tmp0, op.ra);
	c.v_mulfi(op.rt, c.v_tmp0, spv::constants::spread(scale));
}

void spv_recompiler::BRZ(spu_opcode_t op)
{
	const u32 target = spu_branch_target(m_pos, op.i16);
	if (target == m_pos + 4)
	{
		return;
	}

	c.s_xtr(c.s_tmp0, op.rt, 3);
	c.s_brz(spv::constants::make_su(target), c.s_tmp0);

	c.s_bri(spv::constants::make_si(m_pos + 4));
	m_pos = -1; // TODO
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
	c.s_brnz(spv::constants::make_su(target), c.s_tmp0);

	c.s_bri(spv::constants::make_si(m_pos + 4));
	m_pos = -1; // TODO
}

void spv_recompiler::BRHZ(spu_opcode_t op)
{
	const u32 target = spu_branch_target(m_pos, op.i16);
	if (target == m_pos + 4)
	{
		return;
	}

	c.s_xtr(c.s_tmp0, op.rt, 3);
	c.s_andi(c.s_tmp0, c.s_tmp0, spv::constants::make_si(0xffff));
	c.s_brz(spv::constants::make_su(target), c.s_tmp0);

	c.s_bri(spv::constants::make_si(m_pos + 4));
	m_pos = -1; // TODO
}

void spv_recompiler::BRHNZ(spu_opcode_t op)
{
	c.unimplemented("BRHNZ");
}

void spv_recompiler::STQR(spu_opcode_t op)
{
	const auto lsa = spu_ls_target(m_pos, op.i16);
	c.v_storq(spv::constants::make_su(lsa), op.rt);
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
	c.s_bri(spv::constants::make_su(target));
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

	c.v_movsi(op.rt, spv::constants::make_rvi(static_cast<s32>(lr)));

	if (target != m_pos + 4)
	{
		// Fall out
		c.s_bri(spv::constants::make_su(target));
		m_pos = -1;
	}
}

void spv_recompiler::LQR(spu_opcode_t op)
{
	const auto address = spv::constants::make_su(spu_ls_target(m_pos, op.i16));
	c.v_loadq(op.rt, address);
}

void spv_recompiler::IL(spu_opcode_t op)
{
	const auto var = spv::constants::spread(op.si16);
	c.v_movsi(op.rt, var);
}

void spv_recompiler::ILHU(spu_opcode_t op)
{
	c.v_movsi(op.rt, spv::constants::spread(op.i16 << 16).as_vi());
}

void spv_recompiler::ILH(spu_opcode_t op)
{
	c.unimplemented("ILH");
}

void spv_recompiler::IOHL(spu_opcode_t op)
{
	c.v_ori(op.rt, op.rt, spv::constants::spread(op.i16).as_vi());
}

void spv_recompiler::ORI(spu_opcode_t op)
{
	if (op.si10 == 0)
	{
		// MR
		c.v_movs(op.rt, op.ra);
		return;
	}

	const auto var = spv::constants::spread(op.si10);
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
	c.v_subsi(op.rt, spv::constants::spread(op.si10), op.ra);
}

void spv_recompiler::SFHI(spu_opcode_t op)
{
	c.unimplemented("SFHI");
}

void spv_recompiler::ANDI(spu_opcode_t op)
{
	c.v_andi(op.rt, op.ra, spv::constants::spread(op.si10));
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
	c.v_addsi(op.rt, op.ra, spv::constants::spread(op.si10));
}

void spv_recompiler::AHI(spu_opcode_t op)
{
	c.unimplemented("AHI");
}

void spv_recompiler::STQD(spu_opcode_t op)
{
	const auto si10 = spv::constants::make_si(op.si10 * 16);
	const auto address_mask = spv::constants::make_si(0x3fff0);
	c.s_xtr(c.s_tmp0, op.ra, 3);
	c.s_addsi(c.s_tmp0, c.s_tmp0, si10);
	c.s_andi(c.s_tmp0, c.s_tmp0, address_mask);
	c.v_storq(c.s_tmp0, op.rt);
}

void spv_recompiler::LQD(spu_opcode_t op)
{
	const auto offset = spv::constants::make_si(op.si10 * 16);
	const auto address_mask = spv::constants::make_si(0x3fff0);
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
	const auto imm = spv::constants::spread(op.si10);
	const auto all_ones = spv::constants::spread(-1);
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
	const auto imm = spv::constants::spread(op.si10);
	const auto all_ones = spv::constants::spread(-1);
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
	const auto set1 = spv::constants::spread(value << 0);
	const auto set2 = spv::constants::spread(value << 8);
	const auto set3 = spv::constants::spread(value << 16);
	const auto set4 = spv::constants::spread(value << 24);
	const auto mask1 = spv::constants::spread(0xff << 0);
	const auto mask2 = spv::constants::spread(0xff << 8);
	const auto mask3 = spv::constants::spread(0xff << 16);
	const auto mask4 = spv::constants::spread(0xff << 24);

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
	const auto value = spv::constants::make_si(op.si10);
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
	const auto var = spv::constants::spread(op.i18).as_vi();
	c.v_movsi(op.rt, var);
}

void spv_recompiler::SELB(spu_opcode_t op)
{
	c.v_xori(c.v_tmp0, op.rc, spv::constants::spread(0xffffffff).as_vi());
	c.v_and(c.v_tmp1, op.rc, op.rb);
	c.v_and(c.v_tmp0, c.v_tmp0, op.ra);
	c.v_or(op.rt4, c.v_tmp0, c.v_tmp1);
}

void spv_recompiler::SHUFB(spu_opcode_t op)
{
	// TODO: If we already know the CW, we can redirect to shufw which is much faster
	c.q_shufb(op.rt4, op.ra, op.rb, op.rc);
}

void spv_recompiler::MPYA(spu_opcode_t op)
{
	// RT4!
	c.unimplemented("MPYA");
}

void spv_recompiler::FNMS(spu_opcode_t op)
{
	// TODO: xfloat
	// TODO: native FMA
	c.v_mulf(c.v_tmp0, op.ra, op.rb);
	c.v_subf(op.rt4, op.rc, c.v_tmp0);
}

void spv_recompiler::FMA(spu_opcode_t op)
{
	// TODO: xfloat
	// TODO: native FMA
	c.v_mulf(c.v_tmp0, op.ra, op.rb);
	c.v_addf(op.rt4, op.rc, c.v_tmp0);
}

void spv_recompiler::FMS(spu_opcode_t op)
{
	// RT4!
	c.unimplemented("FMS");
}
