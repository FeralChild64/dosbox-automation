// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//
#include "dos/archive/sidecar_locator.h"

#include "dos/archive/sidecar_record.h"
#include "test_temp_dir.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using namespace ArchiveMount;

namespace {

class SidecarLocatorTest : public ::testing::Test {
protected:
	void SetUp() override
	{
		tmp = TestTempDir::MakeUnique("sidecar_locator_");
		ASSERT_FALSE(tmp.empty());
		saves   = tmp / "saves";
		games   = tmp / "games";
		archive = games / "doom.zip";
		fs::create_directories(saves);
		fs::create_directories(games);
		{
			std::ofstream out(archive, std::ios::binary);
			out << "PK";
		}
	}
	void TearDown() override
	{
		std::error_code ec = {};
		fs::remove_all(tmp, ec);
	}
	// A sidecar directory with a record naming archive_path and hash.
	fs::path MakeSidecar(const fs::path& parent, const std::string& name,
	                     const std::string& archive_path, const std::string& hash)
	{
		const auto dir = parent / name;
		fs::create_directories(dir);
		SidecarRecord r = {};
		r.archive_path  = archive_path;
		r.identity_hash = hash;
		auto error      = std::string();
		EXPECT_TRUE(r.Save(dir / SidecarTomlName, &error)) << error;
		return dir;
	}
	SidecarLocation Locate(const std::string& hash)
	{
		return LocateSidecar(saves, fs::canonical(archive), hash);
	}

	fs::path tmp = {}, saves = {}, games = {}, archive = {};
	const std::string hash_a = std::string(64, 'a');
	const std::string hash_b = std::string(64, 'b');
};

TEST_F(SidecarLocatorTest, NothingFoundMeansCreateUnderSaves)
{
	const auto loc = Locate(hash_a);
	EXPECT_EQ(loc.kind, SidecarLocation::Kind::Create);
	EXPECT_EQ(loc.directory, saves / ("doom-" + hash_a.substr(0, 8)));
	EXPECT_TRUE(loc.beside_archive.empty());
	EXPECT_TRUE(loc.replaced_old.empty());
}

TEST_F(SidecarLocatorTest, APathMatchUnderSavesWins)
{
	const auto by_path = MakeSidecar(saves,
	                                 "doom-aaaaaaaa",
	                                 fs::canonical(archive).string(),
	                                 hash_a);
	MakeSidecar(saves, "other-aaaaaaaa", "/elsewhere/doom.zip", hash_a);
	const auto loc = Locate(hash_a);
	EXPECT_EQ(loc.kind, SidecarLocation::Kind::ByPath);
	EXPECT_EQ(loc.directory, by_path);
}

TEST_F(SidecarLocatorTest, AHashMatchFindsAMovedArchive)
{
	const auto by_hash = MakeSidecar(saves,
	                                 "old-aaaaaaaa",
	                                 "/old/place/doom.zip",
	                                 hash_a);
	const auto loc     = Locate(hash_a);
	EXPECT_EQ(loc.kind, SidecarLocation::Kind::ByHash);
	EXPECT_EQ(loc.directory, by_hash);
}

TEST_F(SidecarLocatorTest, PathOutranksHashAndNamesTheOther)
{
	const auto by_path = MakeSidecar(saves,
	                                 "doom-aaaaaaaa",
	                                 fs::canonical(archive).string(),
	                                 hash_a);
	const auto by_hash = MakeSidecar(saves, "old-aaaaaaaa", "/old/doom.zip", hash_a);
	const auto loc = Locate(hash_a);
	EXPECT_EQ(loc.kind, SidecarLocation::Kind::ByPath);
	EXPECT_EQ(loc.directory, by_path);
	EXPECT_EQ(loc.also_matching, by_hash);
}

TEST_F(SidecarLocatorTest, APathMatchWithAnotherHashIsD2a)
{
	const auto old = MakeSidecar(saves,
	                             "doom-bbbbbbbb",
	                             fs::canonical(archive).string(),
	                             hash_b);
	const auto loc = Locate(hash_a);
	EXPECT_EQ(loc.kind, SidecarLocation::Kind::Create);
	EXPECT_EQ(loc.directory, saves / ("doom-" + hash_a.substr(0, 8)));
	EXPECT_EQ(loc.replaced_old, old);
}

TEST_F(SidecarLocatorTest, ASidecarBesideTheArchiveIsReportedNotChosen)
{
	const auto beside = MakeSidecar(games,
	                                "doom-aaaaaaaa",
	                                fs::canonical(archive).string(),
	                                hash_a);
	const auto loc    = Locate(hash_a);
	EXPECT_EQ(loc.kind, SidecarLocation::Kind::Create);
	EXPECT_EQ(loc.beside_archive, beside);
	// Under saves it would have been chosen; beside the archive the
	// caller decides through the directory whitelist (D2 step 4).
}

TEST_F(SidecarLocatorTest, ABesidePathMatchWithAnotherHashIsD2a)
{
	const auto old = MakeSidecar(games,
	                             "doom-bbbbbbbb",
	                             fs::canonical(archive).string(),
	                             hash_b);
	const auto loc = Locate(hash_a);
	EXPECT_EQ(loc.kind, SidecarLocation::Kind::Create);
	EXPECT_TRUE(loc.beside_archive.empty());
	EXPECT_EQ(loc.replaced_old, old);
}

TEST_F(SidecarLocatorTest, BesideTheArchivePathOutranksHash)
{
	MakeSidecar(games, "a-moved-aaaaaaaa", "/old/doom.zip", hash_a);
	const auto by_path = MakeSidecar(games,
	                                 "doom-aaaaaaaa",
	                                 fs::canonical(archive).string(),
	                                 hash_a);
	const auto loc     = Locate(hash_a);
	EXPECT_EQ(loc.beside_archive, by_path);
}

TEST_F(SidecarLocatorTest, IgnoresDirectoriesWithoutARecordAndLinks)
{
	fs::create_directories(saves / "junk-aaaaaaaa");
	{
		std::ofstream out(saves / "junk-aaaaaaaa" / SidecarTomlName);
		out << "garbage = [\n";
	}
	fs::create_directories(saves / "nothing-here");
#if !defined(WIN32)
	const auto real = MakeSidecar(tmp,
	                              "real-aaaaaaaa",
	                              fs::canonical(archive).string(),
	                              hash_a);
	fs::create_directory_symlink(real, saves / "link-aaaaaaaa");
#endif
	const auto loc = Locate(hash_a);
	EXPECT_EQ(loc.kind, SidecarLocation::Kind::Create);
}

TEST_F(SidecarLocatorTest, TheKeyIsTheSanitizedStemAndHash8)
{
	EXPECT_EQ(SidecarKey("Game Disk!.zip", hash_a), "Game_Disk_-aaaaaaaa");
	EXPECT_EQ(SidecarKey("doom.tar.gz", hash_a), "doom.tar-aaaaaaaa");
}

} // namespace
