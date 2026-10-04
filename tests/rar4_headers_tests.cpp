// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//
#include "dos/archive/rar4_headers.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

using namespace Rar4Headers;

namespace {

void U16(std::vector<uint8_t>& out, const uint16_t v)
{
	out.push_back(static_cast<uint8_t>(v));
	out.push_back(static_cast<uint8_t>(v >> 8));
}
void U32(std::vector<uint8_t>& out, const uint32_t v)
{
	for (int i = 0; i < 4; ++i) {
		out.push_back(static_cast<uint8_t>(v >> (8 * i)));
	}
}

// Marker, main header, then one file header per spec with its data.
struct File {
	std::string name;
	std::string data;
	uint32_t crc;
	uint32_t ftime;
	bool dir = false;
};

std::vector<uint8_t> Rar4(const std::vector<File>& files, const bool end_block = true)
{
	std::vector<uint8_t> out = {'R', 'a', 'r', '!', 0x1a, 0x07, 0x00};
	// main header: crc, type 0x73, flags 0, size 13, then 6 reserved bytes
	U16(out, 0);
	out.push_back(0x73);
	U16(out, 0);
	U16(out, 13);
	for (int i = 0; i < 6; ++i) {
		out.push_back(0);
	}
	for (const auto& f : files) {
		const auto head_size = static_cast<uint16_t>(32 + f.name.size());
		U16(out, 0);
		out.push_back(0x74);
		U16(out, f.dir ? 0x00e0 : 0x0000);
		U16(out, head_size);
		U32(out, static_cast<uint32_t>(f.data.size())); // PACK_SIZE,
		                                                // stored
		U32(out, static_cast<uint32_t>(f.data.size())); // UNP_SIZE
		out.push_back(2);                               // HOST_OS win32
		U32(out, f.crc);
		U32(out, f.ftime);
		out.push_back(29);   // UNP_VER
		out.push_back(0x30); // METHOD store
		U16(out, static_cast<uint16_t>(f.name.size()));
		U32(out, f.dir ? 0x10 : 0x20); // ATTR
		out.insert(out.end(), f.name.begin(), f.name.end());
		out.insert(out.end(), f.data.begin(), f.data.end());
	}
	if (end_block) {
		U16(out, 0);
		out.push_back(0x7b);
		U16(out, 0);
		U16(out, 7);
	}
	return out;
}

} // namespace

TEST(Rar4Headers, WalksFileHeadersAndReadsTheStoredFields)
{
	const auto bytes  = Rar4({
                {"GAME.EXE", "xxxx", 0x11223344u, 0x14210800u},
                {"data", "", 0, 0x14210800u, true},
                {"data/l.dat", "ll", 0xdeadbeefu, 0x14211000u},
        });
	const auto walked = Parse(bytes, 100);
	ASSERT_EQ(walked.error, Error::None);
	ASSERT_EQ(walked.files.size(), 3u);
	EXPECT_EQ(walked.files[0].crc32, 0x11223344u);
	EXPECT_EQ(walked.files[0].dos_time, 0x14210800u);
	EXPECT_FALSE(walked.files[0].is_dir);
	EXPECT_EQ(walked.files[0].unpacked_size, 4u);
	EXPECT_TRUE(walked.files[1].is_dir);
	EXPECT_EQ(walked.files[2].crc32, 0xdeadbeefu);
}

TEST(Rar4Headers, ServiceBlocksAndAddSizesAreSkippedNotCounted)
{
	auto bytes = Rar4(
	        {
	                {"a", "1", 1, 1}
        },
	        /*end_block=*/false);
	// A comment block (0x75) with the ADD_SIZE flag and 5 bytes of data,
	// then a second file, then the end block.
	U16(bytes, 0);
	bytes.push_back(0x75);
	U16(bytes, 0x8000);
	U16(bytes, 11);
	U32(bytes, 5);
	for (int i = 0; i < 5; ++i) {
		bytes.push_back('c');
	}
	const auto second = Rar4(
	        {
	                {"b", "22", 2, 2}
        },
	        true);
	bytes.insert(bytes.end(), second.begin() + 7 + 13, second.end());
	const auto walked = Parse(bytes, 100);
	ASSERT_EQ(walked.error, Error::None);
	ASSERT_EQ(walked.files.size(), 2u);
	EXPECT_EQ(walked.files[1].crc32, 2u);
}

TEST(Rar4Headers, RefusesWhatDoesNotAddUp)
{
	EXPECT_EQ(Parse(std::vector<uint8_t>{'P', 'K'}, 100).error, Error::NotRar4);
	auto truncated = Rar4({
	        {"a", "12345", 1, 1}
        });
	truncated.resize(truncated.size() - 9); // cut into the data and the end
	                                        // block
	EXPECT_EQ(Parse(truncated, 100).error, Error::Truncated);
	auto tiny_head        = Rar4({
                {"a", "1", 1, 1}
        });
	tiny_head[7 + 13 + 5] = 3; // HEAD_SIZE below 7
	tiny_head[7 + 13 + 6] = 0;
	EXPECT_EQ(Parse(tiny_head, 100).error, Error::Malformed);
	EXPECT_EQ(Parse(Rar4({
	                        {"a", "1", 1, 1},
                                {"b", "2", 2, 2}
        }),
	                1)
	                  .error,
	          Error::TooManyEntries);
}

TEST(Rar4Headers, ALargeSizeThatWrapsThePositionIsMalformed)
{
	// Marker and main header, then one LARGE file header (flag 0x0100,
	// 40 fixed bytes) whose 64-bit packed size wraps pos + head + data
	// back onto the header itself.
	std::vector<uint8_t> out = {'R', 'a', 'r', '!', 0x1a, 0x07, 0x00};
	U16(out, 0);
	out.push_back(0x73);
	U16(out, 0);
	U16(out, 13);
	for (int i = 0; i < 6; ++i) {
		out.push_back(0);
	}
	const uint16_t head_size = 40 + 1;
	U16(out, 0);
	out.push_back(0x74);
	U16(out, 0x0100);
	U16(out, head_size);
	U32(out, 0xFFFFFFFFu - head_size + 1); // PACK_SIZE low: wraps to pos
	U32(out, 1);                           // UNP_SIZE
	out.push_back(2);
	U32(out, 1);
	U32(out, 1);
	out.push_back(29);
	out.push_back(0x30);
	U16(out, 1);
	U32(out, 0x20);
	U32(out, 0xFFFFFFFFu); // HIGH_PACK_SIZE
	U32(out, 0);           // HIGH_UNP_SIZE
	out.push_back('a');
	EXPECT_EQ(Parse(out, 100).error, Error::Malformed);
}

TEST(Rar4Headers, AMissingEndBlockIsTheEndOfTheFile)
{
	const auto walked = Parse(Rar4(
	                                  {
	                                          {"a", "1", 1, 1}
        },
	                                  /*end_block=*/false),
	                          100);
	ASSERT_EQ(walked.error, Error::None);
	EXPECT_EQ(walked.files.size(), 1u);
}
