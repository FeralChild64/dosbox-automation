// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//
#include "dos/archive/archive_lock.h"

#include "test_temp_dir.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
using ArchiveMount::FileLock;

namespace {

class ArchiveLockTest : public ::testing::Test {
protected:
	void SetUp() override
	{
		dir = TestTempDir::MakeUnique("archive_lock_");
		ASSERT_FALSE(dir.empty());
		path = dir / ".lock";
	}
	void TearDown() override
	{
		std::error_code ec = {};
		fs::remove_all(dir, ec);
	}
	fs::path dir  = {};
	fs::path path = {};
};

TEST_F(ArchiveLockTest, CreatesTheFileAndTwoSharedHoldersCoexist)
{
	auto a = FileLock::TryAcquire(path, FileLock::Mode::Shared);
	ASSERT_TRUE(a);
	EXPECT_TRUE(fs::is_regular_file(path));
	auto b = FileLock::TryAcquire(path, FileLock::Mode::Shared);
	EXPECT_TRUE(b);
	EXPECT_EQ(a->GetMode(), FileLock::Mode::Shared);
}

TEST_F(ArchiveLockTest, ExclusiveIsRefusedWhileSharedIsHeldAndTheOtherWayRound)
{
	{
		auto shared = FileLock::TryAcquire(path, FileLock::Mode::Shared);
		ASSERT_TRUE(shared);
		EXPECT_FALSE(FileLock::TryAcquire(path, FileLock::Mode::Exclusive));
	}
	auto exclusive = FileLock::TryAcquire(path, FileLock::Mode::Exclusive);
	ASSERT_TRUE(exclusive);
	EXPECT_FALSE(FileLock::TryAcquire(path, FileLock::Mode::Shared));
	EXPECT_FALSE(FileLock::TryAcquire(path, FileLock::Mode::Exclusive));
}

TEST_F(ArchiveLockTest, ReleaseOnDestructionAndOnMove)
{
	auto first = FileLock::TryAcquire(path, FileLock::Mode::Exclusive);
	ASSERT_TRUE(first);
	FileLock moved = std::move(*first);
	EXPECT_FALSE(FileLock::TryAcquire(path, FileLock::Mode::Shared));
	{
		FileLock sink = std::move(moved);
		(void)sink;
	}
	EXPECT_TRUE(FileLock::TryAcquire(path, FileLock::Mode::Shared));
}

TEST_F(ArchiveLockTest, RefusesAMissingParent)
{
	EXPECT_FALSE(FileLock::TryAcquire(dir / "nowhere" / ".lock",
	                                  FileLock::Mode::Shared));
}

#if !defined(WIN32)
TEST_F(ArchiveLockTest, NeverFollowsALinkAtThePath)
{
	const auto victim = dir / "victim";
	{
		std::ofstream out(victim);
		out << "x";
	}
	fs::create_symlink(victim, path);
	EXPECT_FALSE(FileLock::TryAcquire(path, FileLock::Mode::Shared));
}
#endif

} // namespace
