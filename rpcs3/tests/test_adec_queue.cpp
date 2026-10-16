#include "stdafx.h"
#include <gtest/gtest.h>
#include "Emu/Cell/PPUModule.h" // cellAdec.h is not self-contained
#include "Emu/Cell/Modules/cellAtracXdec.h"

#include <climits>

// AdecCmdQueue is located in guest memory, the guest can modify any of its members between and during operations

namespace SysModules
{
	// Helpers have internal linkage
	namespace
	{
		template <typename T>
		struct cmd_traits;

		template <>
		struct cmd_traits<LpcmDecCmd>
		{
			static constexpr LpcmDecCmdType decode_au() { return LpcmDecCmdType::decode_au; }
		};

		template <>
		struct cmd_traits<AtracXdecCmd>
		{
			static constexpr AtracXdecCmdType decode_au() { return AtracXdecCmdType::decode_au; }
		};

		template <typename T>
		bool emplace_tagged(AdecCmdQueue<T>& queue, s32 tag)
		{
			CellAdecAuInfo au_info{};
			au_info.size = static_cast<u32>(tag);
			return queue.emplace(cmd_traits<T>::decode_au(), tag, au_info);
		}

		// Surrounds the queue with canaries to detect accesses outside of the queue
		template <typename T>
		struct guarded_queue
		{
			static constexpr u8 canary = 0xcd;

			std::array<u8, 0x100> guard_before;
			AdecCmdQueue<T> queue{};
			std::array<u8, 0x100> guard_after;

			guarded_queue()
			{
				guard_before.fill(canary);
				guard_after.fill(canary);
			}

			bool guards_intact() const
			{
				return std::ranges::all_of(guard_before, [](u8 v) { return v == canary; }) && std::ranges::all_of(guard_after, [](u8 v) { return v == canary; });
			}

			// Indices are only normalized when they are used, so an index written by the guest stays as is until the next operation using it
			bool size_valid() const
			{
				return queue.size >= 0 && queue.size <= 4;
			}
		};

		template <typename T>
		void test_fifo_order()
		{
			AdecCmdQueue<T> queue{};
			EXPECT_TRUE(queue.empty());
			EXPECT_FALSE(queue.full());

			for (s32 i = 0; i < 4; i++)
			{
				EXPECT_TRUE(emplace_tagged(queue, 100 + i));
			}

			EXPECT_TRUE(queue.full());
			EXPECT_FALSE(emplace_tagged(queue, 999)); // Full queue is not modified
			EXPECT_EQ(+queue.size, 4);

			for (s32 i = 0; i < 4; i++)
			{
				T cmd{};
				ASSERT_TRUE(queue.pop(cmd));
				EXPECT_EQ(+cmd.pcm_handle, 100 + i);
				EXPECT_EQ(+cmd.au_size, static_cast<u32>(100 + i));
			}

			EXPECT_TRUE(queue.empty());

			T cmd{};
			EXPECT_FALSE(queue.pop(cmd));
		}

		template <typename T>
		void test_wrap_around()
		{
			AdecCmdQueue<T> queue{};
			s32 next_in = 0;
			s32 next_out = 0;

			// Alternate between one and three queued elements so that the indices wrap around many times
			for (u32 i = 0; i < 1000; i++)
			{
				while (queue.size < (i % 2 ? 3 : 1))
				{
					ASSERT_TRUE(emplace_tagged(queue, next_in++));
				}

				T cmd{};
				ASSERT_TRUE(queue.pop(cmd));
				EXPECT_EQ(+cmd.pcm_handle, next_out++);
				ASSERT_GE(+queue.front, 0);
				ASSERT_LT(+queue.front, 4);
				ASSERT_GE(+queue.back, 0);
				ASSERT_LT(+queue.back, 4);
			}
		}

		template <typename T>
		void test_pop_marks_slot()
		{
			AdecCmdQueue<T> queue{};
			ASSERT_TRUE(emplace_tagged(queue, 7));

			T cmd{};
			ASSERT_TRUE(queue.pop(cmd));
			EXPECT_EQ(+cmd.pcm_handle, 7);
			EXPECT_EQ(+queue.elements[0].pcm_handle, 0xff);
		}

