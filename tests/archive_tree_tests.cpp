// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//
#include "dos/archive/archive_tree.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace ArchiveMount;

namespace {

Entry File(const int ordinal, const std::string& utf8, const std::string& dos,
           const int64_t size, const int64_t mtime = 631152000)
{
	Entry e     = {};
	e.ordinal   = ordinal;
	e.utf8_path = utf8;
	e.dos_path  = dos;
	e.size      = size;
	e.mtime     = mtime;
	return e;
}

Entry Dir(const int ordinal, const std::string& utf8, const std::string& dos)
{
	auto e   = File(ordinal, utf8, dos, 0);
	e.is_dir = true;
	return e;
}

std::vector<Entry> Game()
{
	return {
	        Dir(0, "game", "GAME"),
	        File(1, "game/doom.exe", "GAME\\DOOM.EXE", 700),
	        Dir(2, "game/data", "GAME\\DATA"),
	        File(3, "game/data/level.wad", "GAME\\DATA\\LEVEL.WAD", 5000),
	        File(4, "game/readme.txt", "GAME\\README.TXT", 12),
	};
}

TEST(ArchiveTree, BuildsTheTreeInIndexOrder)
{
	auto error      = std::string();
	const auto tree = ArchiveTree::Build(Game(), "", &error);
	ASSERT_TRUE(tree) << error;
	const auto& root = tree->Root();
	EXPECT_TRUE(root.is_dir);
	EXPECT_EQ(root.entry_index, -1);
	ASSERT_EQ(root.children.size(), 1u);
	const auto& game = tree->Node(root.children[0]);
	EXPECT_EQ(game.name, "GAME");
	EXPECT_EQ(game.dos_path, "GAME");
	ASSERT_EQ(game.children.size(), 3u);
	EXPECT_EQ(tree->Node(game.children[0]).name, "DOOM.EXE");
	EXPECT_EQ(tree->Node(game.children[1]).name, "DATA");
	EXPECT_EQ(tree->Node(game.children[2]).name, "README.TXT");
	EXPECT_EQ(tree->TotalBytes(), 5712);
	EXPECT_EQ(tree->FileCount(), 3);
}

TEST(ArchiveTree, FindsByDosPathAndTheRootByTheEmptyString)
{
	auto error      = std::string();
	const auto tree = ArchiveTree::Build(Game(), "", &error);
	ASSERT_TRUE(tree) << error;
	EXPECT_EQ(tree->Find(""), &tree->Root());
	const auto* wad = tree->Find("GAME\\DATA\\LEVEL.WAD");
	ASSERT_TRUE(wad);
	EXPECT_EQ(wad->ordinal, 3);
	EXPECT_EQ(wad->size, 5000);
	EXPECT_FALSE(wad->is_dir);
	EXPECT_FALSE(tree->Find("GAME\\DATA\\NOPE.WAD"));
	EXPECT_FALSE(tree->Find("game\\data\\level.wad")); // DOS passes uppercase
	EXPECT_FALSE(tree->Find("\\GAME"));
	EXPECT_FALSE(tree->Find("GAME\\"));
}

TEST(ArchiveTree, ARootPathMakesThatDirectoryTheRoot)
{
	auto error      = std::string();
	const auto tree = ArchiveTree::Build(Game(), "GAME", &error);
	ASSERT_TRUE(tree) << error;
	ASSERT_EQ(tree->Root().children.size(), 3u);
	const auto* wad = tree->Find("DATA\\LEVEL.WAD");
	ASSERT_TRUE(wad);
	EXPECT_EQ(wad->ordinal, 3);
	EXPECT_FALSE(tree->Find("GAME"));
	EXPECT_FALSE(tree->Find("GAME\\DOOM.EXE"));
	EXPECT_EQ(tree->TotalBytes(), 5712);
	EXPECT_FALSE(ArchiveTree::Build(Game(), "NOPE", &error));
	EXPECT_FALSE(ArchiveTree::Build(Game(), "GAME\\DOOM.EXE", &error));
}

TEST(ArchiveTree, SoloRootDirectoryIsTheOneTopDirectoryEverythingLivesUnder)
{
	EXPECT_EQ(SoloRootDirectory(Game()), "GAME");
	auto two = Game();
	two.push_back(File(5, "other.txt", "OTHER.TXT", 1));
	EXPECT_EQ(SoloRootDirectory(two), "");
	EXPECT_EQ(SoloRootDirectory({}), "");
	EXPECT_EQ(SoloRootDirectory({Dir(0, "a", "A"), Dir(1, "b", "B")}), "");
	// A lone top-level file is not a directory to enter.
	EXPECT_EQ(SoloRootDirectory({File(0, "x.exe", "X.EXE", 1)}), "");
	// An implied directory counts: the zip had no header for it.
	auto implied       = Game();
	implied[0].ordinal = ImpliedOrdinal;
	EXPECT_EQ(SoloRootDirectory(implied), "GAME");
}

TEST(ArchiveTree, RefusesAMissingParentADuplicateAndAFileUsedAsADirectory)
{
	auto error = std::string();
	EXPECT_FALSE(ArchiveTree::Build({File(0, "a/b.txt", "A\\B.TXT", 1)}, "", &error));
	EXPECT_NE(error.find("A\\B.TXT"), std::string::npos);
	EXPECT_FALSE(ArchiveTree::Build({File(0, "a.txt", "A.TXT", 1),
	                                 File(1, "A.txt", "A.TXT", 2)},
	                                "",
	                                &error));
	EXPECT_FALSE(ArchiveTree::Build(
	        {File(0, "a", "A", 1), File(1, "a/b", "A\\B", 1)}, "", &error));
	EXPECT_FALSE(ArchiveTree::Build({Dir(0, "a", "A"), File(1, "a", "A", 1)},
	                                "",
	                                &error));
	EXPECT_FALSE(ArchiveTree::Build({File(0, "x", "", 1)}, "", &error));
	EXPECT_FALSE(ArchiveTree::Build({File(0, "x", "\\X", 1)}, "", &error));
	EXPECT_FALSE(ArchiveTree::Build({File(0, "x", "X\\", 1)}, "", &error));
	EXPECT_FALSE(ArchiveTree::Build({File(0, "x", "X\\\\Y", 1)}, "", &error));
}

TEST(ArchiveTree, PacksDosDatesLikeTheEngine)
{
	// 1990-01-01 00:00:00 UTC; in any time zone the date is 1989-12-31
	// or 1990-01-01, and the packed fields follow the local calendar.
	const auto d = PackDosDateTime(631152000);
	EXPECT_TRUE(d.date == ((10 << 9) | (1 << 5) | 1) ||
	            d.date == ((9 << 9) | (12 << 5) | 31))
	        << d.date;
	// Before the FAT epoch: clamped to 1980-01-01 00:00.
	const auto early = PackDosDateTime(0);
	EXPECT_EQ(early.date, (0 << 9) | (1 << 5) | 1);
	EXPECT_EQ(early.time, 0);
	// 2107-12-31 23:59:58 UTC is the last representable second; later is
	// clamped.
	const auto late = PackDosDateTime(4354819199);
	EXPECT_EQ(late.date >> 9, 127);
	auto error = std::string();
	const auto tree = ArchiveTree::Build({File(0, "x", "X", 1, 0)}, "", &error);
	ASSERT_TRUE(tree) << error;
	EXPECT_EQ(tree->Find("X")->dos_date, early.date);
}

} // namespace
