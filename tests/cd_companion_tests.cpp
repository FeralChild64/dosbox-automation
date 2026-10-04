// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//
#include "dos/cd_companion.h"

#include "dos/cdrom.h"
#include "dosbox_test_fixture.h"
#include "test_temp_dir.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

void Touch(const std_fs::path& path, const size_t bytes = 2048)
{
	std::ofstream out(path, std::ios::binary);
	out << std::string(bytes, '\0');
}

class CdCompanionTest : public ::testing::Test {
protected:
	void SetUp() override
	{
		tmp = TestTempDir::MakeUnique("cd_companion_");
		ASSERT_FALSE(tmp.empty());
		images = tmp / "images";
		std_fs::create_directories(images);
		image = images / "GAME.CUE";
		Touch(image, 10);
	}
	void TearDown() override
	{
		std::error_code ec = {};
		std_fs::remove_all(tmp, ec);
	}
	std::optional<CdCompanion::Resolved> Resolve(const std::string& name)
	{
		why.clear();
		return CdCompanion::Resolve(image, name, &why);
	}

	std_fs::path tmp = {}, images = {}, image = {};
	std::string why = {};
};

TEST_F(CdCompanionTest, ABareNameBesideTheImageIsTakenAsWritten)
{
	Touch(images / "GAME.BIN");
	const auto r = Resolve("GAME.BIN");
	ASSERT_TRUE(r) << why;
	EXPECT_EQ(r->path, std_fs::canonical(images / "GAME.BIN"));
	EXPECT_FALSE(r->rewritten);
}

TEST_F(CdCompanionTest, AnAbsolutePathNeverLeavesTheImageDirectory)
{
	Touch(tmp / "secret.bin");
	EXPECT_FALSE(Resolve((tmp / "secret.bin").string())) << "outside";
	// The same name beside the image is what the line gets.
	Touch(images / "secret.bin");
	const auto r = Resolve((tmp / "secret.bin").string());
	ASSERT_TRUE(r) << why;
	EXPECT_EQ(r->path, std_fs::canonical(images / "secret.bin"));
	EXPECT_TRUE(r->rewritten);
}

TEST_F(CdCompanionTest, ParentStepsNeverLeaveTheImageDirectory)
{
	Touch(tmp / "secret.bin");
	EXPECT_FALSE(Resolve("../secret.bin"));
	EXPECT_FALSE(Resolve("..\\secret.bin"));
	EXPECT_FALSE(Resolve(".."));
}

TEST_F(CdCompanionTest, ALowercaseFileAnswersAnUppercaseName)
{
	Touch(images / "game.bin");
	const auto r = Resolve("GAME.BIN");
	ASSERT_TRUE(r) << why;
	EXPECT_EQ(r->path, std_fs::canonical(images / "game.bin"));
	EXPECT_TRUE(r->rewritten);
}

// A cue written on Windows names its files in the ANSI code page; those
// bytes are not UTF-8 and must reach the host as they are.
TEST_F(CdCompanionTest, ANameInTheCuesOwnBytesIsTakenAsWritten)
{
	const std::string name = "Pf\xFCtze.bin";
	Touch(images / name);
	if (!std_fs::exists(images / name)) {
		GTEST_SKIP() << "this filesystem wants UTF-8 names";
	}
	const auto r = Resolve(name);
	ASSERT_TRUE(r) << why;
	EXPECT_EQ(r->path, std_fs::canonical(images / name));
	EXPECT_FALSE(r->rewritten);
}

TEST_F(CdCompanionTest, ADosPathFromARipMapsToTheFileBeside)
{
	Touch(images / "GAME.BIN");
	for (const auto* name : {"C:\\RIPS\\GAME.BIN", "D:GAME.BIN", "\\GAME.BIN"}) {
		const auto r = Resolve(name);
		ASSERT_TRUE(r) << name << ": " << why;
		EXPECT_EQ(r->path, std_fs::canonical(images / "GAME.BIN"));
		EXPECT_TRUE(r->rewritten) << name;
	}
}

TEST_F(CdCompanionTest, DotNamedAndReservedCompanionsAreRefused)
{
	Touch(images / ".track.bin");
	EXPECT_FALSE(Resolve(".track.bin"));
	EXPECT_FALSE(Resolve(""));
	EXPECT_FALSE(Resolve("."));
	EXPECT_FALSE(Resolve("NUL.BIN"));
	EXPECT_FALSE(Resolve("com1"));
}

TEST_F(CdCompanionTest, ADirectoryIsNotACompanion)
{
	std_fs::create_directories(images / "TRACKS");
	EXPECT_FALSE(Resolve("TRACKS"));
}

