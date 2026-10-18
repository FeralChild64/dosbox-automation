// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//
#include "dos/dos_names.h"

#include <gtest/gtest.h>

#include <string>

namespace {

using DosNames::WithoutEmptyExtensions;

TEST(DosNames, AnEmptyExtensionIsStrippedFromEveryComponent)
{
	EXPECT_EQ(WithoutEmptyExtensions("SKIEGA."), "SKIEGA");
	EXPECT_EQ(WithoutEmptyExtensions("DATA.\\LEVEL."), "DATA\\LEVEL");
	EXPECT_EQ(WithoutEmptyExtensions("GAME\\X."), "GAME\\X");
}

TEST(DosNames, OnlyAnEmptyExtensionIsStripped)
{
	EXPECT_EQ(WithoutEmptyExtensions("A.B."), "A.B.");
	EXPECT_EQ(WithoutEmptyExtensions(".."), "..");
	EXPECT_EQ(WithoutEmptyExtensions("."), ".");
	EXPECT_EQ(WithoutEmptyExtensions("GAME\\..\\X"), "GAME\\..\\X");
	EXPECT_EQ(WithoutEmptyExtensions("README.TXT"), "README.TXT");
	EXPECT_EQ(WithoutEmptyExtensions(""), "");
}

} // namespace
