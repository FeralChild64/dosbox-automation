// SPDX-FileCopyrightText:  2026 dosbox-automation Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "gui/truetype_output.h"

#include <gtest/gtest.h>

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

} // namespace
