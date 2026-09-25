// SPDX-FileCopyrightText:  2026 dosbox-automation Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "gui/truetype_output.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <fstream>
#include <random>
#include <string>

namespace {

TEST(TrueType, ShortenFontName_NoShortening)
{
	const std::string input = "FooBar.ttf";

	EXPECT_EQ(TTF_ShortenFontName(input, 20), input);
}

TEST(TrueType, ShortenFontName_WithExtesion)
{
	const std::string input    = "Hazeltine.Superluminal.Drive.ttf";
	const std::string expected = "Hazeltine.(...).ttf";

	EXPECT_EQ(TTF_ShortenFontName(input, 19), expected);
}

TEST(TrueType, ShortenFontName_WithoutExtesion)
{
	const std::string input    = "Locarno_Superluminal_Drive";
	const std::string expected = "Locarno(...)";

	EXPECT_EQ(TTF_ShortenFontName(input, 12), expected);
}

TEST(TrueType, ShortenFontName_WithLongExtesion)
{
	const std::string input    = "Books.The_Academy_Series";
	const std::string expected = "Books.The(...)";

	EXPECT_EQ(TTF_ShortenFontName(input, 14), expected);
}

TEST(TrueType, ShortenFontName_WithShortStem)
{
	const std::string input    = "Book.ttf";
	const std::string expected = "B(...)";

	EXPECT_EQ(TTF_ShortenFontName(input, 6), expected);
}

TEST(TrueType, ShortenFontName_ExtremeShortening)
{
	const std::string input    = "FooBar.ttf";
	const std::string expected = "(...)";

	EXPECT_EQ(TTF_ShortenFontName(input, 0), expected);
	EXPECT_EQ(TTF_ShortenFontName(input, 1), expected);
	EXPECT_EQ(TTF_ShortenFontName(input, 5), expected);
}

class FindFontFile : public ::testing::Test {
protected:
	void SetUp() override
	{
		std::random_device rd = {};
		auto dist = std::uniform_int_distribution<uint64_t>();
		for (int attempt = 0; attempt < 16 && root.empty(); ++attempt) {
			const auto candidate = std_fs::temp_directory_path() /
			                       ("find_font_file_" +
			                        std::to_string(dist(rd)));
			std::error_code ec = {};
			if (std_fs::create_directory(candidate, ec) && !ec) {
				std_fs::permissions(candidate,
				                    std_fs::perms::owner_all,
				                    ec);
				root = candidate;
			}
		}
		ASSERT_FALSE(root.empty());
	}

	void TearDown() override
	{
		std::error_code ec = {};
		std_fs::remove_all(root, ec);
	}

	std_fs::path root = {};
};

TEST_F(FindFontFile, FindsAFontTwoLevelsDown)
{
	std_fs::create_directories(root / "a" / "b");
	std::ofstream(root / "a" / "b" / "Font.ttf") << "x";

	EXPECT_EQ(TTF_FindFontFile(root, "Font.ttf"), root / "a" / "b" / "Font.ttf");
}

TEST_F(FindFontFile, StopsBelowTheDepthLimit)
{
	const auto deep = root / "a" / "b" / "c" / "d";
	std_fs::create_directories(deep);
	std::ofstream(deep / "Font.ttf") << "x";

	EXPECT_TRUE(TTF_FindFontFile(root, "Font.ttf", 3).empty());
	EXPECT_EQ(TTF_FindFontFile(root, "Font.ttf", 4), deep / "Font.ttf");
}

#if defined(WIN32) || defined(MACOSX)
TEST_F(FindFontFile, MatchesTheNameCaseInsensitively)
{
	std::ofstream(root / "Font.ttf") << "x";

	EXPECT_EQ(TTF_FindFontFile(root, "FONT.TTF"), root / "Font.ttf");
}
#endif

#if !defined(WIN32)
TEST_F(FindFontFile, SurvivesALinkToTheParentDirectory)
{
	// Points back at root, so the cycle stays inside the fixture
	std_fs::create_directories(root / "sub");
	std_fs::create_directory_symlink("..", root / "sub" / "loop");

	std_fs::path result = {};
	EXPECT_NO_THROW(result = TTF_FindFontFile(root, "missing.ttf"));
	EXPECT_TRUE(result.empty());
}

TEST_F(FindFontFile, SurvivesASelfReferencingSymlinkInTheTree)
{
	std_fs::create_directory_symlink(root / "self", root / "self");

	std_fs::path result = {};
	EXPECT_NO_THROW(result = TTF_FindFontFile(root, "missing.ttf"));
	EXPECT_TRUE(result.empty());
}

TEST_F(FindFontFile, SurvivesASelfReferencingSymlinkAsTheRoot)
{
	std_fs::create_directory_symlink(root / "self", root / "self");

	std_fs::path result = {};
	EXPECT_NO_THROW(result = TTF_FindFontFile(root / "self", "missing.ttf"));
	EXPECT_TRUE(result.empty());
}
#endif

} // namespace
