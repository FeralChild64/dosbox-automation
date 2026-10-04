// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//
#include "dos/archive/member_path.h"

#include "archive_fixtures.h"
#include "test_temp_dir.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#if !defined(WIN32)
#include <sys/stat.h>
#endif

using ArchiveFixtures::FileEntry;
using ArchiveFixtures::Kind;
using ArchiveFixtures::WriteArchive;
using namespace ArchiveMount;

namespace {

namespace fs = std::filesystem;

class MemberPathTest : public testing::Test {
protected:
	void SetUp() override
	{
		tmp = TestTempDir::MakeUnique("member_path_");
		ASSERT_FALSE(tmp.empty());
		fs::create_directories(tmp / "games");
		auto error = std::string();
		ASSERT_TRUE(WriteArchive(tmp / "games" / "game.zip",
		                         Kind::ZipDeflate,
		                         {
		                                 {"disk1.img", std::string(4096, 'a')}
                },
		                         nullptr,
		                         &error))
		        << error;
		std::ofstream(tmp / "games" / "fake.zip") << "not an archive";
	}
	void TearDown() override
	{
		std::error_code ec = {};
		fs::remove_all(tmp, ec);
	}
	std::string P(const std::string& rest) const
	{
		return (tmp / "games").string() + "/" + rest;
	}
	fs::path tmp = {};
};

TEST_F(MemberPathTest, SplitsAtTheArchive)
{
	const auto split = SplitMemberPath(P("game.zip/disk1.img"));
	ASSERT_TRUE(split);
	EXPECT_EQ(split->archive, fs::path(P("game.zip")));
	EXPECT_EQ(split->member, "disk1.img");
}

TEST_F(MemberPathTest, KeepsTheMemberSeparatorsAsTyped)
{
	const auto split = SplitMemberPath(P("game.zip/cd\\disk1.cue"));
	ASSERT_TRUE(split);
	EXPECT_EQ(split->member, "cd\\disk1.cue");
}

TEST_F(MemberPathTest, AnExistingPathIsNoMemberPath)
{
	EXPECT_FALSE(SplitMemberPath(P("game.zip")));
	EXPECT_FALSE(SplitMemberPath((tmp / "games").string()));
}

TEST_F(MemberPathTest, AFileThatIsNoArchiveEndsTheWalk)
{
	EXPECT_FALSE(SplitMemberPath(P("fake.zip/disk1.img")));
}

TEST_F(MemberPathTest, AMissingComponentEndsTheWalk)
{
	EXPECT_FALSE(SplitMemberPath(P("missing/game.zip/disk1.img")));
}

TEST_F(MemberPathTest, AnEmptyMemberIsNoMemberPath)
{
	EXPECT_FALSE(SplitMemberPath(P("game.zip/")));
}

TEST_F(MemberPathTest, IsArchiveFileWantsExtensionAndSignature)
{
	EXPECT_TRUE(IsArchiveFile(P("game.zip")));
	EXPECT_FALSE(IsArchiveFile(P("fake.zip")));
	fs::copy_file(P("game.zip"), P("game.dat"));
	EXPECT_FALSE(IsArchiveFile(P("game.dat")));
}

std::vector<Entry> Index()
{
	auto file = [](int ordinal, const char* utf8, const char* dos) {
		Entry e     = {};
		e.ordinal   = ordinal;
		e.utf8_path = utf8;
		e.dos_path  = dos;
		e.size      = 10;
		return e;
	};
	auto dir   = file(ImpliedOrdinal, "Disks", "DISKS");
	dir.is_dir = true;
	return {dir,
	        file(0, "Disks/Disk One.img", "DISKS\\DISKON~1.IMG"),
	        file(1, "Disks/disk2.img", "DISKS\\DISK2.IMG"),
	        file(2, "a.img", "A.IMG"),
	        file(3, "A.IMG", "A~1.IMG"),
	        file(4, "Disk Two.img", "DISKTW~1.IMG"),
	        file(5, "DISK TWO.IMG", "DISKTW~2.IMG")};
}

#if !defined(WIN32)
TEST_F(MemberPathTest, IsArchiveFileDoesNotWaitOnAFifo)
{
	const auto fifo = tmp / "games" / "pipe.zip";
	ASSERT_EQ(mkfifo(fifo.c_str(), 0600), 0);
	EXPECT_FALSE(IsArchiveFile(fifo));
}
#endif

TEST(FindMember, MatchesTheUtf8PathWithEitherSeparator)
{
	const auto entries = Index();
	auto rejection     = ArchiveNames::Rejection::None;
	const auto* found = FindMember(entries, "Disks\\Disk One.img", &rejection);
	ASSERT_NE(found, nullptr);
	EXPECT_EQ(found->ordinal, 0);
}

TEST(FindMember, MatchesTheDosPath)
{
	const auto entries = Index();
	auto rejection     = ArchiveNames::Rejection::None;
	const auto* found = FindMember(entries, "DISKS/DISKON~1.IMG", &rejection);
	ASSERT_NE(found, nullptr);
	EXPECT_EQ(found->ordinal, 0);
}

TEST(FindMember, MatchesWithoutCaseWhenUnique)
{
	const auto entries = Index();
	auto rejection     = ArchiveNames::Rejection::None;
	const auto* found  = FindMember(entries, "disks/DISK2.img", &rejection);
	ASSERT_NE(found, nullptr);
	EXPECT_EQ(found->ordinal, 1);
}

TEST(FindMember, AnExactSpellingWinsOverACaseTwin)
{
	const auto entries = Index();
	auto rejection     = ArchiveNames::Rejection::None;
	EXPECT_EQ(FindMember(entries, "a.img", &rejection)->ordinal, 2);
	EXPECT_EQ(FindMember(entries, "A.IMG", &rejection)->ordinal, 3);
}

TEST(FindMember, ACaseTwinWithoutAnExactMatchIsAmbiguous)
{
	const auto entries = Index();
	auto rejection     = ArchiveNames::Rejection::None;
	EXPECT_EQ(FindMember(entries, "disk two.Img", &rejection), nullptr);
	EXPECT_EQ(rejection, ArchiveNames::Rejection::None);
	// An 8.3 spelling is the DOS name, which is unique: no ambiguity.
	EXPECT_EQ(FindMember(entries, "a.Img", &rejection)->ordinal, 2);
}

TEST(FindMember, ADirectoryNeverMatches)
{
	const auto entries = Index();
	auto rejection     = ArchiveNames::Rejection::None;
	EXPECT_EQ(FindMember(entries, "Disks", &rejection), nullptr);
}

TEST(FindMember, DotComponentsAreRejected)
{
	const auto entries = Index();
	auto rejection     = ArchiveNames::Rejection::None;
	EXPECT_EQ(FindMember(entries, "Disks/../a.img", &rejection), nullptr);
	EXPECT_EQ(rejection, ArchiveNames::Rejection::DotComponent);
}

TEST(FindMember, AnAbsoluteMemberIsRejected)
{
	const auto entries = Index();
	auto rejection     = ArchiveNames::Rejection::None;
	EXPECT_EQ(FindMember(entries, "/a.img", &rejection), nullptr);
	EXPECT_EQ(rejection, ArchiveNames::Rejection::AbsolutePath);
}

} // namespace
