// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//

#include "gui/truetype_character_block.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <stdexcept>

namespace {

using TrueType::BytesPerPixel;
using TrueType::CharacterBlock;

constexpr uint8_t Full = UINT8_MAX;

CharacterBlock MakeBlock(const uint32_t width, const uint32_t height,
                         const uint8_t value)
{
	CharacterBlock block(width, height);
	for (uint32_t y = 0; y < height; ++y) {
		for (uint32_t x = 0; x < width; ++x) {
			block.SetPixel(x, y, value);
		}
	}
	return block;
}

TEST(TrueTypeCharacterBlock, StartsEmptyAndKeepsItsSize)
{
	const CharacterBlock block(9, 16);

	EXPECT_EQ(block.GetWidth(), 9u);
	EXPECT_EQ(block.GetHeight(), 16u);
	EXPECT_EQ(block.GetPixel(0, 0), 0);
	EXPECT_EQ(block.GetPixel(8, 15), 0);
}

TEST(TrueTypeCharacterBlock, GetPixelOutsideTheBlockThrows)
{
	const CharacterBlock block(8, 8);

	EXPECT_THROW(block.GetPixel(0, 8), std::out_of_range);
}

TEST(TrueTypeCharacterBlock, InvertMirrorsEveryValue)
{
	CharacterBlock block(2, 1);
	block.SetPixel(0, 0, 0);
	block.SetPixel(1, 0, 100);

	block.Invert();

	EXPECT_EQ(block.GetPixel(0, 0), Full);
	EXPECT_EQ(block.GetPixel(1, 0), 155);
}

TEST(TrueTypeCharacterBlock, RenderInGreyWritesOneLineAsGreyPixels)
{
	CharacterBlock block(2, 2);
	block.SetPixel(0, 1, 10);
	block.SetPixel(1, 1, 200);

	std::array<uint8_t, 2 * BytesPerPixel> line = {};
	block.RenderInGrey(line.data(), 1);

	for (uint8_t byte = 0; byte < BytesPerPixel; ++byte) {
		EXPECT_EQ(line[byte], 10);
		EXPECT_EQ(line[BytesPerPixel + byte], 200);
	}
}

TEST(TrueTypeCharacterBlock, OnlyAPixelOnTheBorderTouchesIt)
{
	CharacterBlock middle(8, 8);
	middle.SetPixel(3, 4, 1);

	EXPECT_FALSE(middle.IsTouchingLeft());
	EXPECT_FALSE(middle.IsTouchingRight());
	EXPECT_FALSE(middle.IsTouchingTop());
	EXPECT_FALSE(middle.IsTouchingBottom());

	CharacterBlock right_edge(8, 8);
	right_edge.SetPixel(7, 4, 1);

	EXPECT_FALSE(right_edge.IsTouchingLeft());
	EXPECT_TRUE(right_edge.IsTouchingRight());
	EXPECT_FALSE(right_edge.IsTouchingTop());
	EXPECT_FALSE(right_edge.IsTouchingBottom());
}

TEST(TrueTypeCharacterBlock, DistancesCountFullPixelsToEachBorder)
{
	CharacterBlock block(8, 8);
	block.SetPixel(2, 3, Full);

	EXPECT_FLOAT_EQ(block.GetDistanceLeft(), 2.0f);
	EXPECT_FLOAT_EQ(block.GetDistanceRight(), 5.0f);
	EXPECT_FLOAT_EQ(block.GetDistanceTop(), 3.0f);
	EXPECT_FLOAT_EQ(block.GetDistanceBottom(), 4.0f);
}

// A faint antialiased pixel counts as partly empty space
TEST(TrueTypeCharacterBlock, DistancesAddTheMissingBrightnessOfTheNearestPixel)
{
	CharacterBlock block(8, 8);
	block.SetPixel(2, 3, 51);

	EXPECT_FLOAT_EQ(block.GetDistanceLeft(), 2.0f + 204.0f / 255.0f);
}

TEST(TrueTypeCharacterBlock, DistanceUsesTheBrightestPixelOfTheNearestColumn)
{
	CharacterBlock block(8, 8);
	block.SetPixel(2, 0, 51);
	block.SetPixel(2, 5, Full);
	block.SetPixel(3, 1, Full);

	EXPECT_FLOAT_EQ(block.GetDistanceLeft(), 2.0f);
}

TEST(TrueTypeCharacterBlock, ContentSizeIsWhatTheDistancesLeave)
{
	CharacterBlock block(8, 16);
	for (uint32_t x = 2; x <= 4; ++x) {
		for (uint32_t y = 5; y <= 10; ++y) {
			block.SetPixel(x, y, Full);
		}
	}

	EXPECT_FLOAT_EQ(block.GetContentWidth(), 3.0f);
	EXPECT_FLOAT_EQ(block.GetContentHeight(), 6.0f);
}

// An empty glyph (a space) reports no distances, so its content is the
// whole block; only the aspect ratio correction sees this, and it draws
// nothing for an empty glyph. Pinned so a change here is deliberate.
TEST(TrueTypeCharacterBlock, EmptyBlockReportsTheWholeBlockAsContent)
{
	const CharacterBlock block(8, 16);

	EXPECT_FLOAT_EQ(block.GetDistanceLeft(), 0.0f);
	EXPECT_FLOAT_EQ(block.GetDistanceBottom(), 0.0f);
	EXPECT_FLOAT_EQ(block.GetContentWidth(), 8.0f);
	EXPECT_FLOAT_EQ(block.GetContentHeight(), 16.0f);
}

TEST(TrueTypeCharacterBlock, BlendKeepsTheBrighterPixel)
{
	CharacterBlock block(2, 1);
	block.SetPixel(0, 0, 200);
	block.SetPixel(1, 0, 10);

	CharacterBlock other(2, 1);
	other.SetPixel(0, 0, 50);
	other.SetPixel(1, 0, 90);

	block.Blend(other);

	EXPECT_EQ(block.GetPixel(0, 0), 200);
	EXPECT_EQ(block.GetPixel(1, 0), 90);
}

TEST(TrueTypeCharacterBlock, BlendWithABiggerBlockStaysInsideTheSmallerOne)
{
	auto block       = MakeBlock(2, 2, 0);
	const auto other = MakeBlock(4, 4, Full);

	block.Blend(other);

	EXPECT_EQ(block.GetWidth(), 2u);
	EXPECT_EQ(block.GetPixel(1, 1), Full);

	auto big         = MakeBlock(4, 4, 0);
	const auto small = MakeBlock(2, 2, Full);

	big.Blend(small);

	EXPECT_EQ(big.GetPixel(1, 1), Full);
	EXPECT_EQ(big.GetPixel(2, 2), 0);
}

TEST(TrueTypeCharacterBlock, MirrorSwapsColumnsAndKeepsTheMiddleOne)
{
	CharacterBlock block(3, 1);
	block.SetPixel(0, 0, 1);
	block.SetPixel(1, 0, 2);
	block.SetPixel(2, 0, 3);

	block.MirrorHorizontally();

	EXPECT_EQ(block.GetPixel(0, 0), 3);
	EXPECT_EQ(block.GetPixel(1, 0), 2);
	EXPECT_EQ(block.GetPixel(2, 0), 1);
}

TEST(TrueTypeCharacterBlock, SharpeningLeavesBlocksBelowEightPixelsAlone)
{
	CharacterBlock block(7, 7);
	block.SetPixel(0, 1, 100);

	block.SharpenAllBorders();

	EXPECT_EQ(block.GetPixel(0, 0), 0);
	EXPECT_EQ(block.GetPixel(0, 1), 100);
}

// Depth for 16 px: round(0.2 * 16) - 1 = 2, so rows 0 to 2 take the
// brightest value among them and row 3 is untouched
TEST(TrueTypeCharacterBlock, SharpenOnlyTopFillsTheTopBandWithItsBrightestValue)
{
	CharacterBlock block(8, 16);
	block.SetPixel(0, 2, 180);
	block.SetPixel(0, 3, Full);
	block.SetPixel(0, 15, 40);
	// brightest on the border: the write must reach the band's inner row
	block.SetPixel(1, 0, 120);

	block.SharpenOnlyTop();

	EXPECT_EQ(block.GetPixel(0, 0), 180);
	EXPECT_EQ(block.GetPixel(0, 1), 180);
	EXPECT_EQ(block.GetPixel(0, 2), 180);
	EXPECT_EQ(block.GetPixel(0, 3), Full);
	EXPECT_EQ(block.GetPixel(0, 15), 40);
	EXPECT_EQ(block.GetPixel(1, 2), 120);
	EXPECT_EQ(block.GetPixel(1, 3), 0);
	EXPECT_EQ(block.GetPixel(2, 0), 0);
}

TEST(TrueTypeCharacterBlock, SharpenOnlyBottomFillsTheBottomBandWithItsBrightestValue)
{
	CharacterBlock block(8, 16);
	block.SetPixel(0, 13, 180);
	block.SetPixel(0, 12, Full);
	block.SetPixel(0, 0, 40);
	// brightest on the border: the write must reach the band's inner row
	block.SetPixel(1, 15, 120);

	block.SharpenOnlyBottom();

	EXPECT_EQ(block.GetPixel(0, 15), 180);
	EXPECT_EQ(block.GetPixel(0, 14), 180);
	EXPECT_EQ(block.GetPixel(0, 13), 180);
	EXPECT_EQ(block.GetPixel(0, 12), Full);
	EXPECT_EQ(block.GetPixel(0, 0), 40);
	EXPECT_EQ(block.GetPixel(1, 13), 120);
	EXPECT_EQ(block.GetPixel(1, 12), 0);
}

// Depth for 8 px: round(0.2 * 8) - 1 = 1, so a two-pixel band per side
TEST(TrueTypeCharacterBlock, SharpenAllBordersWorksOnEverySide)
{
	CharacterBlock block(8, 8);
	block.SetPixel(1, 4, 90); // left band
	block.SetPixel(6, 4, 70); // right band
	block.SetPixel(4, 1, 60); // top band
	block.SetPixel(4, 6, 50); // bottom band

	block.SharpenAllBorders();

	EXPECT_EQ(block.GetPixel(0, 4), 90);
	EXPECT_EQ(block.GetPixel(7, 4), 70);
	EXPECT_EQ(block.GetPixel(4, 0), 60);
	EXPECT_EQ(block.GetPixel(4, 7), 50);
	EXPECT_EQ(block.GetPixel(3, 3), 0);
}

} // namespace