		template <typename T>
		void test_corrupted_indices()
		{
			for (const s32 index : { INT_MIN, INT_MIN + 1, -5, -1, 4, 5, 0x7ffffffd, INT_MAX })
			{
				const u32 slot = static_cast<u32>(index) & 3;

				// Corrupted front
				{
					guarded_queue<T> q;

					for (s32 i = 0; i < 4; i++)
					{
						ASSERT_TRUE(emplace_tagged(q.queue, i));
					}

					q.queue.front = index;

					T cmd{};
					ASSERT_TRUE(q.queue.pop(cmd));
					EXPECT_EQ(+cmd.pcm_handle, static_cast<s32>(slot));
					EXPECT_EQ(+q.queue.front, static_cast<s32>((slot + 1) & 3));
					EXPECT_EQ(+q.queue.size, 3);
					EXPECT_TRUE(q.guards_intact());
				}

				// Corrupted back
				{
					guarded_queue<T> q;
					q.queue.back = index;

					ASSERT_TRUE(emplace_tagged(q.queue, 42));
					EXPECT_EQ(+q.queue.elements[slot].pcm_handle, 42);
					EXPECT_EQ(+q.queue.back, static_cast<s32>((slot + 1) & 3));
					EXPECT_EQ(+q.queue.size, 1);
					EXPECT_TRUE(q.guards_intact());
				}
			}
		}

		template <typename T>
		void test_corrupted_size()
		{
			for (const s32 size : { INT_MIN, -1, 5, 0x10000, INT_MAX })
			{
				guarded_queue<T> q;

				ASSERT_TRUE(emplace_tagged(q.queue, 1));
				ASSERT_TRUE(emplace_tagged(q.queue, 2));
				q.queue.size = size;

				// An invalid size resets the queue
				EXPECT_TRUE(q.queue.empty());
				EXPECT_EQ(+q.queue.front, 0);
				EXPECT_EQ(+q.queue.back, 0);
				EXPECT_EQ(+q.queue.size, 0);

				q.queue.size = size;

				T cmd{};
				EXPECT_FALSE(q.queue.pop(cmd));

				q.queue.size = size;
				q.queue.front = INT_MAX;
				q.queue.back = INT_MIN;

				ASSERT_TRUE(emplace_tagged(q.queue, 3));
				EXPECT_EQ(+q.queue.elements[0].pcm_handle, 3);
				EXPECT_EQ(+q.queue.size, 1);

				ASSERT_TRUE(q.queue.pop(cmd));
				EXPECT_EQ(+cmd.pcm_handle, 3);

				q.queue.size = size;
				EXPECT_FALSE(q.queue.full());

				EXPECT_TRUE(q.guards_intact());
			}
		}

		template <typename T>
		void test_mutation_between_operations()
		{
			static constexpr s32 values[] = { INT_MIN, -4, -1, 0, 1, 3, 4, 5, 0x40000000, INT_MAX };

			guarded_queue<T> q;
			u32 seed = 0x12345678;

			const auto next = [&]()
				{
					seed = seed * 1664525 + 1013904223;
					return seed >> 8;
				};

			for (u32 i = 0; i < 10000; i++)
			{
				switch (next() % 4)
				{
				case 0: q.queue.front = values[next() % std::size(values)]; break;
				case 1: q.queue.back = values[next() % std::size(values)]; break;
				case 2: q.queue.size = values[next() % std::size(values)]; break;
				default: break;
				}

				if (next() % 2)
				{
					if (emplace_tagged(q.queue, static_cast<s32>(i)))
					{
						ASSERT_GE(+q.queue.back, 0) << "iteration " << i;
						ASSERT_LT(+q.queue.back, 4) << "iteration " << i;
					}
				}
				else
				{
					T cmd{};

					if (q.queue.pop(cmd))
					{
						ASSERT_GE(+q.queue.front, 0) << "iteration " << i;
						ASSERT_LT(+q.queue.front, 4) << "iteration " << i;
					}
				}

				ASSERT_TRUE(q.size_valid()) << "iteration " << i;
				ASSERT_TRUE(q.guards_intact()) << "iteration " << i;
			}
		}
	}

