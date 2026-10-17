// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//

#include "dos/drive_swap.h"
#include "dos/programs/archive_images.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "archive_fixtures.h"
#include "dos/dos.h"
#include "dosbox_test_fixture.h"
#include "ints/bios_disk.h"
#include "misc/cross.h"

#include "test_temp_dir.h"

namespace {

namespace fs = std::filesystem;

class DriveSwapTest : public testing::Test {
protected:
	fs::path tmp_dir = {};

	void SetUp() override
	{
		tmp_dir = TestTempDir::MakeUnique("drive_swap_");
		ASSERT_FALSE(tmp_dir.empty());
	}

	void TearDown() override
	{
		if (!tmp_dir.empty() && fs::exists(tmp_dir)) {
			fs::remove_all(tmp_dir);
		}
	}

	fs::path CreateDir(const std::string& name)
	{
		const auto path = tmp_dir / name;
		fs::create_directories(path);
		return path;
	}

	// A 512-byte image with a FAT boot signature: passes image
	// validation, but fatDrive cannot parse it, which makes the
	// "Failed to mount image" branch reachable without a real
	// filesystem in the file.
	fs::path CreateFatStub(const std::string& name)
	{
		const auto path = tmp_dir / name;
		fs::create_directories(path.parent_path());
		auto out = std::ofstream(path, std::ios::binary);
		auto buf = std::array<uint8_t, 512>{};
		buf[510] = 0x55;
		buf[511] = 0xAA;
		out.write(reinterpret_cast<const char*>(buf.data()), buf.size());
		return path;
	}
};

// -- ResolveImagePath --

TEST_F(DriveSwapTest, ResolveAbsolutePassesThrough)
{
	const auto abs      = tmp_dir / "nonexistent.img";
	const auto resolved = DriveSwap::ResolveImagePath(abs, tmp_dir, {});
	EXPECT_EQ(resolved, abs);
}

TEST_F(DriveSwapTest, ResolveRelativeUnderAnchor)
{
	const auto anchor = CreateDir("game");
	CreateFatStub("game/disk2.img");

	const auto resolved = DriveSwap::ResolveImagePath("disk2.img", anchor, {});
	EXPECT_EQ(resolved, anchor / "disk2.img");
}

TEST_F(DriveSwapTest, ResolveRelativeUnderRoot)
{
	const auto anchor = CreateDir("conf");
	const auto root   = CreateDir("images");
	CreateFatStub("images/disk2.img");

	const auto resolved = DriveSwap::ResolveImagePath("disk2.img", anchor, {root});
	EXPECT_EQ(resolved, root / "disk2.img");
}

TEST_F(DriveSwapTest, ResolveRelativeMemberUnderRoot)
{
	const auto root = CreateDir("images");
	auto error      = std::string();
	ASSERT_TRUE(ArchiveFixtures::WriteArchive(root / "game.zip",
	                                          ArchiveFixtures::Kind::ZipStored,
	                                          {
	                                                  {"disk2.img",
	                                                   std::string(4096, 'x')}
        },
	                                          nullptr,
	                                          &error))
	        << error;
	const auto resolved = DriveSwap::ResolveImagePath("game.zip/disk2.img",
	                                                  {},
	                                                  {root});
	EXPECT_EQ(resolved, root / "game.zip/disk2.img");
}

TEST_F(DriveSwapTest, ResolveRelativeChainUnderRoot)
{
	// A relative chain is found under a root like a one-level member:
	// the splitter walks the host part on disk and leaves the rest.
	const auto root = CreateDir("images");
	auto error      = std::string();
	ASSERT_TRUE(ArchiveFixtures::WriteArchive(root / "inner.zip",
	                                          ArchiveFixtures::Kind::ZipStored,
	                                          {
	                                                  {"disk2.img",
	                                                   std::string(4096, 'x')}
        },
	                                          nullptr,
	                                          &error))
	        << error;
	ASSERT_TRUE(ArchiveFixtures::WriteArchive(
	        root / "outer.zip",
	        ArchiveFixtures::Kind::ZipStored,
	        {
	                {"inner.zip", ArchiveFixtures::ReadAll(root / "inner.zip")}
        },
	        nullptr,
	        &error))
	        << error;
	const auto resolved = DriveSwap::ResolveImagePath("outer.zip/inner.zip/disk2.img",
	                                                  {},
	                                                  {root});
	EXPECT_EQ(resolved, root / "outer.zip/inner.zip/disk2.img");
}

TEST_F(DriveSwapTest, ResolveAnchorWinsOverRoot)
{
	const auto anchor = CreateDir("game");
	const auto root   = CreateDir("images");
	CreateFatStub("game/disk.img");
	CreateFatStub("images/disk.img");

	const auto resolved = DriveSwap::ResolveImagePath("disk.img", anchor, {root});
	EXPECT_EQ(resolved, anchor / "disk.img");
}

TEST_F(DriveSwapTest, ResolveFirstRootWins)
{
	const auto root_a = CreateDir("a");
	const auto root_b = CreateDir("b");
	CreateFatStub("a/disk.img");
	CreateFatStub("b/disk.img");

	const auto resolved = DriveSwap::ResolveImagePath("disk.img",
	                                                  {},
	                                                  {root_a, root_b});
	EXPECT_EQ(resolved, root_a / "disk.img");
}

TEST_F(DriveSwapTest, ResolveEmptyAnchorSkipped)
{
	const auto root = CreateDir("images");
	CreateFatStub("images/disk.img");

	const auto resolved = DriveSwap::ResolveImagePath("disk.img", {}, {root});
	EXPECT_EQ(resolved, root / "disk.img");
}

TEST_F(DriveSwapTest, ResolveMissPassesThrough)
{
	const auto root = CreateDir("images");

	const auto resolved = DriveSwap::ResolveImagePath("nowhere.img", {}, {root});
	EXPECT_EQ(resolved, fs::path("nowhere.img"));
}

// -- Swap --

TEST_F(DriveSwapTest, SwapRefusedWhenLocked)
{
	const auto root = CreateDir("images");
	const auto img  = CreateFatStub("images/disk.img");

	const auto result = DriveSwap::Swap('A', img, true, {}, {root});
	EXPECT_FALSE(result.ok);
	EXPECT_EQ(result.error, "mount is locked");
}

TEST_F(DriveSwapTest, SwapRefusesNonAlphaDrive)
{
	const auto root = CreateDir("images");
	const auto img  = CreateFatStub("images/disk.img");

	const auto result = DriveSwap::Swap('1', img, false, {}, {root});
	EXPECT_FALSE(result.ok);
	EXPECT_EQ(result.error, "Invalid drive letter");
}

TEST_F(DriveSwapTest, SwapDeniedWithoutRootsOrAnchor)
{
	const auto img = CreateFatStub("disk.img");

	const auto result = DriveSwap::Swap('A', img, false, {}, {});
	EXPECT_FALSE(result.ok);
	EXPECT_EQ(result.error, "Blocked by mount policy");
}

TEST_F(DriveSwapTest, SwapDeniedOutsideRoots)
{
	const auto root    = CreateDir("images");
	const auto outside = CreateFatStub("elsewhere/disk.img");

	const auto result = DriveSwap::Swap('A', outside, false, {}, {root});
	EXPECT_FALSE(result.ok);
	EXPECT_EQ(result.error, "Blocked by mount policy");
}

TEST_F(DriveSwapTest, SwapDeniedTraversalOutOfRoot)
{
	const auto root = CreateDir("images");
	CreateFatStub("outside.img");

	const auto result = DriveSwap::Swap(
	        'A', fs::path("..") / "outside.img", false, {}, {root});
	EXPECT_FALSE(result.ok);
	EXPECT_EQ(result.error, "Blocked by mount policy");
}

// The stub passes policy validation and reaches fatDrive construction,
// which fails on it: proof the pipeline ran end to end. A swap that
// succeeds against a real FAT filesystem is covered by the integration
// tier (test_mount_policy.py), where mformat builds a genuine image.
TEST_F(DriveSwapTest, SwapAbsoluteStubReachesMount)
{
	const auto root = CreateDir("images");
	const auto img  = CreateFatStub("images/disk.img");

	const auto result = DriveSwap::Swap('A', img, false, {}, {root});
	EXPECT_FALSE(result.ok);
	EXPECT_EQ(result.error, "Failed to mount image");
}

TEST_F(DriveSwapTest, SwapRelativeNameReachesMount)
{
	const auto root = CreateDir("images");
	CreateFatStub("images/disk2.img");

	const auto result = DriveSwap::Swap('A', "disk2.img", false, {}, {root});
	EXPECT_FALSE(result.ok);
	EXPECT_EQ(result.error, "Failed to mount image");
}

TEST_F(DriveSwapTest, SwapLowercaseDriveAccepted)
{
	const auto root = CreateDir("images");
	CreateFatStub("images/disk.img");

	const auto result = DriveSwap::Swap('a', "disk.img", false, {}, {root});
	EXPECT_FALSE(result.ok);
	EXPECT_EQ(result.error, "Failed to mount image");
}

// -- Raw BIOS disks (booter games, ada-plh7) --

// A floppy imageDisk writes the drive count into the BIOS data area when
// it is built, so these tests need the emulated machine
class DriveSwapRawTest : public DOSBoxTestFixture {
protected:
	fs::path tmp_dir = {};
	fs::path root    = {};

