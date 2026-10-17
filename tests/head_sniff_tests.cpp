// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//
#include "dos/archive/head_sniff.h"

#include "archive_fixtures.h"
#include "dos/archive/descriptor.h"
#include "test_temp_dir.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <span>
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
std::vector<uint8_t> TarHeader(const std::string& magic_at_257,
                               const std::string& name = "a.txt")
{
	std::vector<uint8_t> h(512, 0);
	std::memcpy(h.data(), name.data(), name.size());
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

// Fourteen bytes of an LZMA_Alone header: properties, dictionary, size.
std::vector<uint8_t> LzmaHead(const uint8_t props, const uint32_t dict,
                              const uint64_t size)
{
	std::vector<uint8_t> h = {props};
	for (auto i = 0; i < 4; ++i) {
		h.push_back(static_cast<uint8_t>(dict >> (8 * i)));
	}
	for (auto i = 0; i < 8; ++i) {
		h.push_back(static_cast<uint8_t>(size >> (8 * i)));
	}
	h.push_back(0x00); // the first byte of the range coder
	return h;
}

constexpr uint64_t UnknownSize = ~uint64_t{0};

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

// UltraISO's ISZ, EZB Systems specification v1.00, section 3.1.
TEST(HeadSniff, RecognisesIszByItsSignature)
{
	const auto sniff = SniffHead(Bytes("IsZ!"s + std::string(60, '\0')));
	EXPECT_EQ(sniff.kind, HeadKind::Isz);
	ASSERT_TRUE(sniff.access.has_value());
	EXPECT_EQ(*sniff.access, Access::PerMember);
	EXPECT_STREQ(HeadKindName(HeadKind::Isz), "isz");
	EXPECT_EQ(SniffHead(Bytes("IsZ?"s + std::string(60, '\0'))).kind,
	          HeadKind::Unknown);
}

// ECMA-119 8.4: the volume descriptors start at sector 16 and the system
// area before them is blank, so the head alone cannot see an ISO.
TEST(HeadSniff, AnIsoIsProbedAtSectorSixteen)
{
	const auto tmp = TestTempDir::MakeUnique("head_sniff_iso_");
	ASSERT_FALSE(tmp.empty());
	const auto iso   = ArchiveFixtures::MinimalIso();
	const auto write = [&](const char* name, const std::string& bytes) {
		ArchiveFixtures::WriteBytes(tmp / name,
		                            std::vector<uint8_t>(bytes.begin(),
		                                                 bytes.end()));
		return tmp / name;
	};
	auto zeros = iso;
	std::fill(zeros.begin(), zeros.end(), '\0');
	auto damaged           = iso;
	damaged[16 * 2048 + 1] = 'X';
	const auto sniff_of    = [](const std::filesystem::path& p) {
                const int fd = OpenReadOnlyShared(p);
                EXPECT_GE(fd, 0);
                const auto s = SniffFile(fd);
                CloseDescriptor(fd);
                return s;
	};
	const auto iso_sniff = sniff_of(write("disc.iso", iso));
	EXPECT_EQ(iso_sniff.kind, HeadKind::Iso9660);
	ASSERT_TRUE(iso_sniff.access.has_value());
	EXPECT_EQ(*iso_sniff.access, Access::PerMember);
	EXPECT_EQ(sniff_of(write("zeros.bin", zeros)).kind, HeadKind::Unknown);
	EXPECT_EQ(sniff_of(write("damaged.iso", damaged)).kind, HeadKind::Unknown);
	// The head alone stays Unknown: the probe is the file's, not the head's.
	EXPECT_EQ(SniffHead(std::span<const uint8_t>(
	                            reinterpret_cast<const uint8_t*>(iso.data()), 512))
	                  .kind,
	          HeadKind::Unknown);
	EXPECT_STREQ(HeadKindName(HeadKind::Iso9660), "ISO 9660");
	std::error_code ec = {};
	std::filesystem::remove_all(tmp, ec);
}

// libarchive's lzma_bidder_bid, case by case (archive_read_support_filter_xz.c).
TEST(HeadSniff, BidsOnLegacyLzmaAsLibarchiveDoes)
{
	const std::vector<uint8_t> lzma[] = {
	        LzmaHead(0x5d, 0x00800000, UnknownSize), // xz-utils lzma -6
	        LzmaHead(0x5e, 0x04000000, UnknownSize), // lzma -9e
	        LzmaHead(0x5d, 0x00010000, 4096),        // LZMA SDK, size known
	        LzmaHead(0x00, 0x00001000, 4096),        // a listed dictionary alone
	        LzmaHead(0x5d, 0x00300000, UnknownSize), // reduced by 1 MiB steps
	        LzmaHead(0x5d, 0x03F00000, UnknownSize),
	};
	for (const auto& head : lzma) {
		const auto sniff = SniffHead(head);
		EXPECT_EQ(sniff.kind, HeadKind::Lzma);
		ASSERT_TRUE(sniff.access.has_value());
		EXPECT_EQ(*sniff.access, Access::Sequential);
	}
	const std::vector<uint8_t> not_lzma[] = {
	        LzmaHead(0xe1, 0x00800000, UnknownSize), // properties above 224
	        LzmaHead(0x5d, 0x00801000, UnknownSize), // not a listed dictionary
	        LzmaHead(0x5d, 0x10000000, UnknownSize), // above -d27
	        LzmaHead(0x5d, 0x00300000, 4096), // reduced needs the unknown size
	        LzmaHead(0x00, 0x00300000, UnknownSize), // and 0x5d or 0x5e
	        LzmaHead(0x5d, 0x00380000, UnknownSize), // not a 1 MiB multiple
	        LzmaHead(0x5d, 0x04100000, UnknownSize), // above the reduced range
	};
	for (const auto& head : not_lzma) {
		EXPECT_EQ(SniffHead(head).kind, HeadKind::Unknown);
	}
	auto short_head = LzmaHead(0x5d, 0x00800000, UnknownSize);
	short_head.resize(13);
	EXPECT_EQ(SniffHead(short_head).kind, HeadKind::Unknown);
	EXPECT_STREQ(HeadKindName(HeadKind::Lzma), "lzma");
}

TEST(HeadSniff, TextAndTarsStartingWithABracketAreNotLzma)
{
	EXPECT_EQ(SniffHead(Bytes("]] a text file, not a stream"s)).kind,
	          HeadKind::Unknown);
	EXPECT_EQ(SniffHead(TarHeader("ustar\x00"
	                              "00"s,
	                              "]"))
	                  .kind,
	          HeadKind::Tar);
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
