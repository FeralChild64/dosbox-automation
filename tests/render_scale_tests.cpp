// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//

#include "gui/render/render.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace {

size_t ExpectedOutBufSize(const size_t width, const size_t height)
{
	return std::max(width, static_cast<size_t>(ScalerMaxWidth)) *
	       std::max(height, static_cast<size_t>(ScalerMaxHeight));
}

TEST(RenderScale, CacheSizeCountsPixels)
{
	Render::Scale scale = {};
	scale.SetSize(640, 400);

	EXPECT_EQ(scale.cache_size, 640u * 400u);
}

TEST(RenderScale, SameSizeKeepsTheBuffers)
{
	Render::Scale scale = {};
	scale.SetSize(640, 400);
	const auto* cache   = scale.cache;
	const auto* out_buf = scale.out_buf;
	scale.cache[0]      = 0xdeadbeef;

	scale.SetSize(640, 400);

	EXPECT_EQ(scale.cache, cache);
	EXPECT_EQ(scale.out_buf, out_buf);
	EXPECT_EQ(scale.cache[0], 0xdeadbeefu);
}

// Same pixel count, different shape: the cache may stay, out_buf may not,
// because its size depends on each dimension against the scaler limits.
TEST(RenderScale, SamePixelCountOtherShapeResizesOutBuf)
{
	const size_t wide   = ScalerMaxWidth * 2;
	const size_t narrow = 100;

	Render::Scale scale = {};
	scale.SetSize(wide, narrow);
	ASSERT_EQ(scale.out_buf_size, ExpectedOutBufSize(wide, narrow));

	scale.SetSize(narrow, wide);

	EXPECT_EQ(scale.out_buf_size, ExpectedOutBufSize(narrow, wide));
	EXPECT_NE(ExpectedOutBufSize(wide, narrow), ExpectedOutBufSize(narrow, wide));
}

// The deinterlacer writes out_height rows at the backend's pitch, which is
// only known when a frame starts; SetSize() can only estimate it.
TEST(RenderScale, ReserveOutBufGrowsAndZeroesWhenTheFrameNeedsMore)
{
	Render::Scale scale = {};
	scale.SetSize(640, 400);
	const auto* cache       = scale.cache;
	const auto needed_bytes = scale.out_buf_size * sizeof(uint32_t) + 4096;

	EXPECT_TRUE(scale.ReserveOutBufBytes(needed_bytes));

	ASSERT_NE(scale.out_buf, nullptr);
	EXPECT_GE(scale.out_buf_size * sizeof(uint32_t), needed_bytes);
	EXPECT_EQ(scale.out_buf[0], 0u);
	EXPECT_EQ(scale.out_buf[scale.out_buf_size - 1], 0u);
	EXPECT_EQ(scale.cache, cache);
}

TEST(RenderScale, ReserveOutBufKeepsTheBufferWhenItFits)
{
	Render::Scale scale = {};
	scale.SetSize(640, 400);
	const auto* out_buf = scale.out_buf;
	const auto size     = scale.out_buf_size;

	EXPECT_FALSE(scale.ReserveOutBufBytes(size * sizeof(uint32_t)));

	EXPECT_EQ(scale.out_buf, out_buf);
	EXPECT_EQ(scale.out_buf_size, size);
}

TEST(RenderScale, ReserveOutBufRoundsUpToWholePixels)
{
	Render::Scale scale = {};
	scale.SetSize(640, 400);
	const auto needed_bytes = scale.out_buf_size * sizeof(uint32_t) + 1;

	EXPECT_TRUE(scale.ReserveOutBufBytes(needed_bytes));

	EXPECT_GE(scale.out_buf_size * sizeof(uint32_t), needed_bytes);
}

} // namespace
