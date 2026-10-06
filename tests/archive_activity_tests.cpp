// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//
#include "dos/archive/archive_activity.h"

#include "archive_fixtures.h"
#include "test_temp_dir.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace ArchiveMount;
namespace fs = std::filesystem;

class ArchiveActivityTest : public testing::Test {
protected:
	void SetUp() override
	{
		tmp = TestTempDir::MakeUnique("archive_activity_");
		ASSERT_FALSE(tmp.empty());
	}
	void TearDown() override
	{
		std::error_code ec = {};
		fs::remove_all(tmp, ec);
	}
	void Bytes(const fs::path& path, const size_t count)
	{
		ArchiveFixtures::WriteBytes(path, std::vector<uint8_t>(count, 7));
	}
	static Activity::Snapshot Member(const fs::path& target, const int64_t total)
	{
		Activity::Snapshot s = {};
		s.kind               = Activity::Kind::Materializing;
		s.archive            = "doom";
		s.bytes_total        = total;
		s.target             = target;
		return s;
	}

	fs::path tmp = {};
};

TEST_F(ArchiveActivityTest, IdleHasNoActivity)
{
	EXPECT_FALSE(Activity::Current());
	EXPECT_FALSE(Activity::CurrentStatus());
}

TEST_F(ArchiveActivityTest, AGuardSetsTheRecordAndClearsIt)
{
	{
		Activity::Snapshot s = {};
		s.kind               = Activity::Kind::Identifying;
		s.archive            = "doom";
		s.bytes_total        = 4096;
		const Activity::Guard guard(s);
		const auto status = Activity::CurrentStatus();
		ASSERT_TRUE(status);
		EXPECT_EQ(status->activity, "identifying");
		EXPECT_EQ(status->archive, "doom");
		EXPECT_EQ(status->bytes_done, 0);
		EXPECT_EQ(status->bytes_total, 4096);
	}
	EXPECT_FALSE(Activity::Current());
}

TEST_F(ArchiveActivityTest, AnInnerGuardWinsUntilItEnds)
{
	Activity::Snapshot outer = {};
	outer.archive            = "outer";
	const Activity::Guard a(outer);
	{
		const Activity::Guard b(Member(tmp / "x.bin", 10));
		ASSERT_TRUE(Activity::Current());
		EXPECT_EQ(Activity::Current()->archive, "doom");
		EXPECT_EQ(Activity::CurrentStatus()->activity, "materializing");
	}
	ASSERT_TRUE(Activity::Current());
	EXPECT_EQ(Activity::Current()->archive, "outer");
}

TEST_F(ArchiveActivityTest, GuardsEndingOutOfOrderLeaveTheLiveOne)
{
	Activity::Snapshot first  = {};
	first.archive             = "first";
	auto a                    = std::make_unique<Activity::Guard>(first);
	Activity::Snapshot second = {};
	second.archive            = "second";
	auto b                    = std::make_unique<Activity::Guard>(second);
	a.reset();
	ASSERT_TRUE(Activity::Current());
	EXPECT_EQ(Activity::Current()->archive, "second");
	b.reset();
	EXPECT_FALSE(Activity::Current());
}

TEST_F(ArchiveActivityTest, AMemberCountsItsNewestPartFile)
{
	const auto target = tmp / "DOOM.WAD";
	const auto old_part = tmp / ("DOOM.WAD." + std::string(32, 'a') + ".part");
	const auto new_part = tmp / ("DOOM.WAD." + std::string(32, 'b') + ".part");
	Bytes(old_part, 50);
	Bytes(new_part, 300);
	fs::last_write_time(old_part,
	                    fs::last_write_time(new_part) - std::chrono::hours(1));
	Bytes(tmp / ("OTHER.WAD." + std::string(32, 'c') + ".part"), 999);
	Bytes(tmp / "DOOM.WAD.notpart", 777);
	Bytes(tmp / ("DOOM.WAD." + std::string(31, 'd') + ".part"), 555);
	EXPECT_EQ(Activity::BytesDone(Member(target, 1000)), 300);
}

TEST_F(ArchiveActivityTest, AFinishedMemberCountsItsFile)
{
	const auto target = tmp / "DOOM.WAD";
	EXPECT_EQ(Activity::BytesDone(Member(target, 1000)), 0);
	Bytes(target, 1000);
	EXPECT_EQ(Activity::BytesDone(Member(target, 1000)), 1000);
}

TEST_F(ArchiveActivityTest, IdentifyingCountsNothingWhateverItsTarget)
{
	Bytes(tmp / "DOOM.WAD", 1000);
	auto s = Member(tmp / "DOOM.WAD", 1000);
	s.kind = Activity::Kind::Identifying;
	EXPECT_EQ(Activity::BytesDone(s), 0);
}

