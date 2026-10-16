#include "stdafx.h"
#include <gtest/gtest.h>
#include "Emu/Cell/Modules/cellDmuxPamf.h"
#include "util/serialization.hpp"

// The queues between the PPU thread and the HLE SPU thread are located on the host, and the access unit queue sizes are set by the guest

namespace SysModules
{
	namespace
	{
		template <u32 N>
		using cmd_queue_t = dmux_pamf_hle_spurs_queue<DmuxPamfCommand, N>;

		template <u32 N>
		void test_fifo()
		{
			cmd_queue_t<N> queue;
			EXPECT_EQ(queue.size(), 0u);

			DmuxPamfCommand cmd{};
			EXPECT_FALSE(queue.peek(cmd));
			EXPECT_FALSE(queue.pop(cmd));

			// Fill and empty the queue several times so that the indices wrap around
			for (u32 round = 0; round < 3; round++)
			{
				for (u32 i = 0; i < N; i++)
				{
					ASSERT_TRUE(queue.emplace(DmuxPamfCommandType::flush_es, be_t<u32>{round * 1000 + i}, be_t<u32>{0}));
				}

				EXPECT_EQ(queue.size(), N);
				EXPECT_FALSE(queue.emplace(DmuxPamfCommandType::flush_es, be_t<u32>{0}, be_t<u32>{0}));

				for (u32 i = 0; i < N; i++)
				{
					ASSERT_TRUE(queue.peek(cmd));
					EXPECT_EQ(+cmd.disable_flush_es.stream_id, round * 1000 + i);
					ASSERT_TRUE(queue.pop(cmd));
					EXPECT_EQ(+cmd.disable_flush_es.stream_id, round * 1000 + i);
				}

				EXPECT_EQ(queue.size(), 0u);
			}
		}

		struct DmuxPamfFake : dmux_pamf_base
		{
			u32 fatal_errors = 0;

		private:
			bool on_au_found(u8, u8, u32, std::span<const u8>, u64, u64, bool, u8, std::array<u8, 0x10>) override { return true; }
			bool on_demux_done() override { return true; }
			void on_fatal_error() override { fatal_errors++; }
			bool on_flush_done(u8, u8, u32) override { return true; }
			bool on_prog_end() override { return true; }
			void on_au_queue_full() override {}
		};
	}

	TEST(DmuxPamfHleQueue, Fifo)
	{
		test_fifo<1>();
		test_fifo<4 + 2 * DMUX_PAMF_MAX_ENABLED_ES_NUM>();
	}

	TEST(DmuxPamfHleQueue, AbortWakesWaiters)
	{
		cmd_queue_t<1> queue;
		queue.abort();
		queue.wait(); // Returns immediately even though the queue is empty

		DmuxPamfCommand cmd{};
		EXPECT_FALSE(queue.pop(cmd));
	}

	TEST(DmuxPamfHleQueue, SaveLoad)
	{
		cmd_queue_t<4> queue;
		ASSERT_TRUE(queue.emplace(DmuxPamfCommandType::flush_es, be_t<u32>{1}, be_t<u32>{0}));
		ASSERT_TRUE(queue.emplace(DmuxPamfCommandType::flush_es, be_t<u32>{2}, be_t<u32>{0}));
		ASSERT_TRUE(queue.emplace(DmuxPamfCommandType::flush_es, be_t<u32>{3}, be_t<u32>{0}));

		DmuxPamfCommand cmd{};
		ASSERT_TRUE(queue.pop(cmd));

		utils::serial ar;
		queue.save(ar);
		ar.set_reading_state();

		cmd_queue_t<4> loaded;
		loaded.load(ar);

		EXPECT_EQ(loaded.size(), 2u);
		ASSERT_TRUE(loaded.pop(cmd));
		EXPECT_EQ(+cmd.disable_flush_es.stream_id, 2u);
		ASSERT_TRUE(loaded.pop(cmd));
		EXPECT_EQ(+cmd.disable_flush_es.stream_id, 3u);
		EXPECT_FALSE(loaded.pop(cmd));
	}

	TEST(DmuxPamfHleQueue, LoadRejectsCorruptedState)
	{
		struct corrupted_state
		{
			u32 front;
			u32 back;
			u32 size;
		};

		for (const corrupted_state state : { corrupted_state{ 4, 0, 0 }, { 0, 4, 0 }, { 0, 0, 5 }, { 1, 1, 1 }, { 0xffffffff, 0xffffffff, 0xffffffff }, { 0, 0, 0x80000000 } })
		{
			utils::serial ar;
			ar(cmd_queue_t<4>::buffer_type{}, state.front, state.back, state.size);
			ar.set_reading_state();

			cmd_queue_t<4> loaded;
			loaded.load(ar);

			EXPECT_EQ(loaded.size(), 0u) << "front=" << state.front << ", back=" << state.back << ", size=" << state.size;

			DmuxPamfCommand cmd{};
			EXPECT_FALSE(loaded.pop(cmd));

			// The queue is usable afterwards
			ASSERT_TRUE(loaded.emplace(DmuxPamfCommandType::flush_es, be_t<u32>{7}, be_t<u32>{0}));
			ASSERT_TRUE(loaded.pop(cmd));
			EXPECT_EQ(+cmd.disable_flush_es.stream_id, 7u);
		}
	}

	TEST(DmuxPamfOutputQueue, InvalidReleaseAndReset)
	{
		DmuxPamfFake demuxer;
		std::array<u8, 0x3000> au_queue_buffer{};

		ASSERT_TRUE(demuxer.enable_es(0xe0, 0x00, false, au_queue_buffer, 0x800, false, 0));

		// The access unit size is set by the user, an invalid size raises a fatal error instead of modifying the queue
		EXPECT_TRUE(demuxer.release_au(0xe0, 0x00, 0x3001));
		EXPECT_EQ(demuxer.fatal_errors, 1u);
		EXPECT_TRUE(demuxer.release_au(0xe0, 0x00, 0xffffffff));
		EXPECT_EQ(demuxer.fatal_errors, 2u);

		EXPECT_TRUE(demuxer.release_au(0xe0, 0x00, 0));
		EXPECT_EQ(demuxer.fatal_errors, 2u);

		EXPECT_TRUE(demuxer.reset_es(0xe0, 0x00, au_queue_buffer.data() + au_queue_buffer.size()));
		EXPECT_EQ(demuxer.fatal_errors, 3u);

		EXPECT_TRUE(demuxer.reset_es(0xe0, 0x00, au_queue_buffer.data()));
		EXPECT_EQ(demuxer.fatal_errors, 3u);

		// Not enabled
		EXPECT_FALSE(demuxer.release_au(0xe1, 0x00, 0x10));
		EXPECT_EQ(demuxer.fatal_errors, 3u);
	}

	TEST(DmuxPamf, GuestLayout)
	{
		// The queues were moved to the host without changing the layout of the context
		EXPECT_EQ(sizeof(DmuxPamfQueueReserved), 0x80u);
		EXPECT_EQ(alignof(DmuxPamfQueueReserved), 0x80u);
		EXPECT_EQ(sizeof(DmuxPamfContext), 0x3d880u);
	}
}