#if !defined(WIN32)
TEST_F(CdCompanionTest, ALinkedCompanionIsRefused)
{
	Touch(tmp / "secret.bin");
	std_fs::create_symlink(tmp / "secret.bin", images / "GAME.BIN");
	EXPECT_FALSE(Resolve("GAME.BIN"));
	EXPECT_FALSE(Resolve("game.bin"));
}
#endif

#if defined(__linux__)
TEST_F(CdCompanionTest, TwoFilesDifferingOnlyInCaseAreAmbiguous)
{
	Touch(images / "game.bin");
	Touch(images / "Game.Bin");
	EXPECT_FALSE(Resolve("GAME.BIN"));
	EXPECT_NE(why.find("ambiguous"), std::string::npos) << why;
	// An exact match is no ambiguity.
	EXPECT_TRUE(Resolve("game.bin"));
}
#endif

// The loaders themselves, through SetDevice as MOUNT and the swap reach them.
class CdImageCompanionTest : public DOSBoxTestFixture {
protected:
	void SetUp() override
	{
		DOSBoxTestFixture::SetUp();
		tmp = TestTempDir::MakeUnique("cd_image_companion_");
		ASSERT_FALSE(tmp.empty());
		images = tmp / "images";
		std_fs::create_directories(images);
	}
	void TearDown() override
	{
		std::error_code ec = {};
		std_fs::remove_all(tmp, ec);
		DOSBoxTestFixture::TearDown();
	}
	bool Load(const std_fs::path& path)
	{
		CDROM_Interface_Image cd = {};
		return cd.SetDevice(path.string().c_str());
	}
	std_fs::path WriteCue(const std::string& file_name)
	{
		const auto cue = images / "GAME.CUE";
		std::ofstream out(cue, std::ios::binary);
		out << "FILE \"" << file_name << "\" BINARY\n"
		    << "  TRACK 01 MODE1/2048\n"
		    << "    INDEX 01 00:00:00\n";
		return cue;
	}
	// One data track whose footer names mdf_name (MDS layout in cdrom_mds.h).
	std_fs::path WriteMds(const std::string& mdf_name)
	{
		std::vector<uint8_t> b(220 + mdf_name.size() + 1, 0);
		const auto put32 = [&](const size_t at, const uint32_t v) {
			for (int i = 0; i < 4; ++i) {
				b[at + i] = static_cast<uint8_t>(v >> (8 * i));
			}
		};
		std::memcpy(b.data(), "MEDIA DESCRIPTOR", 16);
		b[16] = 1;      // version
		b[20] = 1;      // num_sessions
		put32(80, 88);  // session_block_offset
		b[88 + 10] = 1; // num_all_blocks
		put32(88 + 20, 112);
		b[112 + 0] = 2;       // mode 1 data
		b[112 + 4] = 1;       // point: track 1
		put32(112 + 12, 192); // extra_offset
		b[112 + 16] = 0x00;   // sector_size 2048
		b[112 + 17] = 0x08;
		put32(112 + 48, 1);   // number_of_files
		put32(112 + 52, 200); // footer_offset
		put32(192 + 4, 1);    // extra: length one sector
		put32(200, 220);      // footer: filename_offset
		std::memcpy(b.data() + 220, mdf_name.data(), mdf_name.size());
		const auto mds = images / "GAME.MDS";
		std::ofstream out(mds, std::ios::binary);
		out.write(reinterpret_cast<const char*>(b.data()),
		          static_cast<std::streamsize>(b.size()));
		return mds;
	}

	std_fs::path tmp = {}, images = {};
};

TEST_F(CdImageCompanionTest, ACueReachesOnlyFilesBesideIt)
{
	Touch(images / "GAME.BIN");
	EXPECT_TRUE(Load(WriteCue("GAME.BIN")));
	EXPECT_TRUE(Load(WriteCue("C:\\RIPS\\game.bin")));
	Touch(tmp / "secret.bin");
	EXPECT_FALSE(Load(WriteCue((tmp / "secret.bin").string())));
	EXPECT_FALSE(Load(WriteCue("../secret.bin")));
}

TEST_F(CdImageCompanionTest, AnMdsReachesOnlyTheMdfBesideIt)
{
	Touch(images / "GAME.MDF");
	EXPECT_TRUE(Load(WriteMds("*.mdf")));
	EXPECT_TRUE(Load(WriteMds("GAME.MDF")));
	Touch(tmp / "secret.mdf");
	EXPECT_FALSE(Load(WriteMds((tmp / "secret.mdf").string())));
	EXPECT_FALSE(Load(WriteMds("../secret.mdf")));
}

} // namespace
