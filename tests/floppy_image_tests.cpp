// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//

#include "dos/floppy_image.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <vector>

#include <gtest/gtest.h>

#include "test_temp_dir.h"

namespace {

namespace fs = std::filesystem;

class FloppyImageTest : public testing::Test {
protected:
	void SetUp() override
	{
		tmp = TestTempDir::MakeUnique("floppy_image_");
		ASSERT_FALSE(tmp.empty());
	}
	void TearDown() override
	{
		std::error_code ec = {};
		fs::remove_all(tmp, ec);
	}
	// BPB zeros = no boot signature either, so the file is pure zeros.
	fs::path Write(const char* name, const size_t num_bytes,
	               const uint16_t total_sectors = 0,
	               const uint8_t sectors_per_track = 0, const uint8_t heads = 0)
	{
		std::vector<char> buf(num_bytes, 0);
		if (total_sectors) {
			auto put16 = [&](const size_t at, const uint16_t v) {
				buf[at]     = static_cast<char>(v & 0xff);
				buf[at + 1] = static_cast<char>(v >> 8);
			};
			put16(0x0b, 512);
			put16(0x13, total_sectors);
			put16(0x18, sectors_per_track);
			put16(0x1a, heads);
			buf[510] = '\x55';
			buf[511] = '\xAA';
		}
		const auto path = tmp / name;
		std::ofstream out(path, std::ios::binary | std::ios::trunc);
		out.write(buf.data(), static_cast<std::streamsize>(buf.size()));
		return path;
	}
	fs::path tmp = {};
};

TEST_F(FloppyImageTest, ATableSizeIsAFloppy)
{
	EXPECT_TRUE(FloppyImage::IsFloppy(Write("720.img", 737280)));
}

TEST_F(FloppyImageTest, LessThanAKilobyteMoreStillMatchesTheTable)
{
	// Sizes match by whole KB, as MOUNT's detection always did.
	EXPECT_TRUE(FloppyImage::IsFloppy(Write("160.img", 163840 + 512)));
}

TEST_F(FloppyImageTest, AKilobyteMoreWithItsBootSectorIsAFloppy)
{
	// G14, Super Space Invaders: 720K FAT12 plus 1024 bytes.
	EXPECT_TRUE(FloppyImage::IsFloppy(
	        Write("ssi.img", 737280 + 1024, 1440, 9, 2)));
}

TEST_F(FloppyImageTest, AKilobyteMoreWithoutABootSectorIsNot)
{
	EXPECT_FALSE(FloppyImage::IsFloppy(Write("plain.img", 737280 + 1024)));
}

TEST_F(FloppyImageTest, ABootSectorOfAnotherGeometryIsNoWitness)
{
	EXPECT_FALSE(FloppyImage::IsFloppy(
	        Write("liar.img", 737280 + 1024, 1440, 9, 1)));
}

TEST_F(FloppyImageTest, TwoKilobytesMoreIsNotAFloppy)
{
	// imageDisk itself takes at most one KB past a table size.
	EXPECT_FALSE(FloppyImage::IsFloppy(
	        Write("two.img", 737280 + 2048, 1440, 9, 2)));
}

TEST_F(FloppyImageTest, AMissingFileIsNotAFloppy)
{
	EXPECT_FALSE(FloppyImage::IsFloppy(tmp / "none.img"));
}

} // namespace
