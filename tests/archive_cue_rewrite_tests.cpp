// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//
#include "dos/archive/cue_rewrite.h"

#include <gtest/gtest.h>

#include <map>
#include <string>

using namespace ArchiveMount;

namespace {

std::filesystem::path Host(const char* leaf)
{
	return std::filesystem::path("/cache/g/game-1a2b3c4d/files") / leaf;
}

// Answers only for the names the archive holds.
CueFileResolver Members(std::map<std::string, std::string> names)
{
	return [names](const std::string_view name) -> std::optional<std::filesystem::path> {
		const auto found = names.find(std::string(name));
		if (found == names.end()) {
			return {};
		}
		return Host(found->second.c_str());
	};
}

TEST(CueRewrite, RewritesAQuotedFileLine)
{
	const auto out = RewriteCueSheet("FILE \"Game (USA).bin\" BINARY\n  TRACK 01 MODE1/2352\n    INDEX 01 00:00:00\n",
	                                 Members({
	                                         {"Game (USA).bin", "3"}
        }));
	ASSERT_TRUE(out.ok) << out.refused;
	EXPECT_EQ(out.text,
	          "FILE \"" + Host("3").string() +
	                  "\" BINARY\n  TRACK 01 MODE1/2352\n    INDEX 01 00:00:00\n");
}

TEST(CueRewrite, RewritesABareNameAndKeepsTheIndent)
{
	const auto out = RewriteCueSheet("  FILE track02.wav WAVE\r\n",
	                                 Members({
	                                         {"track02.wav", "4"}
        }));
	ASSERT_TRUE(out.ok) << out.refused;
	EXPECT_EQ(out.text, "  FILE \"" + Host("4").string() + "\" WAVE\n");
}

TEST(CueRewrite, TheKeywordIsMatchedWithoutCase)
{
	const auto out = RewriteCueSheet("file \"/etc/passwd\" BINARY\n",
	                                 Members({}));
	EXPECT_FALSE(out.ok);
	EXPECT_NE(out.refused.find("/etc/passwd"), std::string::npos);
}

TEST(CueRewrite, ANameTheArchiveDoesNotHoldIsRefused)
{
	for (const auto* line : {"FILE \"../../outside.bin\" BINARY\n",
	                         "FILE \"C:\\game\\track.bin\" BINARY\n",
	                         "FILE missing.bin BINARY\n"}) {
		EXPECT_FALSE(RewriteCueSheet(line,
		                             Members({
		                                     {"track.bin", "1"}
                }))
		                     .ok)
		        << line;
	}
}

TEST(CueRewrite, ASheetWithoutFileLinesIsRefused)
{
	EXPECT_FALSE(RewriteCueSheet("TRACK 01 MODE1/2048\n", Members({})).ok);
}

TEST(CueRewrite, AnUnterminatedQuoteIsRefused)
{
	EXPECT_FALSE(RewriteCueSheet("FILE \"track.bin BINARY\n",
	                             Members({
	                                     {"track.bin", "1"}
        }))
	                     .ok);
}

TEST(CueRewrite, AnEmptyNameIsRefused)
{
	EXPECT_FALSE(RewriteCueSheet("FILE \"\" BINARY\n",
	                             Members({
	                                     {"", "1"}
        }))
	                     .ok);
}

TEST(CueRewrite, ANulByteIsRefused)
{
	const std::string cue = std::string("FILE \"a.bin\" BINARY\n") + '\0' + "\n";
	EXPECT_FALSE(RewriteCueSheet(cue,
	                             Members({
	                                     {"a.bin", "1"}
        }))
	                     .ok);
}

TEST(CueRewrite, ALineTheLoaderCannotReadIsRefused)
{
	const auto cue = std::string("REM ") + std::string(600, 'x') +
	                 "\nFILE \"a.bin\" BINARY\n";
	EXPECT_FALSE(RewriteCueSheet(cue,
	                             Members({
	                                     {"a.bin", "1"}
        }))
	                     .ok);
}

TEST(CueRewrite, AHostPathWithAQuoteIsRefused)
{
	const CueFileResolver quoted = [](std::string_view) {
		return std::optional<std::filesystem::path>("/cache/a\"b/1");
	};
	EXPECT_FALSE(RewriteCueSheet("FILE \"a.bin\" BINARY\n", quoted).ok);
}

TEST(CueRewrite, AHostPathOverTheLoadersNameLimitIsRefused)
{
	const CueFileResolver longer = [](std::string_view) {
		return std::optional<std::filesystem::path>(
		        "/" + std::string(300, 'c'));
	};
	EXPECT_FALSE(RewriteCueSheet("FILE \"a.bin\" BINARY\n", longer).ok);
}

// The loader reads keywords with operator>>, which skips every isspace
// character; a blank the rewriter does not know would carry a line past it.
TEST(CueRewrite, BlanksAreTheLoadersWhitespace)
{
	const auto out = RewriteCueSheet("\fFILE\v\"a.bin\"\tBINARY\r\n",
	                                 Members({
	                                         {"a.bin", "1"}
        }));
	ASSERT_TRUE(out.ok) << out.refused;
	EXPECT_EQ(out.text.find("a.bin"), std::string::npos) << out.text;
	EXPECT_EQ(out.text, "FILE \"" + Host("1").string() + "\" BINARY\n");
}

// GetCueString takes the text up to the next quote: "a"b.bin" names a.
TEST(CueRewrite, AQuoteFollowedByTextIsRefused)
{
	EXPECT_FALSE(RewriteCueSheet("FILE \"a\"b.bin\" BINARY\n",
	                             Members({
	                                     {"a", "1"}
        }))
	                     .ok);
}

TEST(CueRewrite, TwoFilesAreBothRewritten)
{
	const auto out = RewriteCueSheet(
	        "FILE \"t1.bin\" BINARY\n  TRACK 01 MODE1/2352\n"
	        "FILE \"t2.bin\" BINARY\n  TRACK 02 AUDIO\n",
	        Members({
	                {"t1.bin", "1"},
                        {"t2.bin", "2"}
        }));
	ASSERT_TRUE(out.ok) << out.refused;
	EXPECT_NE(out.text.find(Host("1").string()), std::string::npos);
	EXPECT_NE(out.text.find(Host("2").string()), std::string::npos);
	EXPECT_EQ(out.text.find("t1.bin"), std::string::npos);
}

} // namespace
