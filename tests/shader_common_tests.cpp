// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//
#include "gui/render/private/shader_common.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "utils/rect.h"

namespace {

std::string ReadWholeFile(const std::filesystem::path& path)
{
	std::ifstream in(path, std::ios::binary);
	return {std::istreambuf_iterator<char>(in), {}};
}

} // namespace

// ----------------------------------------------------------------------------
// GetPassOutputOrigin: the window position the final pass draws at, which
// the shaders subtract from gl_FragCoord so masks start at the image corner
// ----------------------------------------------------------------------------

TEST(ShaderOutputOrigin, IntermediatePassIsAlwaysZero)
{
	const DosBox::Rect out_size = {266.5f, 199.5f, 1600.0f, 1200.0f};

	const auto origin = GetPassOutputOrigin(out_size, false);

	EXPECT_EQ(origin.x, 0);
	EXPECT_EQ(origin.y, 0);
}

TEST(ShaderOutputOrigin, FinalPassTruncatesLikeGlViewport)
{
	const DosBox::Rect viewport = {266.5f, 199.5f, 1600.0f, 1200.0f};

	const auto origin = GetPassOutputOrigin(viewport, true);

	EXPECT_EQ(origin.x, 266);
	EXPECT_EQ(origin.y, 199);
}

TEST(ShaderOutputOrigin, FinalPassAtWindowCornerIsZero)
{
	const DosBox::Rect viewport = {0.0f, 0.0f, 640.0f, 480.0f};

	const auto origin = GetPassOutputOrigin(viewport, true);

	EXPECT_EQ(origin.x, 0);
	EXPECT_EQ(origin.y, 0);
}

// A relative viewport above 100% makes the image larger than the window,
// so the centred origin goes negative (render.cpp, MaxRelativeScaleFactor).
TEST(ShaderOutputOrigin, FinalPassTruncatesANegativeOriginLikeGlViewport)
{
	const DosBox::Rect viewport = {-120.5f, -270.75f, 2160.0f, 1620.0f};

	const auto origin = GetPassOutputOrigin(viewport, true);

	EXPECT_EQ(origin.x, static_cast<int>(viewport.x));
	EXPECT_EQ(origin.y, static_cast<int>(viewport.y));
	EXPECT_EQ(origin.x, -120);
	EXPECT_EQ(origin.y, -270);
}

// ----------------------------------------------------------------------------
// Every bundled shader that reads the window-relative fragment coordinate
// must anchor it to the image through OUTPUT_ORIGIN. Runs from the source
// tree (ctest sets the working directory).
// ----------------------------------------------------------------------------

TEST(ShaderSources, FragCoordReadersAnchorToOutputOrigin)
{
	const std::filesystem::path shader_dir = "resources/shaders";
	ASSERT_TRUE(std::filesystem::is_directory(shader_dir))
	        << "run from the source tree";

	std::vector<std::string> readers    = {};
	std::vector<std::string> unanchored = {};

	for (const auto& entry :
	     std::filesystem::recursive_directory_iterator(shader_dir)) {
		if (!entry.is_regular_file() || entry.path().extension() != ".glsl") {
			continue;
		}
		const auto source = ReadWholeFile(entry.path());
		if (source.find("gl_FragCoord") == std::string::npos) {
			continue;
		}
		readers.push_back(entry.path().string());

		const bool declares = source.find("uniform vec2 OUTPUT_ORIGIN;") !=
		                      std::string::npos;
		const bool subtracts = source.find("gl_FragCoord.xy - OUTPUT_ORIGIN") !=
		                       std::string::npos;
		if (!declares || !subtracts) {
			unanchored.push_back(entry.path().string());
		}
	}

	// The three CRT mask shaders read gl_FragCoord today; a count below
	// that means the walk looked at the wrong tree.
	EXPECT_GE(readers.size(), 3u);
	EXPECT_TRUE(unanchored.empty())
	        << "shaders reading gl_FragCoord without OUTPUT_ORIGIN: "
	        << ::testing::PrintToString(unanchored);
}