TEST_F(ArchiveActivityTest, MaterializeAllSumsTheFolderOneLevelDeep)
{
	const auto dir = tmp / "all";
	fs::create_directories(dir / "sub");
	Bytes(dir / "1", 10);
	Bytes(dir / ("2." + std::string(32, 'e') + ".part"), 20);
	Bytes(dir / "sub" / "3", 400);
	Activity::Snapshot s = {};
	s.kind               = Activity::Kind::Materializing;
	s.target             = dir;
	s.target_is_folder   = true;
	EXPECT_EQ(Activity::BytesDone(s), 30);
}

// A failed pass leaves finished members behind; the next pass writes each
// again beside them, so a member counts once, by its larger file.
TEST_F(ArchiveActivityTest, AMemberRewrittenBesideItsOldFileCountsOnce)
{
	const auto dir = tmp / "all";
	fs::create_directories(dir);
	Bytes(dir / "1", 100);
	Bytes(dir / ("1." + std::string(32, 'a') + ".part"), 40);
	Bytes(dir / ("2." + std::string(32, 'b') + ".part"), 30);
	Activity::Snapshot s = {};
	s.kind               = Activity::Kind::Materializing;
	s.target             = dir;
	s.target_is_folder   = true;
	EXPECT_EQ(Activity::BytesDone(s), 130);
}

TEST_F(ArchiveActivityTest, SymlinksAndMissingFoldersCountNothing)
{
	const auto outside = tmp / "outside.bin";
	Bytes(outside, 5000);
	const auto dir = tmp / "all";
	fs::create_directories(dir);
	Bytes(dir / "1", 10);
	std::error_code ec = {};
	fs::create_symlink(outside, dir / "link", ec);
	fs::create_symlink(outside,
	                   dir / ("link." + std::string(32, 'f') + ".part"),
	                   ec);
	if (ec) {
		GTEST_SKIP() << "no symlinks here: " << ec.message();
	}
	Activity::Snapshot all = {};
	all.kind               = Activity::Kind::Materializing;
	all.target             = dir;
	all.target_is_folder   = true;
	EXPECT_EQ(Activity::BytesDone(all), 10);
	EXPECT_EQ(Activity::BytesDone(Member(dir / "link", 5000)), 0);
	all.target = tmp / "missing";
	EXPECT_EQ(Activity::BytesDone(all), 0);
	EXPECT_EQ(Activity::BytesDone(Member(tmp / "missing" / "x", 5)), 0);
}

// gtest here cannot print a u8string, so the test speaks std::string.
std::optional<std::string> Stem(const std::string& name)
{
	const auto stem = PartFileStem(std::u8string(name.begin(), name.end()));
	if (!stem) {
		return {};
	}
	return std::string(stem->begin(), stem->end());
}

TEST_F(ArchiveActivityTest, PartFileStemFollowsTheReaderNaming)
{
	const auto hex = std::string(32, '0');
	EXPECT_EQ(Stem("DOOM.WAD." + hex + ".part"), "DOOM.WAD");
	EXPECT_EQ(Stem("7." + std::string("0123456789abcdef0123456789abcdef") + ".part"),
	          "7");
	EXPECT_EQ(Stem("DOOM.WAD.part"), std::nullopt);
	EXPECT_EQ(Stem("." + hex + ".part"), std::nullopt);
	EXPECT_EQ(Stem("X." + std::string(31, '0') + ".part"), std::nullopt);
	EXPECT_EQ(Stem("X." + std::string(32, 'g') + ".part"), std::nullopt);
	EXPECT_EQ(Stem("X." + std::string(32, 'A') + ".part"), std::nullopt);
	EXPECT_EQ(Stem("X_" + hex + ".part"), std::nullopt);
	EXPECT_EQ(Stem("X." + hex + ".parts"), std::nullopt);
}

// The webserver thread reads while the emulation thread starts and ends
// guards; a torn record or a crash here is a race.
TEST_F(ArchiveActivityTest, ReadersAndGuardsOnTwoThreads)
{
	std::atomic<bool> stop = false;
	std::atomic<int> seen  = 0;
	std::thread reader([&] {
		while (!stop) {
			if (const auto status = Activity::CurrentStatus()) {
				EXPECT_TRUE(status->archive == "a" ||
				            status->archive == "b")
				        << status->archive;
				++seen;
			}
		}
	});
	for (int i = 0; i < 20000; ++i) {
		Activity::Snapshot s = {};
		s.archive            = (i % 2) ? "a" : "b";
		const Activity::Guard guard(s);
	}
	stop = true;
	reader.join();
	EXPECT_FALSE(Activity::Current());
}

} // namespace
