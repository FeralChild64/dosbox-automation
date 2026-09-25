// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//

#include "misc/cross.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <string>

#include "utils/env_utils.h"

namespace {

#if defined(WIN32)
TEST(StandardFontDirs, UnsetWindirGivesNoDirectoryInsteadOfCrashing)
{
	const auto saved = get_env_var("WINDIR");
	// An empty value removes the variable on Windows
	_putenv_s("WINDIR", "");

	const auto directories = get_standard_font_dirs();

	_putenv_s("WINDIR", saved.c_str());
	EXPECT_TRUE(directories.empty());
}
#endif

} // namespace