	TEST(AdecQueue, IndexNormalization)
	{
		EXPECT_EQ(adec_internal::queue_index(0), 0u);
		EXPECT_EQ(adec_internal::queue_index(3), 3u);
		EXPECT_EQ(adec_internal::queue_index(4), 0u);
		EXPECT_EQ(adec_internal::queue_index(-1), 3u);
		EXPECT_EQ(adec_internal::queue_index(INT_MIN), 0u);
		EXPECT_EQ(adec_internal::queue_index(INT_MAX), 3u);
	}

	TEST(AdecCmdQueue, FifoOrder)
	{
		test_fifo_order<LpcmDecCmd>();
		test_fifo_order<AtracXdecCmd>();
	}

	TEST(AdecCmdQueue, WrapAround)
	{
		test_wrap_around<LpcmDecCmd>();
		test_wrap_around<AtracXdecCmd>();
	}

	TEST(AdecCmdQueue, PopMarksSlot)
	{
		test_pop_marks_slot<LpcmDecCmd>();
		test_pop_marks_slot<AtracXdecCmd>();
	}

	TEST(AdecCmdQueue, CorruptedIndices)
	{
		test_corrupted_indices<LpcmDecCmd>();
		test_corrupted_indices<AtracXdecCmd>();
	}

	TEST(AdecCmdQueue, CorruptedSize)
	{
		test_corrupted_size<LpcmDecCmd>();
		test_corrupted_size<AtracXdecCmd>();
	}

	TEST(AdecCmdQueue, MutationBetweenOperations)
	{
		test_mutation_between_operations<LpcmDecCmd>();
		test_mutation_between_operations<AtracXdecCmd>();
	}

	namespace
	{
		CellAdecParamLpcm make_lpcm_param(u32 channel_number, u32 size_of_word, u32 audio_payload_size)
		{
			CellAdecParamLpcm param{};
			param.channelNumber = channel_number;
			param.sampleRate = CELL_ADEC_FS_48kHz;
			param.sizeOfWord = size_of_word;
			param.audioPayloadSize = audio_payload_size;
			return param;
		}
	}

	TEST(LpcmDecLayout, ValidAccessUnit)
	{
		const auto layout = adec_internal::get_lpcm_au_layout(false, make_lpcm_param(CELL_ADEC_CH_STEREO, CELL_ADEC_BIT_LENGTH_16, 1920), 1920);
		EXPECT_TRUE(layout.valid);
		EXPECT_EQ(layout.output_size, 3840);
		EXPECT_EQ(layout.sample_num, 960);
		EXPECT_EQ(layout.channel_num, 2);
		EXPECT_EQ(layout.input_overread, 0u);
	}

	TEST(LpcmDecLayout, ConversionExceedsOutputBuffer)
	{
		// Mono streams contain a dummy channel, which is converted as well: the conversion writes twice the reported output size
		const auto mono = make_lpcm_param(CELL_ADEC_CH_MONO, CELL_ADEC_BIT_LENGTH_16, 0x10);
		EXPECT_TRUE(adec_internal::get_lpcm_au_layout(false, mono, 0x20000).valid);
		EXPECT_FALSE(adec_internal::get_lpcm_au_layout(false, mono, 0x20002).valid);

		const auto ch8_24 = make_lpcm_param(CELL_ADEC_CH_3_4_LFE, CELL_ADEC_BIT_LENGTH_24, 0x10);
		EXPECT_TRUE(adec_internal::get_lpcm_au_layout(false, ch8_24, 0x30000).valid);
		EXPECT_FALSE(adec_internal::get_lpcm_au_layout(false, ch8_24, 0x30003).valid);

		const auto unknown_word = make_lpcm_param(CELL_ADEC_CH_STEREO, CELL_ADEC_BIT_LENGTH_RESERVED1, 0x100);
		EXPECT_TRUE(adec_internal::get_lpcm_au_layout(false, unknown_word, 0x100).valid);
		EXPECT_FALSE(adec_internal::get_lpcm_au_layout(false, unknown_word, 0x100000).valid);
	}

