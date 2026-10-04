// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//
#include "dos/archive/random_name.h"

#include <gtest/gtest.h>

#include <set>
#include <string>

using ArchiveMount::RandomHex;

TEST(RandomHex, EightLowerCaseHexDigitsPerWord)
{
	for (const int words : {1, 2, 4}) {
		const auto name = RandomHex(words);
		EXPECT_EQ(name.size(), static_cast<size_t>(words * 8));
		EXPECT_EQ(name.find_first_not_of("0123456789abcdef"),
		          std::string::npos)
		        << name;
	}
}

TEST(RandomHex, NamesDoNotRepeat)
{
	std::set<std::string> seen = {};
	for (int i = 0; i < 1000; ++i) {
		seen.insert(RandomHex(2));
	}
	EXPECT_EQ(seen.size(), 1000u);
}
