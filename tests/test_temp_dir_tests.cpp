// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//

#include "test_temp_dir.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "dos/programs/mount_policy.h"

namespace {

namespace fs = std::filesystem;

class TestTempDirTest : public testing::Test {
protected:
	void SetUp() override
	{
		parent = TestTempDir::MakeUnique("test_temp_dir_",
		                                 fs::temp_directory_path());
		ASSERT_FALSE(parent.empty());
	}

	void TearDown() override
	{
		std::error_code ec = {};
		fs::remove_all(parent, ec);
	}

	fs::path parent = {};
};

TEST_F(TestTempDirTest, MakeUniqueCreatesAnEmptyDirectoryUnderTheBase)
{
	const auto dir = TestTempDir::MakeUnique("probe_", parent);

	ASSERT_FALSE(dir.empty());
	EXPECT_EQ(dir.parent_path(), parent);
	EXPECT_TRUE(fs::is_directory(dir));
	EXPECT_TRUE(fs::is_empty(dir));
	EXPECT_EQ(dir.filename().string().rfind("probe_", 0), 0u);
}

TEST_F(TestTempDirTest, MakeUniqueNeverHandsOutTheSameDirectoryTwice)
{
	const auto first  = TestTempDir::MakeUnique("probe_", parent);
	const auto second = TestTempDir::MakeUnique("probe_", parent);

	ASSERT_FALSE(first.empty());
	ASSERT_FALSE(second.empty());
	EXPECT_NE(first, second);
}

TEST_F(TestTempDirTest, MakeUniqueCreatesAMissingBase)
{
	const auto base = parent / "not" / "there" / "yet";

	const auto dir = TestTempDir::MakeUnique("probe_", base);

	ASSERT_FALSE(dir.empty());
	EXPECT_EQ(dir.parent_path(), base);
	EXPECT_TRUE(fs::is_directory(dir));
}

// The mount policy compares against canonical roots, and the tests pass
// fixture paths as roots. CMake hands Windows a forward-slash base.
TEST_F(TestTempDirTest, MakeUniqueReturnsACanonicalPath)
{
	fs::create_directory(parent / "a");
	const auto base = parent / "a" / ".." / "b";

	const auto dir = TestTempDir::MakeUnique("probe_", base);

	ASSERT_FALSE(dir.empty());
	EXPECT_EQ(dir, fs::canonical(dir));
	EXPECT_EQ(dir.parent_path(), fs::canonical(parent / "b"));
}

TEST_F(TestTempDirTest, MakeUniqueReturnsEmptyWhenTheBaseIsAFile)
{
	const auto file = parent / "plain_file";
	std::ofstream(file) << "x";

	EXPECT_TRUE(TestTempDir::MakeUnique("probe_", file).empty());
}

TEST_F(TestTempDirTest, MakeUniqueRejectsAPrefixThatLeavesTheBase)
{
	// Without it the separator cases fail on a missing parent, not on
	// the prefix check
	fs::create_directory(parent / "sub");

	EXPECT_TRUE(TestTempDir::MakeUnique("../escape_", parent).empty());
	EXPECT_TRUE(TestTempDir::MakeUnique("sub/escape_", parent).empty());
	EXPECT_TRUE(TestTempDir::MakeUnique("sub\\escape_", parent).empty());
	EXPECT_TRUE(TestTempDir::MakeUnique("C:escape_", parent).empty());
}

// The mount suites create their fixtures here and mount them; a base on
// the system-path denylist blocks every one of them (ada-925x: %TEMP% on
// Windows).
TEST(TestTempDir, BaseIsNotOnTheMountDenylist)
{
	const auto base = TestTempDir::Base();

	ASSERT_FALSE(base.empty());
	std::error_code ec = {};
	fs::create_directories(base, ec);
	ASSERT_FALSE(ec) << ec.message();

	EXPECT_FALSE(MountPolicy::IsUnderSystemPath(fs::canonical(base)))
	        << base.string();
}

} // namespace
