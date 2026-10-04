// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//
#include "config/setup.h"
#include "dosbox_test_fixture.h"

#include <gtest/gtest.h>

#include <string>

namespace {

class DosSettings : public DOSBoxTestFixture {};

TEST_F(DosSettings, ArchiveSettingsExistWithTheDesignDefaults)
{
	const auto section = get_section("dos");
	ASSERT_NE(section, nullptr);
	const std::pair<const char*, const char*> expected[] = {
	        {    "archive_cache_path",       ""},
	        {"archive_cache_max_size",   "4096"},
	        {"archive_cache_min_free",   "2048"},
	        {     "archive_cache_ttl",      "7"},
	        {  "archive_memory_limit",     "16"},
	        {   "archive_max_entries",  "65536"},
	        {      "archive_max_size",   "4096"},
	        {    "archive_sequential", "refuse"},
	        {  "archive_sidecar_path",       ""},
	};
	for (const auto& [name, value] : expected) {
		const auto property = section->GetProperty(name);
		ASSERT_NE(property, nullptr) << name;
		EXPECT_EQ(property->GetDefaultValue().ToString(), value) << name;
		EXPECT_FALSE(property->GetHelp().empty()) << name;
		EXPECT_EQ(property->GetHelp().find('%'), std::string::npos) << name;
	}
}

TEST_F(DosSettings, ArchiveSettingsRefuseValuesOutsideTheirRange)
{
	const auto section = get_section("dos");
	ASSERT_NE(section, nullptr);
	auto ttl = section->GetProperty("archive_cache_ttl");
	ASSERT_NE(ttl, nullptr);
	// PropInt warns and restores the default for a value outside its
	// range (setup.cpp, IsValidValue); SetValue still reports success.
	EXPECT_TRUE(ttl->SetValue("31"));
	EXPECT_EQ(section->GetInt("archive_cache_ttl"), 7);
	EXPECT_TRUE(ttl->SetValue("30"));
	EXPECT_EQ(section->GetInt("archive_cache_ttl"), 30);
	auto seq = section->GetProperty("archive_sequential");
	ASSERT_NE(seq, nullptr);
	EXPECT_FALSE(seq->SetValue("unpack"));
	EXPECT_TRUE(seq->SetValue("cache"));
	EXPECT_EQ(section->GetString("archive_sequential"), "cache");
}

} // namespace
