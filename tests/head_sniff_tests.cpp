// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//
#include "dos/archive/head_sniff.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace ArchiveMount;
using namespace std::string_literals;

namespace {

// Takes std::string, never a C literal: the magics carry NUL bytes and
// strlen would cut them (the "..."s suffix keeps the length).
std::vector<uint8_t> Bytes(const std::string& s)
{
	return std::vector<uint8_t>(s.begin(), s.end());
}

// A 512-byte tar header with the given magic field and a checksum that
// adds up, the way tar(1) writes it: the field counts as eight spaces.
std::vector<uint8_t> TarHeader(const std::string& magic_at_257)
{
	std::vector<uint8_t> h(512, 0);
	std::memcpy(h.data(), "a.txt", 5);
	std::memcpy(h.data() + 100, "0000644", 7);
	std::memcpy(h.data() + 124, "00000000005", 11);
	std::memcpy(h.data() + 257, magic_at_257.data(), magic_at_257.size());
	std::memset(h.data() + 148, ' ', 8);
	unsigned sum = 0;
	for (const auto b : h) {
		sum += b;
	}
	char field[8] = {};
	std::snprintf(field, sizeof(field), "%06o", sum);
	std::memcpy(h.data() + 148, field, 6);
	h[154] = 0;
	h[155] = ' ';
	return h;
}

} // namespace

TEST(HeadSniff, KnowsEveryMagicAndItsAccessClass)
{
	struct Case {
		std::vector<uint8_t> head;
		HeadKind kind;
		Access access;
	};
	const Case cases[] = {
	        {               Bytes("PK\x03\x04rest"s),HeadKind::Zip,Access::PerMember                                                                          },
	        {         Bytes("Rar!\x1a\x07\x00rest"s),  HeadKind::Rar4,  Access::PerMember},
	        {     Bytes("Rar!\x1a\x07\x01\x00rest"s),  HeadKind::Rar5, Access::Sequential},
	        {        Bytes("7z\xbc\xaf\x27\x1c"
        "rest"s),
	         HeadKind::SevenZip,
	         Access::Sequential	                                                  },
	        {             Bytes("\x1f\x8b\x08rest"s),  HeadKind::Gzip, Access::Sequential},
	        {	             Bytes("BZh9rest"s), HeadKind::Bzip2, Access::Sequential},
	        {Bytes("\xfd"
"7zXZ\x00rest"s),
	         HeadKind::Xz,
	         Access::Sequential	                                                  },
	        {        Bytes("\x28\xb5\x2f\xfd"
        "rest"s),
	         HeadKind::Zstd,
	         Access::Sequential	                                                  },
	        {      TarHeader("ustar\x00"
      "00"s),
	         HeadKind::Tar,
	         Access::Sequential	                                                  },
	        {              TarHeader("ustar  \x00"s),   HeadKind::Tar, Access::Sequential},
	        {	                  TarHeader(""),   HeadKind::Tar, Access::Sequential}, // v7:  checksum
  // only
	};
	for (const auto& c : cases) {
		const auto sniff = SniffHead(c.head);
		EXPECT_EQ(sniff.kind, c.kind);
		ASSERT_TRUE(sniff.access.has_value());
		EXPECT_EQ(*sniff.access, c.access);
	}
}

TEST(HeadSniff, UnknownWhenNothingMatches)
{
	EXPECT_EQ(SniffHead(Bytes("MZ this is a program"s)).kind, HeadKind::Unknown);
	EXPECT_FALSE(SniffHead(Bytes("MZ this is a program"s)).access.has_value());
	EXPECT_EQ(SniffHead({}).kind, HeadKind::Unknown);
	EXPECT_EQ(SniffHead(Bytes("PK"s)).kind, HeadKind::Unknown); // too short
	// A tar header whose checksum does not add up is not a tar.
	auto broken = TarHeader("");
	broken[0]   = 'b';
	EXPECT_EQ(SniffHead(broken).kind, HeadKind::Unknown);
	// 512 bytes of zeros (an empty tar's end blocks) is not a tar either:
	// the checksum field is all zero, not octal spaces.
	EXPECT_EQ(SniffHead(std::vector<uint8_t>(512, 0)).kind, HeadKind::Unknown);
}

TEST(HeadSniff, SniffsAFileThroughItsDescriptorWithoutMovingIt)
{
	// Covered end to end by ArchiveReaderTest and the helper client tests;
	// here only the contract that a bad descriptor is Unknown.
	EXPECT_EQ(SniffFile(-1).kind, HeadKind::Unknown);
}