	void SetUp() override
	{
		DOSBoxTestFixture::SetUp();
		tmp_dir = TestTempDir::MakeUnique("drive_swap_raw_");
		ASSERT_FALSE(tmp_dir.empty());
		root = tmp_dir / "images";
		fs::create_directories(root);
	}

	void TearDown() override
	{
		// Swaps put open images into the global disk and drive tables
		imageDiskList.fill(nullptr);
		diskSwap.fill(nullptr);
		Drives[0].reset();
		DOSBoxTestFixture::TearDown();
		std::error_code ec = {};
		fs::remove_all(tmp_dir, ec);
	}

	// A booter disk: a floppy-sized image with no FAT, first byte marked
	// BOOT names a listed disk by the policy's resolved path, so the swap's
	// match by name sees the spelling it sees in the engine.
	void BootListDisk(const size_t slot, const fs::path& image)
	{
		const auto verdict = MountPolicy::ValidateImagePath(image,
		                                                    MountOrigin::Api,
		                                                    {root});
		ASSERT_TRUE(verdict.allowed);
		const auto name  = verdict.resolved.string();
		bool is_readonly = false;
		auto* file       = fopen_wrap_ro_fallback(name, is_readonly);
		ASSERT_NE(file, nullptr);
		diskSwap[slot] = std::make_shared<imageDisk>(file, name.c_str(), 160, false);
	}

