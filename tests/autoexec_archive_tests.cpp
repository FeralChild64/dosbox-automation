// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//
#include "shell/autoexec_archive.h"

#include "archive_fixtures.h"
#include "test_temp_dir.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

using namespace testing;
using ArchiveFixtures::Filter;
using ArchiveFixtures::IszChunk;
using ArchiveFixtures::Kind;
using ArchiveFixtures::MinimalIso;
using ArchiveFixtures::WriteArchive;
using ArchiveFixtures::WriteCompressed;
using ArchiveFixtures::WriteIsz;
using AutoexecArchive::ArchiveLines;
using AutoexecArchive::Argument;
using AutoexecArchive::Classify;

class AutoexecArchiveTest : public Test {
protected:
	void SetUp() override
	{
		tmp = TestTempDir::MakeUnique("autoexec_archive_");
		ASSERT_FALSE(tmp.empty());
	}
	void TearDown() override
	{
		std::error_code ec = {};
		std::filesystem::remove_all(tmp, ec);
	}
	std::filesystem::path Text(const std::string& name, const std::string& text)
	{
		std::ofstream(tmp / name, std::ios::binary) << text;
		return tmp / name;
	}

	std::filesystem::path tmp = {};
};

TEST_F(AutoexecArchiveTest, AZipAndATarGzAreArchives)
{
	ASSERT_TRUE(WriteArchive(tmp / "game.zip",
	                         Kind::ZipStored,
	                         {
	                                 {"GAME.BAT", "x"}
        }));
	ASSERT_TRUE(WriteArchive(tmp / "game.tar.gz",
	                         Kind::TarGz,
	                         {
	                                 {"GAME.BAT", "x"}
        }));
	EXPECT_EQ(Classify(tmp / "game.zip"), Argument::Archive);
	EXPECT_EQ(Classify(tmp / "game.tar.gz"), Argument::Archive);
}

TEST_F(AutoexecArchiveTest, ACompressedFloppyAndAnImzAreDiskContainers)
{
	ASSERT_TRUE(WriteCompressed(tmp / "disk1.img.xz",
	                            Filter::Xz,
	                            std::string(1024, 'f')));
	ASSERT_TRUE(WriteArchive(tmp / "game.imz",
	                         Kind::ZipDeflate,
	                         {
	                                 {"DISK1.IMA", "f"}
        }));
	EXPECT_EQ(Classify(tmp / "disk1.img.xz"), Argument::DiskContainer);
	EXPECT_EQ(Classify(tmp / "game.imz"), Argument::DiskContainer);
}

TEST_F(AutoexecArchiveTest, AnIszAndACompressedCdImageAreCdContainers)
{
	ASSERT_TRUE(WriteIsz(tmp / "game.isz", MinimalIso(), IszChunk::Zlib));
	ASSERT_TRUE(WriteCompressed(tmp / "game.iso.xz", Filter::Xz, MinimalIso()));
	ASSERT_TRUE(WriteCompressed(tmp / "track.bin.gz", Filter::Gzip, MinimalIso()));
	EXPECT_EQ(Classify(tmp / "game.isz"), Argument::CdContainer);
	EXPECT_EQ(Classify(tmp / "game.iso.xz"), Argument::CdContainer);
	EXPECT_EQ(Classify(tmp / "track.bin.gz"), Argument::CdContainer);
}

TEST_F(AutoexecArchiveTest, NamesWithoutTheMatchingBytesAreNothing)
{
	EXPECT_EQ(Classify(Text("junk.zip", "not a zip at all")), std::nullopt);
	EXPECT_EQ(Classify(Text("junk.img.xz", "not xz either")), std::nullopt);
	EXPECT_EQ(Classify(Text("junk.isz", "no IsZ! here")), std::nullopt);
	EXPECT_EQ(Classify(Text("game.exe", "MZ")), std::nullopt);
	EXPECT_EQ(Classify(Text("disk.img", std::string(1024, 'f'))), std::nullopt);
	std::filesystem::create_directories(tmp / "folder.zip");
	EXPECT_EQ(Classify(tmp / "folder.zip"), std::nullopt);
	EXPECT_EQ(Classify(tmp / "missing.zip"), std::nullopt);
}

TEST_F(AutoexecArchiveTest, ArchiveLinesMountCAndLeaveTheProgramToMount)
{
	EXPECT_THAT(ArchiveLines("/games/doom.zip", ""),
	            Optional(ElementsAre("@Z:\\MOUNT.COM -u C >NUL",
	                                 "@Z:\\MOUNT.COM C \"/games/doom.zip\" -run -exit",
	                                 "@C:")));
	EXPECT_THAT(ArchiveLines("/my games/doom.zip", "GAME\\START.EXE"),
	            Optional(ElementsAre("@Z:\\MOUNT.COM -u C >NUL",
	                                 "@Z:\\MOUNT.COM C \"/my games/doom.zip\" -run "
	                                 "GAME\\START.EXE -exit",
	                                 "@C:")));
}

// AUTOEXEC.BAT goes through the batch expansion, where a lone % ends the
// line and %1 or %PATH% are replaced.
TEST_F(AutoexecArchiveTest, PercentSignsSurviveTheBatchExpansion)
{
	EXPECT_THAT(ArchiveLines("/games/100% Doom %1 %PATH%.zip", "100%.EXE"),
	            Optional(ElementsAre("@Z:\\MOUNT.COM -u C >NUL",
	                                 "@Z:\\MOUNT.COM C \"/games/100%% Doom %%1 "
	                                 "%%PATH%%.zip\" -run 100%%.EXE -exit",
	                                 "@C:")));
}

// The name lands in a batch line, where > < | are redirections.
TEST_F(AutoexecArchiveTest, ARunNameThatIsNoProgramNameIsLeftOut)
{
	for (const auto* name : {"X >C:\\AUTOEXEC.BAT",
	                         "A|B",
	                         "..\\X.EXE",
	                         "C:X.EXE",
	                         "A B.EXE",
	                         "X.EXE -fs none",
	                         "-GAME.EXE"}) {
		EXPECT_THAT(ArchiveLines("/games/doom.zip", name),
		            Optional(ElementsAre("@Z:\\MOUNT.COM -u C >NUL",
		                                 "@Z:\\MOUNT.COM C \"/games/doom.zip\"",
		                                 "@C:")))
		        << name;
	}
}

TEST_F(AutoexecArchiveTest, APathThatCannotBeQuotedIsRefused)
{
	EXPECT_EQ(ArchiveLines("/games/a\"b.zip", ""), std::nullopt);
	EXPECT_EQ(ArchiveLines("/games/a\nb.zip", ""), std::nullopt);
	EXPECT_EQ(ArchiveLines("/games/a\rb.zip", ""), std::nullopt);
	EXPECT_EQ(ArchiveLines(std::string("/games/a\0b.zip", 14), ""), std::nullopt);
	EXPECT_EQ(ArchiveLines("", ""), std::nullopt);
}

} // namespace
