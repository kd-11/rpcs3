#include "stdafx.h"
#include <gtest/gtest.h>
#include "Emu/Cell/PPUModule.h" // cellAdec.h is not self-contained
#include "Emu/Cell/Modules/cellAtracXdec.h"

// The configuration of an ATRAC3plus decoder is set by the guest and has to be validated before use

namespace SysModules
{
	namespace
	{
		constexpr u32 max_nch_values[] = { 2, 6, 8 };
		constexpr u32 bw_pcm_values[] = { CELL_ADEC_ATRACX_WORD_SZ_16BIT, CELL_ADEC_ATRACX_WORD_SZ_24BIT, CELL_ADEC_ATRACX_WORD_SZ_32BIT, CELL_ADEC_ATRACX_WORD_SZ_FLOAT };

		AtracXdecAtsHeader make_ats_header(u16 sync_word, u16 params)
		{
			AtracXdecAtsHeader ats{};
			ats.sync_word = sync_word;
			ats.params = params;
			return ats;
		}

		constexpr u16 make_ats_params(u32 sample_rate_idx, u32 ch_config_idx, u32 nbytes)
		{
			return static_cast<u16>(sample_rate_idx << 13 | ch_config_idx << 10 | (nbytes / 8 - 1));
		}
	}

	TEST(AtracXdec, ConfigInfo)
	{
		EXPECT_TRUE(atracxdec_internal::check_config_info(2, 48000, 2, 0x2000));
		EXPECT_TRUE(atracxdec_internal::check_config_info(2, 44100, 1, 1));

		EXPECT_FALSE(atracxdec_internal::check_config_info(8, 32000, 2, 0x100));
		EXPECT_FALSE(atracxdec_internal::check_config_info(8, 0, 2, 0x100));
		EXPECT_FALSE(atracxdec_internal::check_config_info(8, 48000, 0, 0x100));
		EXPECT_FALSE(atracxdec_internal::check_config_info(8, 48000, 8, 0x100));
		EXPECT_FALSE(atracxdec_internal::check_config_info(8, 48000, 0xffffffff, 0x100));
		EXPECT_FALSE(atracxdec_internal::check_config_info(8, 48000, 2, 0));
		EXPECT_FALSE(atracxdec_internal::check_config_info(8, 48000, 2, 0x2001));
		EXPECT_FALSE(atracxdec_internal::check_config_info(8, 48000, 2, 0xffffffff));
	}

	TEST(AtracXdec, ConfigInfoCapacity)
	{
		// ch_config_idx 1-4: 1-4 channels, 5-7: 6-8 channels
		for (u32 ch_config_idx = 1; ch_config_idx <= 7; ch_config_idx++)
		{
			const u32 nch_in = atracxdec_internal::get_nch_in(ch_config_idx);
			EXPECT_EQ(nch_in, ch_config_idx <= 4 ? ch_config_idx : ch_config_idx + 1);

			for (const u32 max_nch : max_nch_values)
			{
				EXPECT_EQ(atracxdec_internal::check_config_info(max_nch, 48000, ch_config_idx, 0x100), nch_in <= max_nch) << "ch_config_idx=" << ch_config_idx << ", max_nch=" << max_nch;
			}
		}
	}

	TEST(AtracXdec, OutputFormat)
	{
		for (const u32 bw_pcm : bw_pcm_values)
		{
			EXPECT_TRUE(atracxdec_internal::check_output_format(2, bw_pcm, 1));
			EXPECT_TRUE(atracxdec_internal::check_output_format(2, bw_pcm, 2));
			EXPECT_FALSE(atracxdec_internal::check_output_format(2, bw_pcm, 3));
			EXPECT_FALSE(atracxdec_internal::check_output_format(8, bw_pcm, 0));
			EXPECT_FALSE(atracxdec_internal::check_output_format(8, bw_pcm, 9));
			EXPECT_FALSE(atracxdec_internal::check_output_format(8, bw_pcm, 0xffffffff));
		}

		for (const u32 bw_pcm : { 0u, 1u, 5u, 0x7fu, 0x82u, 0x83u, 0x85u, 0xffffffffu })
		{
			EXPECT_FALSE(atracxdec_internal::check_output_format(8, bw_pcm, 2)) << "bw_pcm=" << bw_pcm;
		}
	}

	TEST(AtracXdec, PcmOutputSize)
	{
		EXPECT_EQ(atracxdec_internal::get_pcm_output_size(CELL_ADEC_ATRACX_WORD_SZ_16BIT, 2), 2u * 2 * ATXDEC_SAMPLES_PER_FRAME);
		EXPECT_EQ(atracxdec_internal::get_pcm_output_size(CELL_ADEC_ATRACX_WORD_SZ_24BIT, 2), 4u * 2 * ATXDEC_SAMPLES_PER_FRAME);
		EXPECT_EQ(atracxdec_internal::get_pcm_output_size(CELL_ADEC_ATRACX_WORD_SZ_FLOAT, 8), 4u * 8 * ATXDEC_SAMPLES_PER_FRAME);
	}

