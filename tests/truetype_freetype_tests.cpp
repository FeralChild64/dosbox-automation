// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//

#include "gui/truetype_freetype.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "augra/log.h"
#include "test_temp_dir.h"

namespace {

class FreeTypeNewFace : public ::testing::Test {
protected:
	void SetUp() override
	{
		augra::Logger::instance().add_sink([this](augra::LogLevel,
		                                          const char* component,
		                                          const std::string& message) {
			if (std::string_view(component) == "ttf") {
				warnings.push_back(message);
			}
		});
		dir = TestTempDir::MakeUnique("truetype_freetype_");
		ASSERT_FALSE(dir.empty());
	}

	void TearDown() override
	{
		augra::Logger::instance().clear_sinks();
		std::error_code ec = {};
		std_fs::remove_all(dir, ec);
	}

	bool Logged(const std::string_view text) const
	{
		for (const auto& warning : warnings) {
			if (warning.find(text) != std::string::npos) {
				return true;
			}
		}
		return false;
	}

	std_fs::path dir                  = {};
	std::vector<std::string> warnings = {};
};

TEST_F(FreeTypeNewFace, MissingFileIsReportedAsNotOpened)
{
	FT_Face face = nullptr;

	EXPECT_FALSE(FreeType::NewFace(dir / "missing.ttf", 0, &face));
	EXPECT_TRUE(Logged("Could not open font file"));
	EXPECT_FALSE(Logged("too large"));
}

TEST_F(FreeTypeNewFace, DirectoryIsRejectedWithoutASizeClaim)
{
	FT_Face face = nullptr;

	EXPECT_FALSE(FreeType::NewFace(dir, 0, &face));
	EXPECT_TRUE(Logged("Could not open font file"));
	EXPECT_FALSE(Logged("too large"));
}

} // namespace
