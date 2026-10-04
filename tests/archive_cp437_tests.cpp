// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//
#include "dos/archive/cp437.h"

#include <gtest/gtest.h>

#include "misc/cross.h"
#include "misc/unicode.h"

using ArchiveMount::Cp437ToUtf8;

// The helper process cannot load the engine's code page resources, so it
// carries its own table; both must name every file the same way.
TEST(Cp437, AgreesWithTheEngineDecoderOnEveryByte)
{
	init_config_dir();
	for (int b = 0; b < 256; ++b) {
		const auto raw = std::string(1, static_cast<char>(b));
		EXPECT_EQ(Cp437ToUtf8(raw), dos_437_to_fs_utf8(raw)) << "byte " << b;
	}
}

TEST(Cp437, AgreesOnAMixedName)
{
	init_config_dir();
	const auto raw = std::string(
	        "G\x8e"
	        "ME\x01\x7f\xff.EXE");
	EXPECT_EQ(Cp437ToUtf8(raw), dos_437_to_fs_utf8(raw));
	EXPECT_EQ(Cp437ToUtf8(raw), "G\xc3\x84ME??\xc2\xa0.EXE");
}

TEST(Cp437, EmptyStaysEmpty)
{
	EXPECT_EQ(Cp437ToUtf8(""), "");
}
