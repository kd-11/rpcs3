#include "stdafx.h"
#include <gtest/gtest.h>
// cellVdec.h is not self-contained: these are the headers cellVdec.cpp includes before it
#include "Emu/Cell/PPUModule.h"
#include "Emu/Cell/Modules/cellPamf.h"
#include "Emu/Cell/Modules/libavcdec.h"
#include "Emu/Cell/Modules/libdivx311dec.h"
#include "Emu/Cell/Modules/libdivxdec.h"
#include "Emu/Cell/Modules/libmvcdec.h"
#include "Emu/Cell/Modules/libsjvtd.h"
#include "Emu/Cell/Modules/libsmvd2.h"
#include "Emu/Cell/Modules/libsmvd4.h"
#include "Emu/Cell/Modules/libsvc1d.h"
#include "Emu/Cell/Modules/cellVdec.h"

// The output buffer of cellVdecGetPicture is set by the guest, the picture size by the stream

namespace SysModules
{
	using namespace vdec_internal;

	TEST(Vdec, PictureLayoutRgb)
	{
		for (const u32 format : {CELL_VDEC_PICFMT_ARGB32_ILV, CELL_VDEC_PICFMT_RGBA32_ILV})
		{
			const auto layout = get_picture_layout(format, 1920, 1080);
			ASSERT_TRUE(layout);
			EXPECT_EQ(layout.size, 1920u * 1080 * 4);
		}

		EXPECT_EQ(get_picture_layout(CELL_VDEC_PICFMT_ARGB32_ILV, 3, 3).size, 36u);
	}

	TEST(Vdec, PictureLayoutUyvy)
	{
		EXPECT_EQ(get_picture_layout(CELL_VDEC_PICFMT_UYVY422_ILV, 1920, 1080).size, 1920u * 1080 * 2);

		// The line is rounded up to a whole pair of pixels
		EXPECT_EQ(get_picture_layout(CELL_VDEC_PICFMT_UYVY422_ILV, 3, 2).size, 16u);
	}

	TEST(Vdec, PictureLayoutYuv420)
	{
		const auto hd = get_picture_layout(CELL_VDEC_PICFMT_YUV420_PLANAR, 1920, 1080);
		ASSERT_TRUE(hd);
		EXPECT_EQ(hd.u_offset, 1920u * 1080);
		EXPECT_EQ(hd.v_offset, 1920u * 1080 * 5 / 4);
		EXPECT_EQ(hd.size, 1920u * 1080 * 3 / 2);

		// Odd sizes round the chroma planes up
		const auto odd = get_picture_layout(CELL_VDEC_PICFMT_YUV420_PLANAR, 3, 3);
		ASSERT_TRUE(odd);
		EXPECT_EQ(odd.u_offset, 9u);
		EXPECT_EQ(odd.v_offset, 11u);
		EXPECT_EQ(odd.size, 15u);
	}

	TEST(Vdec, PictureLayoutInvalid)
	{
		// Only a layout built from a valid picture is valid
		EXPECT_FALSE(picture_layout{});

		// Unknown formats
		EXPECT_FALSE(get_picture_layout(4, 1920, 1080));
		EXPECT_FALSE(get_picture_layout(0xffffffff, 1920, 1080));

		// Empty or negative sizes
		EXPECT_FALSE(get_picture_layout(CELL_VDEC_PICFMT_ARGB32_ILV, 0, 1080));
		EXPECT_FALSE(get_picture_layout(CELL_VDEC_PICFMT_ARGB32_ILV, 1920, 0));
		EXPECT_FALSE(get_picture_layout(CELL_VDEC_PICFMT_ARGB32_ILV, -1, 1080));
		EXPECT_FALSE(get_picture_layout(CELL_VDEC_PICFMT_YUV420_PLANAR, 1920, INT32_MIN));

		// The output has to fit in the guest address space
		EXPECT_EQ(get_picture_layout(CELL_VDEC_PICFMT_ARGB32_ILV, 0x8000, 0x7fff).size, 0xfffe0000u);
		EXPECT_FALSE(get_picture_layout(CELL_VDEC_PICFMT_ARGB32_ILV, 0x8000, 0x8000)); // 2^32
		EXPECT_FALSE(get_picture_layout(CELL_VDEC_PICFMT_UYVY422_ILV, 0x10000, 0x8000));
		EXPECT_FALSE(get_picture_layout(CELL_VDEC_PICFMT_YUV420_PLANAR, 0x10000, 0xc000));
		EXPECT_FALSE(get_picture_layout(CELL_VDEC_PICFMT_RGBA32_ILV, INT32_MAX, INT32_MAX));
	}
}
