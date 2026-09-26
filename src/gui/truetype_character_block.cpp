// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//
// Written by FeralChild64 for the TrueType output (PR #8), moved here
// from truetype_output.cpp unchanged.

#include "truetype_character_block.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>

namespace TrueType {

CharacterBlock::CharacterBlock(const uint32_t horizontal_px, const uint32_t vertical_px)
        : size_horizontal_px(horizontal_px),
          size_vertical_px(vertical_px)
{
	assert(horizontal_px < UINT16_MAX);
	assert(vertical_px < UINT16_MAX);

	data.resize(horizontal_px * vertical_px);
	data.shrink_to_fit();
}

uint8_t CharacterBlock::GetPixel(const uint32_t horizontal_px,
                                 const uint32_t vertical_px) const
{
	return data.at(horizontal_px + vertical_px * size_horizontal_px);
}

void CharacterBlock::SetPixel(const uint32_t horizontal_px,
                              const uint32_t vertical_px, const uint8_t value)
{
	data[horizontal_px + vertical_px * size_horizontal_px] = value;
}

void CharacterBlock::Invert()
{
	for (auto& pixel : data) {
		pixel = UINT8_MAX - pixel;
	}
}

void CharacterBlock::RenderInGrey(uint8_t* const destination,
                                  const uint32_t block_line) const
{
	for (uint32_t pixel = 0; pixel < size_horizontal_px; ++pixel) {
		const auto value = *(data.begin() +
		                     block_line * size_horizontal_px + pixel);

		*(destination + pixel * BytesPerPixel + 0) = value;
		*(destination + pixel * BytesPerPixel + 1) = value;
		*(destination + pixel * BytesPerPixel + 2) = value;

		static_assert(BytesPerPixel >= 3);
		for (uint8_t byte = 3; byte < BytesPerPixel; ++byte) {
			*(destination + pixel * BytesPerPixel + byte) = 0;
		}
	}
}

bool CharacterBlock::IsTouchingLeft() const
{
	for (uint32_t y = 0; y < size_vertical_px; ++y) {
		if (GetPixel(0, y) != 0) {
			return true;
		}
	}

	return false;
}

bool CharacterBlock::IsTouchingRight() const
{
	for (uint32_t y = 0; y < size_vertical_px; ++y) {
		if (GetPixel(size_horizontal_px - 1, y) != 0) {
			return true;
		}
	}

	return false;
}

bool CharacterBlock::IsTouchingTop() const
{
	for (uint32_t x = 0; x < size_horizontal_px; ++x) {
		if (GetPixel(x, 0) != 0) {
			return true;
		}
	}

	return false;
}

bool CharacterBlock::IsTouchingBottom() const
{
	for (uint32_t x = 0; x < size_horizontal_px; ++x) {
		if (GetPixel(x, size_vertical_px - 1) != 0) {
			return true;
		}
	}

	return false;
}

float CharacterBlock::GetDistanceLeft() const
{
	auto result = static_cast<float>(size_horizontal_px);

	for (uint32_t x = 0; x < size_horizontal_px; ++x) {
		bool found = false;
		for (uint32_t y = 0; y < size_vertical_px; ++y) {
			const auto value = GetPixel(x, y);
			if (value == 0) {
				continue;
			}

			found = true;

			auto distance = static_cast<float>(x);
			distance += (UINT8_MAX - value) /
			            static_cast<float>(UINT8_MAX);

			result = std::min(result, distance);
		}

		if (found) {
			return result;
		}
	}

	return 0.0f;
}

float CharacterBlock::GetDistanceRight() const
{
	auto result = static_cast<float>(size_horizontal_px);

	for (uint32_t x = 0; x < size_horizontal_px; ++x) {
		bool found = false;
		for (uint32_t y = 0; y < size_vertical_px; ++y) {
			const auto value = GetPixel(size_horizontal_px - x - 1, y);
			if (value == 0) {
				continue;
			}

			found = true;

			auto distance = static_cast<float>(x);
			distance += (UINT8_MAX - value) /
			            static_cast<float>(UINT8_MAX);

			result = std::min(result, distance);
		}

		if (found) {
			return result;
		}
	}

	return 0.0f;
}

float CharacterBlock::GetDistanceTop() const
{
	auto result = static_cast<float>(size_vertical_px);

	for (uint32_t y = 0; y < size_vertical_px; ++y) {
		bool found = false;
		for (uint32_t x = 0; x < size_horizontal_px; ++x) {
			const auto value = GetPixel(x, y);
			if (value == 0) {
				continue;
			}

			found = true;

			auto distance = static_cast<float>(y);
			distance += (UINT8_MAX - value) /
			            static_cast<float>(UINT8_MAX);

			result = std::min(result, distance);
		}

		if (found) {
			return result;
		}
	}

	return 0.0f;
}

float CharacterBlock::GetDistanceBottom() const
{
	auto result = static_cast<float>(size_vertical_px);

	for (uint32_t y = 0; y < size_vertical_px; ++y) {
		bool found = false;
		for (uint32_t x = 0; x < size_horizontal_px; ++x) {
			const auto value = GetPixel(x, size_vertical_px - y - 1);
			if (value == 0) {
				continue;
			}

			found = true;

			auto distance = static_cast<float>(y);
			distance += (UINT8_MAX - value) /
			            static_cast<float>(UINT8_MAX);

			result = std::min(result, distance);
		}

		if (found) {
			return result;
		}
	}

	return 0.0f;
}

float CharacterBlock::GetContentWidth() const
{
	auto value = static_cast<float>(size_horizontal_px);
	value -= GetDistanceLeft();
	value -= GetDistanceRight();

	return std::max(0.0f, value);
}

float CharacterBlock::GetContentHeight() const
{
	auto value = static_cast<float>(size_vertical_px);
	value -= GetDistanceTop();
	value -= GetDistanceBottom();

	return std::max(0.0f, value);
}

void CharacterBlock::Blend(const CharacterBlock& other)
{
	const auto limit_horizontal_px = std::min(size_horizontal_px,
	                                          other.size_horizontal_px);
	const auto limit_vertical_px   = std::min(size_vertical_px,
	                                          other.size_vertical_px);

	for (uint32_t x = 0; x < limit_horizontal_px; ++x) {
		for (uint32_t y = 0; y < limit_vertical_px; ++y) {
			const auto other_value = other.GetPixel(x, y);
			SetPixel(x, y, std::max(GetPixel(x, y), other_value));
		}
	}
}

void CharacterBlock::MirrorHorizontally()
{
	for (uint32_t x1 = 0; x1 < size_horizontal_px / 2; ++x1) {
		for (uint32_t y = 0; y < size_vertical_px; ++y) {
			const uint32_t x2  = size_horizontal_px - x1 - 1;
			const auto value_1 = GetPixel(x1, y);
			const auto value_2 = GetPixel(x2, y);
			SetPixel(x1, y, value_2);
			SetPixel(x2, y, value_1);
		}
	}
}

uint32_t CharacterBlock::GetSharpenDepth(const uint32_t depth_check_px) const
{
	constexpr uint32_t MinSizeToProcess = 8;
	constexpr float DepthProportion     = 0.2f;

	static_assert(DepthProportion * MinSizeToProcess >= 1.0f);

	if (depth_check_px < MinSizeToProcess) {
		return 0;
	}

	const auto depth_px = std::lround(DepthProportion *
	                                  static_cast<float>(depth_check_px));

	constexpr uint32_t Min = 1;
	return std::max(Min, static_cast<uint32_t>(depth_px - 1));
}

void CharacterBlock::SharpenTop(const uint32_t depth_vertical_px)
{
	if (depth_vertical_px == 0) {
		return;
	}

	for (uint32_t x = 0; x < size_horizontal_px; ++x) {
		const uint32_t y_border = 0;
		const uint32_t y_limit  = y_border + depth_vertical_px;

		auto value = GetPixel(x, y_border);
		for (uint32_t y = y_border + 1; y <= y_limit; ++y) {
			value = std::max(value, GetPixel(x, y));
		}

		for (uint32_t y = y_border; y <= y_limit; ++y) {
			SetPixel(x, y, value);
		}
	}
}

void CharacterBlock::SharpenBottom(const uint32_t depth_vertical_px)
{
	if (depth_vertical_px == 0) {
		return;
	}

	for (uint32_t x = 0; x < size_horizontal_px; ++x) {
		const uint32_t y_border = size_vertical_px - 1;
		const uint32_t y_limit  = y_border - depth_vertical_px;

		auto value = GetPixel(x, y_border);
		for (uint32_t y = y_border - 1; y >= y_limit; --y) {
			value = std::max(value, GetPixel(x, y));
		}

		for (uint32_t y = y_border; y >= y_limit; --y) {
			SetPixel(x, y, value);
		}
	}
}

void CharacterBlock::SharpenLeft(const uint32_t depth_horizontal_px)
{
	if (depth_horizontal_px == 0) {
		return;
	}

	for (uint32_t y = 0; y < size_vertical_px; ++y) {
		const uint32_t x_border = 0;
		const uint32_t x_limit  = x_border + depth_horizontal_px;

		auto value = GetPixel(x_border, y);
		for (uint32_t x = x_border + 1; x <= x_limit; ++x) {
			value = std::max(value, GetPixel(x, y));
		}

		for (uint32_t x = x_border; x <= x_limit; ++x) {
			SetPixel(x, y, value);
		}
	}
}

void CharacterBlock::SharpenRight(const uint32_t depth_horizontal_px)
{
	if (depth_horizontal_px == 0) {
		return;
	}

	for (uint32_t y = 0; y < size_vertical_px; ++y) {
		const uint32_t x_border = size_horizontal_px - 1;
		const uint32_t x_limit  = x_border - depth_horizontal_px;

		auto value = GetPixel(x_border, y);
		for (uint32_t x = x_border - 1; x >= x_limit; --x) {
			value = std::max(value, GetPixel(x, y));
		}

		for (uint32_t x = x_border; x >= x_limit; --x) {
			SetPixel(x, y, value);
		}
	}
}

void CharacterBlock::SharpenAllBorders()
{
	const auto depth_horizontal_px = GetSharpenDepth(size_horizontal_px);
	const auto depth_vertical_px   = GetSharpenDepth(size_vertical_px);

	SharpenTop(depth_vertical_px);
	SharpenBottom(depth_vertical_px);
	SharpenLeft(depth_horizontal_px);
	SharpenRight(depth_horizontal_px);
}

void CharacterBlock::SharpenOnlyTop()
{
	SharpenTop(GetSharpenDepth(size_vertical_px));
}

void CharacterBlock::SharpenOnlyBottom()
{
	SharpenBottom(GetSharpenDepth(size_vertical_px));
}

} // namespace TrueType
