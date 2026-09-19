// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//

#include <gtest/gtest.h>

#include "dosbox_test_fixture.h"
#include "misc/unicode.h"

namespace {

class FsUtf8ToDos437Test : public DOSBoxTestFixture {};

// Regression: unmappable UTF-8 characters returned an empty string,
// which corrupted the drive cache (empty shortname key).
// The fix substitutes '?' for unmappable characters.
TEST_F(FsUtf8ToDos437Test, UnmappableCharProducesPlaceholder)
{
	// U+4E16 (CJK, not in CP437)
	const std::string input = "\xe4\xb8\x96";
	const auto result = fs_utf8_to_dos_437(input);

	EXPECT_FALSE(result.empty());
	EXPECT_EQ(result, "?");
}

TEST_F(FsUtf8ToDos437Test, MixedMappableAndUnmappable)
{
	// "A" + U+4E16 + "B"
	const std::string input = "A\xe4\xb8\x96" "B";
	const auto result = fs_utf8_to_dos_437(input);

	EXPECT_FALSE(result.empty());
	EXPECT_EQ(result, "A?B");
}

TEST_F(FsUtf8ToDos437Test, PureAsciiUnchanged)
{
	const auto result = fs_utf8_to_dos_437("HELLO.TXT");
	EXPECT_EQ(result, "HELLO.TXT");
}

TEST_F(FsUtf8ToDos437Test, EmptyInputReturnsEmpty)
{
	const auto result = fs_utf8_to_dos_437("");
	EXPECT_TRUE(result.empty());
}

TEST_F(FsUtf8ToDos437Test, ControlCodeProducesPlaceholder)
{
	// Tab character embedded in a filename
	const std::string input = "AB\x09" "C";
	const auto result = fs_utf8_to_dos_437(input);

	EXPECT_FALSE(result.empty());
	EXPECT_EQ(result, "AB?C");
}

// Verify that the function never returns empty for non-empty input,
// which is the contract the drive cache depends on.
TEST_F(FsUtf8ToDos437Test, NonEmptyInputNeverReturnsEmpty)
{
	// All unmappable CJK characters
	const std::string input = "\xe4\xb8\x96\xe7\x95\x8c";
	const auto result = fs_utf8_to_dos_437(input);

	EXPECT_FALSE(result.empty());
}

} // namespace
