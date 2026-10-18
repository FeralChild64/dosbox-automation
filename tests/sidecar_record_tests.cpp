// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//
#include "dos/archive/sidecar_record.h"

#include "dos/archive/archive_toml.h"
#include "test_temp_dir.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <string>

namespace fs = std::filesystem;
using namespace ArchiveMount;

namespace {

SidecarRecord Fresh()
{
	SidecarRecord r = {};
	r.archive_path  = "/games/doom.zip";
	r.identity_hash = std::string(64, 'a');
	r.entry_count   = 5;
	r.total_bytes   = 8005;
	r.created       = 1'700'000'000;
	return r;
}

TEST(SidecarRecord, RoundTripsThroughToml)
{
	auto r = Fresh();
	r.AddDelete("GAME\\README.TXT");
	r.AddRename("GAME\\OLD.CFG", "GAME\\NEW.CFG");
	const auto doc  = r.ToDocument();
	auto error      = std::string();
	const auto back = SidecarRecord::FromDocument(doc, &error);
	ASSERT_TRUE(back) << error;
	EXPECT_EQ(back->archive_path, "/games/doom.zip");
	EXPECT_EQ(back->identity_hash, r.identity_hash);
	EXPECT_EQ(back->entry_count, 5);
	EXPECT_EQ(back->total_bytes, 8005);
	EXPECT_EQ(back->created, 1'700'000'000);
	ASSERT_EQ(back->changes.size(), 2u);
	EXPECT_EQ(back->changes[0].op, SidecarChange::Op::Delete);
	EXPECT_EQ(back->changes[0].path, "GAME\\README.TXT");
	EXPECT_EQ(back->changes[1].op, SidecarChange::Op::Rename);
	EXPECT_EQ(back->changes[1].path, "GAME\\OLD.CFG");
	EXPECT_EQ(back->changes[1].to, "GAME\\NEW.CFG");
	EXPECT_TRUE(back->IsHidden("GAME\\README.TXT"));
	EXPECT_TRUE(back->IsHidden("GAME\\OLD.CFG"));
	EXPECT_FALSE(back->IsHidden("GAME\\NEW.CFG"));
	EXPECT_EQ(doc.root.Integer("version"), SidecarRecordVersion);
}

TEST(SidecarRecord, AHiddenDirectoryHidesEverythingBelowIt)
{
	auto r = Fresh();
	r.AddDelete("GAME\\DATA");
	EXPECT_TRUE(r.IsHidden("GAME\\DATA"));
	EXPECT_TRUE(r.IsHidden("GAME\\DATA\\LEVEL.WAD"));
	EXPECT_TRUE(r.IsHidden("GAME\\DATA\\SUB\\X.TXT"));
	EXPECT_FALSE(r.IsHidden("GAME\\DATAX"));
	EXPECT_FALSE(r.IsHidden("GAME"));
	EXPECT_FALSE(r.IsHidden(""));
}

TEST(SidecarRecord, ACreateDropsTheDeleteOfTheSameName)
{
	auto r = Fresh();
	r.AddDelete("GAME\\SAVE.DAT");
	EXPECT_TRUE(r.IsHidden("GAME\\SAVE.DAT"));
	EXPECT_TRUE(r.Unhide("GAME\\SAVE.DAT"));
	EXPECT_FALSE(r.IsHidden("GAME\\SAVE.DAT"));
	EXPECT_TRUE(r.changes.empty());
	EXPECT_FALSE(r.Unhide("GAME\\OTHER.DAT"));
	// A directory delete is dropped by a create below it only for the
	// directory itself, which MakeDir does; a file create below a hidden
	// directory cannot happen (the directory is not visible).
	r.AddDelete("GAME\\DATA");
	EXPECT_FALSE(r.Unhide("GAME\\DATA\\X.TXT"));
	EXPECT_TRUE(r.Unhide("GAME\\DATA"));
}

TEST(SidecarRecord, DuplicatesAreNotRecordedTwice)
{
	auto r = Fresh();
	r.AddDelete("A.TXT");
	r.AddDelete("A.TXT");
	r.AddRename("B.TXT", "C.TXT");
	r.AddRename("B.TXT", "C.TXT");
	EXPECT_EQ(r.changes.size(), 2u);
}

TEST(SidecarRecord, LoadDropsEntriesThatFailTheDosPathRule)
{
	auto error     = std::string();
	const auto doc = ArchiveToml::Parse(
	        "version = 1\n"
	        "archive_path = \"/games/doom.zip\"\n"
	        "identity_hash = \"" +
	                std::string(64, 'b') +
	                "\"\n"
	                "entry_count = 1\n"
	                "total_bytes = 2\n"
	                "created = 3\n"
	                "[[changes]]\n"
	                "op = \"delete\"\n"
	                "path = \"GAME\\\\OK.TXT\"\n"
	                "[[changes]]\n"
	                "op = \"delete\"\n"
	                "path = \"..\\\\ESCAPE.TXT\"\n"
	                "[[changes]]\n"
	                "op = \"delete\"\n"
	                "path = \"/etc/passwd\"\n"
	                "[[changes]]\n"
	                "op = \"delete\"\n"
	                "path = \"lower.txt\"\n"
	                "[[changes]]\n"
	                "op = \"delete\"\n"
	                "path = \"TOOLONGNAME.TXT\"\n"
	                "[[changes]]\n"
	                "op = \"rename\"\n"
	                "from = \"A.TXT\"\n"
	                "[[changes]]\n"
	                "op = \"paint\"\n"
	                "path = \"A.TXT\"\n"
	                "[[changes]]\n"
	                "op = \"rename\"\n"
	                "from = \"A.TXT\"\n"
	                "to = \"B\\\\C.TXT\"\n",
	        &error);
	ASSERT_TRUE(doc) << error;
	const auto r = SidecarRecord::FromDocument(*doc, &error);
	ASSERT_TRUE(r) << error;
	ASSERT_EQ(r->changes.size(), 2u);
	EXPECT_EQ(r->changes[0].path, "GAME\\OK.TXT");
	EXPECT_EQ(r->changes[1].to, "B\\C.TXT");
	EXPECT_EQ(r->dropped_on_load, 6);
}

TEST(SidecarRecord, LoadRefusesAnotherVersionOrAMissingIdentity)
{
	auto error = std::string();
	auto doc = ArchiveToml::Parse("version = 2\nidentity_hash = \"x\"\n", &error);
	ASSERT_TRUE(doc);
	EXPECT_FALSE(SidecarRecord::FromDocument(*doc, &error));
	doc = ArchiveToml::Parse("version = 1\narchive_path = \"x\"\n", &error);
	ASSERT_TRUE(doc);
	EXPECT_FALSE(SidecarRecord::FromDocument(*doc, &error));
}

TEST(SidecarRecord, DosPathRule)
{
	EXPECT_TRUE(IsSidecarDosPath("A.TXT"));
	EXPECT_TRUE(IsSidecarDosPath("GAME\\DATA\\LEVEL.WAD"));
	EXPECT_TRUE(IsSidecarDosPath("FILENAME.EXT"));
	EXPECT_TRUE(IsSidecarDosPath("NOEXT"));
	EXPECT_TRUE(IsSidecarDosPath("A~1.TXT"));
	EXPECT_FALSE(IsSidecarDosPath(""));
	EXPECT_FALSE(IsSidecarDosPath("\\A.TXT"));
	EXPECT_FALSE(IsSidecarDosPath("A.TXT\\"));
	EXPECT_FALSE(IsSidecarDosPath("A\\\\B.TXT"));
	EXPECT_FALSE(IsSidecarDosPath("a.txt"));
	EXPECT_FALSE(IsSidecarDosPath("A/B.TXT"));
	EXPECT_FALSE(IsSidecarDosPath("..\\B.TXT"));
	EXPECT_FALSE(IsSidecarDosPath(".\\B.TXT"));
	EXPECT_FALSE(IsSidecarDosPath("TOOLONGNAME.TXT"));
	EXPECT_FALSE(IsSidecarDosPath("A.TOOLONG"));
	EXPECT_FALSE(IsSidecarDosPath("A.B.C"));
	EXPECT_FALSE(IsSidecarDosPath("A B.TXT"));
	EXPECT_FALSE(IsSidecarDosPath(std::string("A\x01.TXT")));
	EXPECT_FALSE(IsSidecarDosPath("C:A.TXT"));
}

TEST(SidecarRecord, ReadsAndWritesAFile)
{
	const auto dir = TestTempDir::MakeUnique("sidecar_record_");
	ASSERT_FALSE(dir.empty());
	auto r = Fresh();
	r.AddDelete("A.TXT");
	auto error = std::string();
	ASSERT_TRUE(r.Save(dir / SidecarTomlName, &error)) << error;
	const auto back = SidecarRecord::Load(dir / SidecarTomlName, &error);
	ASSERT_TRUE(back) << error;
	EXPECT_EQ(back->changes.size(), 1u);
	EXPECT_FALSE(SidecarRecord::Load(dir / "missing.toml", &error));
	std::error_code ec = {};
	fs::remove_all(dir, ec);
}

TEST(SidecarRecord, ADeleteBelowAHiddenDirectoryAddsNothing)
{
	auto r = Fresh();
	r.AddDelete("GAME\\A.TXT");
	r.AddDelete("GAME");
	ASSERT_EQ(r.changes.size(), 2u);
	EXPECT_TRUE(r.AddDelete("GAME\\B.TXT"));
	EXPECT_EQ(r.changes.size(), 2u);
	EXPECT_TRUE(r.IsHidden("GAME\\B.TXT"));
}

TEST(SidecarRecord, TheRecordRefusesAChangeItCouldNotLoadAgain)
{
	const auto dir = TestTempDir::MakeUnique("sidecar_record_");
	ASSERT_FALSE(dir.empty());
	auto r    = Fresh();
	auto name = [](const int i) {
		std::string n = std::to_string(i);
		n             = std::string(8 - n.size(), '0') + n;
		return "LONGDIR1\\LONGDIR2\\LONGDIR3\\LONGDIR4\\" + n + ".TXT";
	};
	int added = 0;
	while (added < 200000 && r.HasRoomForDelete(name(added))) {
		ASSERT_TRUE(r.AddDelete(name(added)));
		++added;
	}
	ASSERT_LT(added, 200000);
	EXPECT_FALSE(r.AddDelete(name(added)));
	EXPECT_FALSE(r.HasRoomForRename(name(added), "X.TXT"));
	EXPECT_EQ(r.changes.size(), static_cast<size_t>(added));
	auto error = std::string();
	ASSERT_TRUE(r.Save(dir / SidecarTomlName, &error)) << error;
	const auto back = SidecarRecord::Load(dir / SidecarTomlName, &error);
	ASSERT_TRUE(back) << error;
	EXPECT_EQ(back->changes.size(), static_cast<size_t>(added));
	std::error_code ec = {};
	fs::remove_all(dir, ec);
}

TEST(SidecarRecordTest, TrailingDotsAreCanonicalizedToTheSameKey)
{
	SidecarRecord r = {};
	EXPECT_TRUE(r.AddDelete("SKIEGA."));
	EXPECT_TRUE(r.IsHidden("SKIEGA"));
	EXPECT_TRUE(r.IsHidden("SKIEGA."));
	EXPECT_EQ(r.changes.size(), 1u);
	EXPECT_EQ(r.changes[0].path, "SKIEGA");
}

TEST(SidecarRecordTest, UnhideByTrailingDotFormRemovesTheCanonicalEntry)
{
	SidecarRecord r = {};
	EXPECT_TRUE(r.AddDelete("DATA"));
	EXPECT_TRUE(r.IsHidden("DATA"));
	EXPECT_TRUE(r.Unhide("DATA."));
	EXPECT_FALSE(r.IsHidden("DATA"));
	EXPECT_FALSE(r.IsHidden("DATA."));
	EXPECT_EQ(r.changes.size(), 0u);
}

// The archive drive looks "A.B." up as itself; the record must not hide
// A.B for it (one rule with the drive, gate 2026-10-17).
TEST(SidecarRecordTest, OnlyAnEmptyExtensionIsStripped)
{
	SidecarRecord r = {};
	EXPECT_TRUE(r.AddDelete("A.B."));
	EXPECT_FALSE(r.IsHidden("A.B"));
	EXPECT_TRUE(r.IsHidden("A.B."));
}

TEST(SidecarRecordTest, TrailingDotInSubpathIsCanonicalizedPerComponent)
{
	SidecarRecord r = {};
	EXPECT_TRUE(r.AddDelete("DATA.\\LEVEL."));
	EXPECT_TRUE(r.IsHidden("DATA\\LEVEL"));
	EXPECT_TRUE(r.IsHidden("DATA.\\LEVEL."));
	EXPECT_EQ(r.changes[0].path, "DATA\\LEVEL");
}

} // namespace
