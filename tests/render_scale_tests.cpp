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

} // namespace