	TEST(AtracXdec, OutputFitsWorkMemory)
	{
		// Every accepted configuration writes only inside the work memory checked when opening the decoder
		for (const u32 max_nch : max_nch_values)
		{
			const u32 work_mem_end = atracxdec_internal::get_work_mem_end(max_nch);

			for (u32 ch_config_idx = 1; ch_config_idx <= 7; ch_config_idx++)
			{
				if (!atracxdec_internal::check_config_info(max_nch, 48000, ch_config_idx, 0x100))
				{
					continue;
				}

				const u32 nch_in = atracxdec_internal::get_nch_in(ch_config_idx);
				const u32 output_offset = atracxdec_internal::WORK_MEM_OFFSET + atracxdec_internal::get_spurs_mem_size(nch_in);

				for (const u32 bw_pcm : bw_pcm_values)
				{
					for (u32 nch_out = 1; nch_out <= max_nch; nch_out++)
					{
						ASSERT_TRUE(atracxdec_internal::check_output_format(max_nch, bw_pcm, nch_out));

						const u32 conversion_size = atracxdec_internal::get_pcm_output_size(bw_pcm, nch_in);
						const u32 silence_size = ATXDEC_SAMPLES_PER_FRAME * (bw_pcm & 0x7f) * nch_out;

						EXPECT_LE(output_offset + conversion_size, work_mem_end) << "max_nch=" << max_nch << ", ch_config_idx=" << ch_config_idx << ", bw_pcm=" << bw_pcm;
						EXPECT_LE(output_offset + silence_size, work_mem_end) << "max_nch=" << max_nch << ", ch_config_idx=" << ch_config_idx << ", bw_pcm=" << bw_pcm << ", nch_out=" << nch_out;
					}
				}
			}
		}
	}

	TEST(AtracXdec, AtsHeader)
	{
		const auto info = atracxdec_internal::parse_ats_header(make_ats_header(0x0fd0, make_ats_params(2, 2, 0x2e8)));
		ASSERT_TRUE(info.has_value());
		EXPECT_EQ(info->sampling_freq, 48000u);
		EXPECT_EQ(info->ch_config_idx, 2u);
		EXPECT_EQ(info->nbytes, 0x2e8u);

		const auto info_44k = atracxdec_internal::parse_ats_header(make_ats_header(0x0fd0, make_ats_params(1, 7, 0x2000)));
		ASSERT_TRUE(info_44k.has_value());
		EXPECT_EQ(info_44k->sampling_freq, 44100u);
		EXPECT_EQ(info_44k->ch_config_idx, 7u);
		EXPECT_EQ(info_44k->nbytes, 0x2000u);

		EXPECT_FALSE(atracxdec_internal::parse_ats_header(make_ats_header(0x0fd1, make_ats_params(2, 2, 0x2e8))).has_value());
		EXPECT_FALSE(atracxdec_internal::parse_ats_header(make_ats_header(0x0fd0, make_ats_params(2, 0, 0x2e8))).has_value());
		EXPECT_FALSE(atracxdec_internal::parse_ats_header(make_ats_header(0x0fd0, make_ats_params(0, 2, 0x2e8))).has_value());
		EXPECT_FALSE(atracxdec_internal::parse_ats_header(make_ats_header(0x0fd0, make_ats_params(3, 2, 0x2e8))).has_value());
		EXPECT_FALSE(atracxdec_internal::parse_ats_header(make_ats_header(0x0fd0, make_ats_params(7, 2, 0x2e8))).has_value());

		// Every value the header can encode is within the accepted range, except for the number of channels supported by the work memory
		for (u32 params = 0; params <= 0xffff; params++)
		{
			if (const auto parsed = atracxdec_internal::parse_ats_header(make_ats_header(0x0fd0, static_cast<u16>(params))))
			{
				ASSERT_TRUE(atracxdec_internal::check_config_info(8, parsed->sampling_freq, parsed->ch_config_idx, parsed->nbytes)) << "params=" << params;
			}
		}
	}

	TEST(AtracXdec, GuestLayout)
	{
		// The former native pointers are replaced by the host object ID and padding, without changing the layout
		EXPECT_EQ(sizeof(AtracXdecDecoder), 0xa8u);
		EXPECT_EQ(offsetof(AtracXdecDecoder, host_id), 0x30u);
		EXPECT_EQ(sizeof(AtracXdecContext), 0x268u);
		EXPECT_EQ(offsetof(AtracXdecContext, decoder), 0x1c0u);
		EXPECT_EQ(atracxdec_internal::WORK_MEM_OFFSET, 0x280u + ATXDEC_SPURS_STRUCTS_SIZE);
	}
}
