// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//
#include "misc/cross.h"

#include "utils/fs_utils.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <string>

namespace {

void SetEnv(const char* name, const char* value)
{
#if defined(WIN32)
	_putenv_s(name, value);
#else
	setenv(name, value, 1);
#endif
}

void UnsetEnv(const char* name)
{
#if defined(WIN32)
	_putenv_s(name, "");
#else
	unsetenv(name);
#endif
}

#if defined(MACOSX)
TEST(CacheDir, MacUsesLibraryCaches)
{
	const auto dir = get_cache_dir().string();
	EXPECT_TRUE(dir.ends_with("/Library/Caches/dosbox-automation")) << dir;
}
#else
TEST(CacheDir, XdgCacheHomeOverridesTheDefault)
{
	SetEnv("XDG_CACHE_HOME", "/tmp/override-cache");
	const auto dir = get_cache_dir();
	UnsetEnv("XDG_CACHE_HOME");
	EXPECT_EQ(dir, std::filesystem::path("/tmp/override-cache") / "dosbox-automation");
}
#endif

#if !defined(WIN32) && !defined(MACOSX)
TEST(CacheDir, UnsetFallsBackToDotCache)
{
	UnsetEnv("XDG_CACHE_HOME");
	const auto dir = get_cache_dir().string();
	EXPECT_TRUE(dir.ends_with("/.cache/dosbox-automation")) << dir;
	EXPECT_TRUE(get_xdg_cache_home().string().ends_with("/.cache"));
}
#endif

#if defined(WIN32)
TEST(CacheDir, WindowsUsesLocalAppDataCache)
{
	UnsetEnv("XDG_CACHE_HOME");
	const auto dir = get_cache_dir().string();
	EXPECT_TRUE(dir.ends_with("\\dosbox-automation\\cache")) << dir;
}
#endif

#if !defined(WIN32) && !defined(MACOSX)
TEST(DataDir, XdgDataHomeOverridesAndFallsBackToLocalShare)
{
	SetEnv("XDG_DATA_HOME", "/tmp/override-data");
	const auto dir = get_data_dir();
	UnsetEnv("XDG_DATA_HOME");
	EXPECT_EQ(dir, std::filesystem::path("/tmp/override-data") / "dosbox-automation");
	EXPECT_TRUE(get_data_dir().string().ends_with(
	        "/.local/share/dosbox-automation"));
}
#endif
#if defined(MACOSX)
TEST(DataDir, MacUsesApplicationSupport)
{
	EXPECT_TRUE(get_data_dir().string().ends_with(
	        "/Library/Application Support/dosbox-automation"));
}
#endif
#if defined(WIN32)
TEST(DataDir, WindowsUsesLocalAppData)
{
	UnsetEnv("XDG_DATA_HOME");
	EXPECT_TRUE(get_data_dir().string().ends_with("\\dosbox-automation"));
}
#endif

} // namespace
