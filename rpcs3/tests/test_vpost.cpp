#include "stdafx.h"
#include <gtest/gtest.h>
#include "Emu/Cell/Modules/cellVpost.h"

// The picture sizes passed to cellVpostOpen and cellVpostExec are set by the guest

namespace SysModules
{
	using namespace vpost_internal;

	TEST(Vpost, MaxSize)
	{
		EXPECT_TRUE(is_valid_max_size(1, 1));
		EXPECT_TRUE(is_valid_max_size(1920, 1080));
		EXPECT_TRUE(is_valid_max_size(MAX_PICTURE_DIMENSION, MAX_PICTURE_DIMENSION));

		EXPECT_FALSE(is_valid_max_size(0, 1080));
		EXPECT_FALSE(is_valid_max_size(1920, 0));
		EXPECT_FALSE(is_valid_max_size(MAX_PICTURE_DIMENSION + 1, 1080));
		EXPECT_FALSE(is_valid_max_size(1920, MAX_PICTURE_DIMENSION + 1));
		EXPECT_FALSE(is_valid_max_size(0xffffffff, 0xffffffff));
	}

	TEST(Vpost, PictureLayout)
	{
		constexpr picture_limits hd{1920, 1080, 1920, 1080};

		const auto full = get_picture_layout(hd, 1920, 1080, 1920, 1080);
		ASSERT_TRUE(full.has_value());
		EXPECT_EQ(full->in_size, 1920u * 1080 * 3 / 2);
		EXPECT_EQ(full->out_size, 1920u * 1080 * 4);

		const auto scaled = get_picture_layout(hd, 720, 480, 1280, 720);
		ASSERT_TRUE(scaled.has_value());
		EXPECT_EQ(scaled->in_size, 720u * 480 * 3 / 2);
		EXPECT_EQ(scaled->out_size, 1280u * 720 * 4);

		// The output is not subsampled, so odd output sizes are fine
		const auto odd_out = get_picture_layout(hd, 2, 2, 1, 1);
		ASSERT_TRUE(odd_out.has_value());
		EXPECT_EQ(odd_out->in_size, 6u);
		EXPECT_EQ(odd_out->out_size, 4u);

		// Zero sizes
		EXPECT_FALSE(get_picture_layout(hd, 0, 1080, 1920, 1080).has_value());
		EXPECT_FALSE(get_picture_layout(hd, 1920, 0, 1920, 1080).has_value());
		EXPECT_FALSE(get_picture_layout(hd, 1920, 1080, 0, 1080).has_value());
		EXPECT_FALSE(get_picture_layout(hd, 1920, 1080, 1920, 0).has_value());

		// The YUV420 input needs even sizes
		EXPECT_FALSE(get_picture_layout(hd, 1919, 1080, 1920, 1080).has_value());
		EXPECT_FALSE(get_picture_layout(hd, 1920, 1079, 1920, 1080).has_value());

		// Larger than the limits the instance was opened with
		EXPECT_FALSE(get_picture_layout(hd, 1922, 1080, 1920, 1080).has_value());
		EXPECT_FALSE(get_picture_layout(hd, 1920, 1082, 1920, 1080).has_value());
		EXPECT_FALSE(get_picture_layout(hd, 1920, 1080, 1921, 1080).has_value());
		EXPECT_FALSE(get_picture_layout(hd, 1920, 1080, 1920, 1081).has_value());
	}

	TEST(Vpost, PictureLayoutCap)
	{
		// The dimension cap holds even if the limits are larger
		constexpr picture_limits huge{0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff};

		const auto max = get_picture_layout(huge, MAX_PICTURE_DIMENSION, MAX_PICTURE_DIMENSION, MAX_PICTURE_DIMENSION, MAX_PICTURE_DIMENSION);
		ASSERT_TRUE(max.has_value());
		EXPECT_EQ(max->in_size, u64{MAX_PICTURE_DIMENSION} * MAX_PICTURE_DIMENSION * 3 / 2);
		EXPECT_EQ(max->out_size, u64{MAX_PICTURE_DIMENSION} * MAX_PICTURE_DIMENSION * 4);

		EXPECT_FALSE(get_picture_layout(huge, MAX_PICTURE_DIMENSION + 2, 2, 2, 2).has_value());
		EXPECT_FALSE(get_picture_layout(huge, 2, 2, 2, MAX_PICTURE_DIMENSION + 1).has_value());

		// Products that wrap in 32 bits
		EXPECT_FALSE(get_picture_layout(huge, 0x10000, 0x10000, 2, 2).has_value());
		EXPECT_FALSE(get_picture_layout(huge, 2, 2, 0x10000, 0x10001).has_value());

		// Sizes whose row stride overflows an int
		EXPECT_FALSE(get_picture_layout(huge, 2, 2, 0x20000000, 2).has_value());
		EXPECT_FALSE(get_picture_layout(huge, 0x80000000, 2, 2, 2).has_value());
		EXPECT_FALSE(get_picture_layout(huge, 0xfffffffe, 0xfffffffe, 0xffffffff, 0xffffffff).has_value());
	}
}