	fs::path CreateBooterImage(const std::string& name, const uint8_t marker)
	{
		constexpr size_t SizeBytes = 163840; // 160K, no FAT signature
		const auto path            = tmp_dir / name;
		fs::create_directories(path.parent_path());
		auto out  = std::ofstream(path, std::ios::binary);
		auto data = std::vector<char>(SizeBytes, 0);
		data[0]   = static_cast<char>(marker);
		out.write(data.data(), static_cast<std::streamsize>(data.size()));
		return path;
	}

	// What 'mount a <image> -t floppy -fs none' or BOOT leaves behind: a
	// raw BIOS disk in slot 0 and no DOS drive
	void InsertRawDiskA(const fs::path& image)
	{
		bool is_readonly = false;
		auto* file = fopen_wrap_ro_fallback(image.string(), is_readonly);
		ASSERT_NE(file, nullptr);
		const auto size_kb = static_cast<uint32_t>(fs::file_size(image) / 1024);
		imageDiskList[0] = std::make_shared<imageDisk>(
		        file, image.string().c_str(), size_kb, false);
	}

	static uint8_t FirstByteOfDiskA()
	{
		auto sector = std::array<uint8_t, 512>{};
		EXPECT_EQ(imageDiskList[0]->Read_AbsoluteSector(0, sector.data()), 0);
		return sector[0];
	}
};

TEST_F(DriveSwapRawTest, SwapReplacesARawDiskWithoutADosDrive)
{
	InsertRawDiskA(CreateBooterImage("images/disk1.img", 0x11));
	const auto disk2 = CreateBooterImage("images/disk2.img", 0x22);
	ASSERT_EQ(FirstByteOfDiskA(), 0x11);

	const auto result = DriveSwap::Swap('A', disk2, false, {}, {root});

	EXPECT_TRUE(result.ok) << result.error;
	ASSERT_NE(imageDiskList[0], nullptr);
	EXPECT_EQ(FirstByteOfDiskA(), 0x22);
	EXPECT_EQ(Drives[0], nullptr);
}

// BOOT keeps its own list of the booted images for Ctrl+F4; after an API
// swap it must hand back the swapped disk, not the one BOOT loaded
TEST_F(DriveSwapRawTest, RawSwapKeepsBootsSwapListInStep)
{
	InsertRawDiskA(CreateBooterImage("images/disk1.img", 0x11));
	diskSwap[0]      = imageDiskList[0];
	const auto disk2 = CreateBooterImage("images/disk2.img", 0x22);

	const auto result = DriveSwap::Swap('A', disk2, false, {}, {root});
	ASSERT_TRUE(result.ok) << result.error;

	swapInDisks(0);
	EXPECT_EQ(FirstByteOfDiskA(), 0x22);
}

// ada-ciru item 3: the drive a swap replaces stays alive in the cycle
// list with its stdio buffer, so a sector it wrote must be in the file by
// the time the swap back opens the image a second time.
TEST_F(DriveSwapRawTest, AWrittenSectorIsInTheFileBeforeASecondOpen)
{
	const auto image = CreateBooterImage("images/disk1.img", 0x11);
	InsertRawDiskA(image);
	auto sector = std::array<uint8_t, 512>{};
	sector.fill(0x5A);
	ASSERT_EQ(imageDiskList[0]->Write_AbsoluteSector(7, sector.data()), 0);

	auto in = std::ifstream(image, std::ios::binary);
	in.seekg(7 * 512);
	auto seen = std::array<char, 512>{};
	in.read(seen.data(), static_cast<std::streamsize>(seen.size()));
	EXPECT_EQ(static_cast<uint8_t>(seen[0]), 0x5A);
	EXPECT_EQ(static_cast<uint8_t>(seen[511]), 0x5A);
}

// ada-ciru item 1: BOOT already has the image open in its Ctrl+F4 list; a
// swap to it hands that object to the slot instead of opening the file a
// second time with its own buffer.
TEST_F(DriveSwapRawTest, RawSwapReusesTheDiskBootKeepsInItsList)
{
	InsertRawDiskA(CreateBooterImage("images/disk1.img", 0x11));
	diskSwap[0]      = imageDiskList[0];
	const auto disk2 = CreateBooterImage("images/disk2.img", 0x22);
	BootListDisk(1, disk2);

	const auto result = DriveSwap::Swap('A', disk2, false, {}, {root});
	ASSERT_TRUE(result.ok) << result.error;
	EXPECT_EQ(imageDiskList[0], diskSwap[1]);
	EXPECT_EQ(FirstByteOfDiskA(), 0x22);
}

TEST_F(DriveSwapRawTest, AReadOnlyRawSwapDoesNotTakeBootsWritableDisk)
{
	InsertRawDiskA(CreateBooterImage("images/disk1.img", 0x11));
	diskSwap[0]      = imageDiskList[0];
	const auto disk2 = CreateBooterImage("images/disk2.img", 0x22);
	BootListDisk(1, disk2);

	ArchiveImages::SetDriveMode(0, ArchiveImages::DriveMode{.read_only = true});
	const auto result = DriveSwap::Swap('A', disk2, false, {}, {root});
	ArchiveImages::SetDriveMode(0, {});
	ASSERT_TRUE(result.ok) << result.error;
	EXPECT_NE(imageDiskList[0], diskSwap[1]);
	EXPECT_EQ(FirstByteOfDiskA(), 0x22);
}

TEST_F(DriveSwapRawTest, RawSwapRefusesAnImageThatIsNoFloppySize)
{
	InsertRawDiskA(CreateBooterImage("images/disk1.img", 0x11));
	// A FAT boot signature passes image validation; 512 bytes fit no
	// floppy geometry, so it cannot stand in for a floppy disk
	const auto odd = tmp_dir / "images" / "odd.img";
	{
		auto out = std::ofstream(odd, std::ios::binary);
		auto buf = std::array<char, 512>{};
		buf[510] = static_cast<char>(0x55);
		buf[511] = static_cast<char>(0xAA);
		out.write(buf.data(), buf.size());
	}

	const auto result = DriveSwap::Swap('A', odd, false, {}, {root});

	EXPECT_FALSE(result.ok);
	EXPECT_EQ(result.error, "Not a floppy image");
	EXPECT_EQ(FirstByteOfDiskA(), 0x11);
}

TEST_F(DriveSwapRawTest, RawSwapTakesAFloppyWithAKilobyteMore)
{
	// G14, Super Space Invaders' disks: 720K FAT12 plus 1024 bytes, the
	// boot sector's BPB naming the 720K geometry.
	InsertRawDiskA(CreateBooterImage("images/disk1.img", 0x11));
	const auto disk = tmp_dir / "images" / "ssi.img";
	{
		auto buf   = std::vector<char>(737280 + 1024, 0);
		auto put16 = [&](const size_t at, const uint16_t v) {
			buf[at]     = static_cast<char>(v & 0xff);
			buf[at + 1] = static_cast<char>(v >> 8);
		};
		buf[0] = static_cast<char>(0xEB);
		put16(0x0b, 512);
		put16(0x13, 1440);
		put16(0x18, 9);
		put16(0x1a, 2);
		buf[510] = static_cast<char>(0x55);
		buf[511] = static_cast<char>(0xAA);
		auto out = std::ofstream(disk, std::ios::binary);
		out.write(buf.data(), static_cast<std::streamsize>(buf.size()));
	}

	const auto result = DriveSwap::Swap('A', disk, false, {}, {root});

	EXPECT_TRUE(result.ok) << result.error;
	EXPECT_EQ(FirstByteOfDiskA(), 0xEB);
}

TEST_F(DriveSwapRawTest, RawSwapStillChecksTheMountPolicy)
{
	InsertRawDiskA(CreateBooterImage("images/disk1.img", 0x11));
	const auto outside = CreateBooterImage("elsewhere/disk2.img", 0x22);

	const auto result = DriveSwap::Swap('A', outside, false, {}, {root});

	EXPECT_FALSE(result.ok);
	EXPECT_EQ(result.error, "Blocked by mount policy");
	EXPECT_EQ(FirstByteOfDiskA(), 0x11);
}

TEST_F(DriveSwapRawTest, RawSwapIsRefusedWhenLocked)
{
	InsertRawDiskA(CreateBooterImage("images/disk1.img", 0x11));
	const auto disk2 = CreateBooterImage("images/disk2.img", 0x22);

	const auto result = DriveSwap::Swap('A', disk2, true, {}, {root});

	EXPECT_FALSE(result.ok);
	EXPECT_EQ(FirstByteOfDiskA(), 0x11);
}

} // namespace
