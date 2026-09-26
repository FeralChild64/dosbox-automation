// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//
// Written by FeralChild64 for the TrueType output (PR #8), moved here
// from truetype_output.cpp unchanged.

#ifndef DOSBOX_TRUETYPE_CHARACTER_BLOCK_H
#define DOSBOX_TRUETYPE_CHARACTER_BLOCK_H

#include <cstdint>
#include <vector>

// Rendering engine single block support, split out of truetype_output.cpp
// so the arithmetic can be unit tested

namespace TrueType {

// Value depending on the output rendering format
constexpr uint8_t BytesPerPixel = 3;

class CharacterBlock {
public:
	CharacterBlock() = delete;
	CharacterBlock(const uint32_t horizontal_px, const uint32_t vertical_px);

	uint32_t GetWidth() const
	{
		return size_horizontal_px;
	}
	uint32_t GetHeight() const
	{
		return size_vertical_px;
	}

	uint8_t GetPixel(const uint32_t horizontal_px,
	                 const uint32_t vertical_px) const;
	void SetPixel(const uint32_t horizontal_px,
	              const uint32_t vertical_px,
	              const uint8_t value);

	void Invert();

	// Render one block, without colors
	void RenderInGrey(uint8_t* const destination, const uint32_t block_line) const;

	// Functions to check if the content touches the border, do not take
	// pixel brightness into account
	bool IsTouchingLeft() const;
	bool IsTouchingRight() const;
	bool IsTouchingTop() const;
	bool IsTouchingBottom() const;

	// Calculates the glyph distance from the border, take the antialiased
	// pixel brightness into account
	float GetDistanceLeft() const;
	float GetDistanceRight() const;
	float GetDistanceTop() const;
	float GetDistanceBottom() const;

	float GetContentWidth() const;
	float GetContentHeight() const;

	// Blend other block with the current one
	void Blend(const CharacterBlock& other);

	// Replace the character with the mirrored image
	void MirrorHorizontally();

	// Functions to remove antialiasing from the given borders, to fix the
	// view when two drawing characters are placed next to each other
	void SharpenAllBorders();
	void SharpenOnlyTop();
	void SharpenOnlyBottom();

private:
	// Helper functions for border sharpening (de-antialiasing)
	uint32_t GetSharpenDepth(const uint32_t depth_check_px) const;
	void SharpenTop(const uint32_t depth_vertical_px);
	void SharpenBottom(const uint32_t depth_vertical_px);
	void SharpenLeft(const uint32_t depth_horizontal_px);
	void SharpenRight(const uint32_t depth_horizontal_px);

	uint32_t size_horizontal_px = 0;
	uint32_t size_vertical_px   = 0;

	std::vector<uint8_t> data = {};
};

} // namespace TrueType

#endif // DOSBOX_TRUETYPE_CHARACTER_BLOCK_H
