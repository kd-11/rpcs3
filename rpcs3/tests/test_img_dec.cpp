#include "stdafx.h"
#include <gtest/gtest.h>
#include "Emu/Cell/Modules/img_dec_common.h"
#include "Emu/Cell/Modules/cellJpgDec.h"

// The output buffer, its bytes per line and the source size of the GIF and JPG decoders are set by the guest

namespace SysModules
{
	using namespace img_dec_internal;

	TEST(ImgDec, OutputLayout)
	{
		// Packed rows: a smaller bytes per line (including 0) doesn't overlap them
		for (const u64 bytes_per_line : {u64{0}, u64{1}, u64{0x3ff}, u64{0x400}})
		{
			const auto layout = get_output_layout(bytes_per_line, 0x100, 0x10, 4);
			ASSERT_TRUE(layout.has_value());
			EXPECT_EQ(layout->row_bytes, 0x400u);
			EXPECT_EQ(layout->stride, 0x400u);
			EXPECT_EQ(layout->size, 0x4000u);
		}

		// Padded rows: the last row doesn't need the padding
		const auto padded = get_output_layout(0x500, 0x100, 0x10, 4);
		ASSERT_TRUE(padded.has_value());
		EXPECT_EQ(padded->stride, 0x500u);
		EXPECT_EQ(padded->size, 0x500u * 0xf + 0x400);

		const auto rgb = get_output_layout(0, 3, 2, 3);
		ASSERT_TRUE(rgb.has_value());
		EXPECT_EQ(rgb->row_bytes, 9u);
		EXPECT_EQ(rgb->size, 18u);

		// Empty images
		EXPECT_FALSE(get_output_layout(0, 0, 0x10, 4).has_value());
		EXPECT_FALSE(get_output_layout(0, 0x100, 0, 4).has_value());
		EXPECT_FALSE(get_output_layout(0, 0x100, 0x10, 0).has_value());

		// The output has to fit in the guest address space
		EXPECT_EQ(get_output_layout(0x10000, 0x3fff, 0x10000, 4)->size, 0xfffffffcu);
		EXPECT_FALSE(get_output_layout(0x10000, 0x4000, 0x10000, 4).has_value()); // 2^32
		EXPECT_FALSE(get_output_layout(0x10000, 0x3fff, 0x10001, 4).has_value());
		EXPECT_FALSE(get_output_layout(0x100000000ull, 1, 2, 4).has_value());
		EXPECT_FALSE(get_output_layout(u64{umax}, 1, 1, 4).has_value());
		EXPECT_FALSE(get_output_layout(0x80000000, 0x10, 3, 4).has_value()); // Strides that are negative as an int
		EXPECT_FALSE(get_output_layout(0, 0x40000000, 1, 4).has_value());
		EXPECT_FALSE(get_output_layout(0, 0xffffffff, 0xffffffff, 4).has_value());

		// A single row only needs its own bytes, whatever the stride
		EXPECT_EQ(get_output_layout(0xffffffff, 1, 1, 4)->size, 4u);
	}

	TEST(ImgDec, WriteRows)
	{
		const u8 src[2][8] =
		{
			{ 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17 },
			{ 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27 },
		};

		const auto layout = get_output_layout(10, 2, 2, 4);
		ASSERT_TRUE(layout.has_value());
		ASSERT_EQ(layout->size, 18u);

		// One canary byte after the output
		std::array<u8, 19> dst{};

		dst.fill(0xcc);
		write_rows(dst.data(), *layout, &src[0][0], 2, false, false);
		EXPECT_EQ(std::memcmp(dst.data(), src[0], 8), 0);
		EXPECT_EQ(dst[8], 0xcc); // Padding is left alone
		EXPECT_EQ(std::memcmp(dst.data() + 10, src[1], 8), 0);
		EXPECT_EQ(dst[18], 0xcc);

		dst.fill(0xcc);
		write_rows(dst.data(), *layout, &src[0][0], 2, false, true);
		EXPECT_EQ(std::memcmp(dst.data(), src[1], 8), 0);
		EXPECT_EQ(std::memcmp(dst.data() + 10, src[0], 8), 0);
		EXPECT_EQ(dst[18], 0xcc);

		dst.fill(0xcc);
		write_rows(dst.data(), *layout, &src[0][0], 2, true, false);
		const u8 argb0[8] = { 0x13, 0x10, 0x11, 0x12, 0x17, 0x14, 0x15, 0x16 };
		const u8 argb1[8] = { 0x23, 0x20, 0x21, 0x22, 0x27, 0x24, 0x25, 0x26 };
		EXPECT_EQ(std::memcmp(dst.data(), argb0, 8), 0);
		EXPECT_EQ(std::memcmp(dst.data() + 10, argb1, 8), 0);
		EXPECT_EQ(dst[18], 0xcc);
	}

	TEST(ImgDec, SerializeBytes)
	{
		CellJpgDecInfo info{};
		info.imageWidth = 640;
		info.imageHeight = 480;
		info.numComponents = 3;
		info.colorSpace = CELL_JPG_RGB;

		utils::serial ar;
		serialize_bytes(ar, info);
		ar.set_reading_state();

		CellJpgDecInfo loaded{};
		serialize_bytes(ar, loaded);

		EXPECT_EQ(std::memcmp(&loaded, &info, sizeof(info)), 0);
	}
}