	TEST(LpcmDecLayout, SampleNum)
	{
		EXPECT_TRUE(adec_internal::get_lpcm_au_layout(false, make_lpcm_param(CELL_ADEC_CH_STEREO, CELL_ADEC_BIT_LENGTH_16, 0x20000), 0x10).valid);
		EXPECT_FALSE(adec_internal::get_lpcm_au_layout(false, make_lpcm_param(CELL_ADEC_CH_STEREO, CELL_ADEC_BIT_LENGTH_16, 0x20020), 0x10).valid);
		EXPECT_FALSE(adec_internal::get_lpcm_au_layout(false, make_lpcm_param(CELL_ADEC_CH_STEREO, CELL_ADEC_BIT_LENGTH_16, 0x80000000), 0x10).valid);
		EXPECT_FALSE(adec_internal::get_lpcm_au_layout(false, make_lpcm_param(CELL_ADEC_CH_STEREO, CELL_ADEC_BIT_LENGTH_RESERVED1, 0xfffffff0), 0x10).valid);
	}

	TEST(LpcmDecLayout, Dvd)
	{
		const auto stereo_16 = make_lpcm_param(CELL_ADEC_LPCM_DVD_CH_STEREO, CELL_ADEC_BIT_LENGTH_16, 0x10);
		EXPECT_TRUE(adec_internal::get_lpcm_au_layout(true, stereo_16, 0x20000).valid);
		EXPECT_FALSE(adec_internal::get_lpcm_au_layout(true, stereo_16, 0x20001).valid);
		EXPECT_EQ(adec_internal::get_lpcm_au_layout(true, stereo_16, 0x100).input_overread, 0u);

		// Parameters modified by the guest after adecSetLpcmDvdParams()
		EXPECT_FALSE(adec_internal::get_lpcm_au_layout(true, make_lpcm_param(CELL_ADEC_LPCM_DVD_CH_STEREO, CELL_ADEC_BIT_LENGTH_RESERVED1, 0x10), 0x100).valid);
		EXPECT_FALSE(adec_internal::get_lpcm_au_layout(true, make_lpcm_param(CELL_ADEC_LPCM_DVD_CH_RESERVED2, CELL_ADEC_BIT_LENGTH_16, 0x10), 0x100).valid);
		EXPECT_FALSE(adec_internal::get_lpcm_au_layout(true, make_lpcm_param(0x1f, CELL_ADEC_BIT_LENGTH_16, 0x10), 0x100).valid);

		// The 20-bit conversion reads past the end of the access unit
		EXPECT_EQ(adec_internal::get_lpcm_au_layout(true, make_lpcm_param(CELL_ADEC_LPCM_DVD_CH_STEREO, CELL_ADEC_BIT_LENGTH_20, 0x10), 0x100).input_overread, 2u);
		EXPECT_EQ(adec_internal::get_lpcm_au_layout(true, make_lpcm_param(CELL_ADEC_LPCM_DVD_CH_MONO, CELL_ADEC_BIT_LENGTH_20, 0x10), 0x100).input_overread, 2u);
		EXPECT_EQ(adec_internal::get_lpcm_au_layout(true, make_lpcm_param(CELL_ADEC_LPCM_DVD_CH_UNK1, CELL_ADEC_BIT_LENGTH_20, 0x10), 0x100).input_overread, 1u);
		EXPECT_EQ(adec_internal::get_lpcm_au_layout(true, make_lpcm_param(CELL_ADEC_LPCM_DVD_CH_STEREO, CELL_ADEC_BIT_LENGTH_24, 0x10), 0x100).input_overread, 0u);
	}
}
