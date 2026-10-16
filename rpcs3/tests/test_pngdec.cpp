#include "stdafx.h"
#include <gtest/gtest.h>
#include "Emu/Cell/Modules/cellPngDec.h"

// The output buffers and the input stream of the PNG decoder are set by the guest

namespace SysModules
{
	TEST(PngDec, OutputSize)
	{
		using png_dec_internal::get_output_size;

		EXPECT_EQ(get_output_size(0, 0x100, 0x100), 0u);
		EXPECT_EQ(get_output_size(1, 0x100, 0x80), 0x80u);
		EXPECT_EQ(get_output_size(4, 0x100, 0x100), 0x400u);
		EXPECT_EQ(get_output_size(4, 0x200, 0x100), 0x700u); // The last line doesn't need the padding

		// Lines can't overlap
		EXPECT_FALSE(get_output_size(4, 0x80, 0x100).has_value());

		// The output has to fit in the guest address space
		EXPECT_FALSE(get_output_size(0x10000, 0x10000, 0x10000).has_value()); // 0xffff * 0x10000 + 0x10000 = 2^32
		EXPECT_FALSE(get_output_size(0x10001, 0x10000, 0x10000).has_value());
		EXPECT_FALSE(get_output_size(2, 0x100000000ull, 0x10).has_value());
		EXPECT_FALSE(get_output_size(0xffffffff, 0xffffffff, 0xffffffff).has_value());
		EXPECT_EQ(get_output_size(0x10000, 0x10000, 0xffff), 0xffffffffu);
	}

	TEST(PngDec, ProgressiveRowAddr)
	{
		using png_dec_internal::get_progressive_row_addr;

		EXPECT_EQ(get_progressive_row_addr(0x10000000, 5, 4, 0x400, 0, 0x400), 0x10000400u);
		EXPECT_EQ(get_progressive_row_addr(0x10000000, 4, 4, 0x400, 0, 0x400), 0x10000000u);

		// Interlaced passes always write at the start of the output image
		EXPECT_EQ(get_progressive_row_addr(0x10000000, 100, 4, 0x400, 3, 0x400), 0x10000000u);

		// The row must not be before the start of the output
		EXPECT_FALSE(get_progressive_row_addr(0x10000000, 3, 4, 0x400, 0, 0x400).has_value());

		// The guest can set any frame width
		EXPECT_FALSE(get_progressive_row_addr(0x10000000, 5, 4, 0x100000000ull, 0, 0x400).has_value());
		EXPECT_FALSE(get_progressive_row_addr(0x10000000, 0xffffffff, 0, 0xffffffff, 0, 0x400).has_value());

		// The row has to fit in the guest address space
		EXPECT_EQ(get_progressive_row_addr(0xfffffc00, 0, 0, 0x400, 0, 0x400), 0xfffffc00u);
		EXPECT_FALSE(get_progressive_row_addr(0xfffffc01, 0, 0, 0x400, 0, 0x400).has_value());
		EXPECT_FALSE(get_progressive_row_addr(0xfffffc00, 1, 0, 0x400, 0, 0x400).has_value());
	}

	TEST(PngDec, CanRead)
	{
		using png_dec_internal::can_read;

		EXPECT_TRUE(can_read(8, 0x100, 0xf8));
		EXPECT_TRUE(can_read(0x100, 0x100, 0));
		EXPECT_FALSE(can_read(8, 0x100, 0xf9));
		EXPECT_FALSE(can_read(0x101, 0x100, 0));
		EXPECT_FALSE(can_read(8, 0x100, 0xffffffffffffffffull));
	}

	TEST(PngDec, GuestLayout)
	{
		// The guest structs only contain IDs and reserved bytes
		EXPECT_EQ(sizeof(PngStream), 0x80u);
		EXPECT_EQ(sizeof(PngBuffer), 0x20u);
		EXPECT_EQ(sizeof(PngHandle), 0x14u);
	}
}
