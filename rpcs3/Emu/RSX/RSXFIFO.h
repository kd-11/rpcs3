#pragma once

#include "util/types.hpp"
#include "Emu/RSX/gcm_enums.h"

#include <span>

struct RsxDmaControl;

namespace rsx
{
	class thread;
	struct rsx_iomap_table;

	namespace FIFO
	{
		enum internal_commands : u32
		{
			FIFO_NOP = 0xBABEF1F4,
			FIFO_EMPTY = 0xDEADF1F0,
			FIFO_BUSY = 0xBABEF1F0,
			FIFO_ERROR = 0xDEADBEEF,
			FIFO_PACKET_BEGIN = 0xF1F0,
			FIFO_DISABLED_COMMAND = 0xF1F4,
			FIFO_DRAW_BARRIER = 0xF1F8,
		};

		enum flatten_op : u32
		{
			NOTHING = 0,
			EMIT_END = 1,
			EMIT_BARRIER = 2
		};

		enum class state : u8
		{
			running = 0,
			empty = 1,    // PUT == GET
			spinning = 2, // Puller continuously jumps to self addr (synchronization technique)
			nop = 3,      // Puller is processing a NOP command
			lock_wait = 4,// Puller is processing a lock acquire
			paused = 5,   // Puller is paused externallly
		};

		enum class interrupt_hint : u8
		{
			conditional_render_eval = 1,
			zcull_sync = 2
		};

		struct register_pair
		{
			u32 reg;
			u32 value;

			void set(u32 reg, u32 val)
			{
				this->reg = reg;
				this->value = val;
			}
		};

		class flattening_helper
		{
			enum register_props : u8
			{
				none          = 0,
				continue_draw = 1, // Do not break current draw call
				force_nop     = 2, // Rewrite command with NOP
				deduplicate   = 4, // Skip command if it is a duplicate
			};

			enum optimization_hint : u8
			{
				unknown,
				load_low,
				load_unoptimizable,
				application_not_compatible
			};

			static constexpr std::array<u8, 0x10000 / 4> m_register_properties = []
			{
				constexpr std::array<std::pair<u32, u32>, 4> ignorable_ranges =
				{{
					// General
					{ NV4097_INVALIDATE_VERTEX_FILE, 3 }, // PSLight clears VERTEX_FILE[0-2]
					{ NV4097_INVALIDATE_VERTEX_CACHE_FILE, 1 },
					{ NV4097_INVALIDATE_L2, 1 },
					{ NV4097_INVALIDATE_ZCULL, 1 }
				}};

				std::array<u8, 0x10000 / 4> register_properties{};

				const auto fill_range = [&register_properties](u32 index, u32 count, u8 flags) -> void
				{
					for (u32 i = 0; i < count; ++i)
					{
						register_properties[index + i] |= flags;
					}
				};

				for (const auto &method : ignorable_ranges)
				{
					fill_range(method.first, method.second, register_props::force_nop);
				}

				// Instancing detection helpers
				register_properties[NV4097_SET_TRANSFORM_CONSTANT_LOAD] = register_props::continue_draw; // No break
				fill_range(NV4097_SET_TRANSFORM_CONSTANT, 32, register_props::continue_draw);
				register_properties[NV4097_SET_INDEX_ARRAY_ADDRESS] = register_props::deduplicate;
				register_properties[NV4097_SET_INDEX_ARRAY_DMA] = register_props::deduplicate;

				// Vertex config
				register_properties[NV4097_SET_VERTEX_DATA_BASE_INDEX] = register_props::deduplicate;
				register_properties[NV4097_SET_VERTEX_DATA_BASE_OFFSET] = register_props::deduplicate;
				fill_range(NV4097_SET_VERTEX_DATA_ARRAY_FORMAT, 16, register_props::deduplicate);

				return register_properties;
			}();

			u32 deferred_primitive = 0;
			u32 draw_count = 0;
			bool in_begin_end = false;

			bool enabled = false;
			u32  num_collapsed = 0;
			optimization_hint fifo_hint = unknown;

			void reset(bool _enabled);

		public:
			flattening_helper() = default;
			~flattening_helper() = default;

			u32 get_primitive() const { return deferred_primitive; }
			bool is_enabled() const { return enabled; }

			void force_disable();
			void evaluate_performance(u32 total_draw_count);
			flatten_op test(register_pair& command, const std::array<u32, 0x10000 / 4>& register_shadow);
		};

		class FIFO_control
		{
		private:
			mutable rsx::thread* m_thread;
			RsxDmaControl* m_ctrl = nullptr;
			const rsx::rsx_iomap_table* m_iotable;
			u32 m_internal_get = 0;

			u32 m_memwatch_addr = 0;
			u32 m_memwatch_cmp = 0;

			u32 m_command_reg = 0;
			u32 m_command_inc = 0;
			u32 m_remaining_commands = 0;
			u32 m_args_ptr = 0;
			u32 m_cmd = ~0u;

			u32 m_cache_addr = 0;
			u32 m_cache_size = 0;
			alignas(64) std::byte m_cache[8][128];

		public:
			FIFO_control(rsx::thread* pctrl);
			~FIFO_control() = default;

			u32 translate_address(u32 addr) const;

			std::pair<bool, u32> fetch_u32(u32 addr);
			void invalidate_cache() { m_cache_size = 0; }

			u32 get_pos() const { return m_internal_get; }
			u32 last_cmd() const { return m_cmd; }
			void sync_get() const;
			std::span<const u32> get_current_arg_ptr(u32 length_in_words) const;
			u32 get_remaining_args_count() const { return m_remaining_commands; }
			void restore_state(u32 cmd, u32 count);
			void inc_get(bool wait);

			void set_get(u32 get, u32 spin_cmd = 0);
			void abort();

			template <bool = true>
			u32 read_put() const;

			void read(register_pair& data);
			inline bool read_unsafe(register_pair& data);
			bool skip_methods(u32 count);
		};
	}
}
