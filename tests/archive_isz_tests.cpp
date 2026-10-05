// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//
#include "dos/archive/isz_image.h"

#include "archive_fixtures.h"
#include "dos/archive/descriptor.h"
#include "test_temp_dir.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

using ArchiveFixtures::IszChunk;
using ArchiveFixtures::IszOptions;
using ArchiveFixtures::MakeIsz;
using ArchiveFixtures::PatchLe;
using ArchiveMount::Error;
namespace IszField = ArchiveFixtures::IszField;

namespace {

namespace fs = std::filesystem;

constexpr int64_t NoCap = int64_t{1} << 40;

// 150 sectors: four 64 KiB chunks and a short fifth; the third chunk is
// all zeros, the rest carry a pattern that differs per sector.
std::string Image()
{
	std::string image(150 * 2048, '\0');
	for (size_t sector = 0; sector < 150; ++sector) {
		if (sector >= 64 && sector < 96) {
			continue;
		}
		for (size_t j = 0; j < 2048; ++j) {
			image[sector * 2048 + j] = static_cast<char>(
			        (sector * 7 + j) & 0xff);
		}
	}
	return image;
}

// Rewrites chunk pointer i: the table is XORed with the key (D13.4).
void SetPointer(std::string& isz, const size_t i, const uint8_t ptr_len,
                const uint64_t type, const uint64_t length)
{
	const auto bits  = ptr_len * 8;
	const auto value = (type << (bits - 2)) | length;
	for (size_t b = 0; b < ptr_len; ++b) {
		const auto at = 48 + i * ptr_len + b;
		isz[at]       = static_cast<char>(((value >> (8 * b)) & 0xff) ^
                                            Isz::TableKey[(at - 48) % 4]);
	}
}

class IszTest : public testing::Test {
protected:
	void SetUp() override
	{
		tmp = TestTempDir::MakeUnique("isz_");
		ASSERT_FALSE(tmp.empty());
	}
	void TearDown() override
	{
		CloseFd();
		std::error_code ec = {};
		fs::remove_all(tmp, ec);
	}
	void CloseFd()
	{
		if (fd >= 0) {
			ArchiveMount::CloseDescriptor(fd);
			fd = -1;
		}
	}
	Isz::ParseResult ParseBytes(const std::string& isz, const int64_t cap = NoCap)
	{
		CloseFd();
		const auto path = tmp / "image.isz";
		ArchiveFixtures::WriteBytes(path,
		                            std::vector<uint8_t>(isz.begin(),
		                                                 isz.end()));
		fd = ArchiveMount::OpenReadOnlyShared(path);
		EXPECT_GE(fd, 0);
		return Isz::Parse(fd, static_cast<int64_t>(isz.size()), cap);
	}
	Error ReadInto(const Isz::Image& image, std::string& out,
	               std::string* detail = nullptr)
	{
		out.clear();
		return Isz::ReadAll(
		        fd,
		        image,
		        [&](const uint8_t* bytes, const size_t size) {
			        out.append(reinterpret_cast<const char*>(bytes), size);
			        return true;
		        },
		        detail);
	}
	// The whole round trip: parse, then every byte of the image back.
	void ExpectRoundTrip(const std::string& isz, const std::string& image)
	{
		const auto parsed = ParseBytes(isz);
		ASSERT_EQ(parsed.error, Error::None) << parsed.detail;
		EXPECT_EQ(parsed.image.total_bytes, image.size());
		std::string out    = {};
		std::string detail = {};
		ASSERT_EQ(ReadInto(parsed.image, out, &detail), Error::None)
		        << detail;
		EXPECT_EQ(out.size(), image.size());
		EXPECT_TRUE(out == image);
	}
	void ExpectRefused(const std::string& isz, const Error error,
	                   const char* word, const int64_t cap = NoCap)
	{
		const auto parsed = ParseBytes(isz, cap);
		EXPECT_EQ(parsed.error, error) << word << ": " << parsed.detail;
		EXPECT_NE(parsed.detail.find(word), std::string::npos)
		        << parsed.detail;
	}
	fs::path tmp = {};
	int fd       = -1;
};

TEST_F(IszTest, EveryChunkKindDecodesByteForByte)
{
	const auto image = Image();
	for (const auto kind :
	     {IszChunk::Stored, IszChunk::Zlib, IszChunk::Bzip2, IszChunk::Zeros}) {
		ExpectRoundTrip(MakeIsz(image, kind), image);
	}
}

TEST_F(IszTest, AnAllZeroChunkOfACompressedImageIsZeros)
{
	const auto parsed = ParseBytes(MakeIsz(Image(), IszChunk::Zlib));
	ASSERT_EQ(parsed.error, Error::None) << parsed.detail;
	ASSERT_EQ(parsed.image.chunks.size(), 5u);
	EXPECT_EQ(parsed.image.chunks[1].type, Isz::ChunkType::Zlib);
	EXPECT_EQ(parsed.image.chunks[2].type, Isz::ChunkType::Zeros);
	// Zeros store nothing: the next chunk starts where the second ended.
	EXPECT_EQ(parsed.image.chunks[3].offset,
	          parsed.image.chunks[1].offset + parsed.image.chunks[1].length);
}

TEST_F(IszTest, TheLastChunkIsShorterThanTheBlock)
{
	const auto parsed = ParseBytes(MakeIsz(Image(), IszChunk::Stored));
	ASSERT_EQ(parsed.error, Error::None) << parsed.detail;
	ASSERT_EQ(parsed.image.chunks.size(), 5u);
	EXPECT_EQ(parsed.image.chunks[4].length, 150u * 2048 - 4 * 65536);
}

TEST_F(IszTest, AnImageWithoutATableIsStoredRawAfterTheHeader)
{
	auto o            = IszOptions{};
	o.table           = false;
	const auto isz    = MakeIsz(Image(), IszChunk::Stored, o);
	const auto parsed = ParseBytes(isz);
	ASSERT_EQ(parsed.error, Error::None) << parsed.detail;
	EXPECT_TRUE(parsed.image.chunks.empty());
	ExpectRoundTrip(isz, Image());
}

TEST_F(IszTest, ARawImageCutShortIsRefused)
{
	auto o    = IszOptions{};
	o.table   = false;
	o.trailer = false;
	auto isz  = MakeIsz(Image(), IszChunk::Stored, o);
	isz.resize(isz.size() - 2048);
	ExpectRefused(isz, Error::Corrupt, "past the file size");
}

// A clean stream that ends early must not leave the previous chunk's bytes
// standing in the rest of the share.
TEST_F(IszTest, AChunkThatDecodesShortIsCorrupt)
{
	const std::string payload(1000, 'x');
	for (const auto kind : {Isz::ChunkType::Zlib, Isz::ChunkType::Bzip2}) {
		std::string packed(4096, '\0');
		if (kind == Isz::ChunkType::Zlib) {
			auto size = static_cast<uLongf>(packed.size());
			ASSERT_EQ(compress2(reinterpret_cast<Bytef*>(packed.data()),
			                    &size,
			                    reinterpret_cast<const Bytef*>(
			                            payload.data()),
			                    static_cast<uLong>(payload.size()),
			                    9),
			          Z_OK);
			packed.resize(size);
		} else {
			auto size = static_cast<unsigned>(packed.size());
			auto in   = payload;
			ASSERT_EQ(BZ2_bzBuffToBuffCompress(packed.data(),
			                                   &size,
			                                   in.data(),
			                                   static_cast<unsigned>(
			                                           in.size()),
			                                   9,
			                                   0,
			                                   0),
			          BZ_OK);
			packed.resize(size);
		}
		ParseBytes(packed);
		auto image        = Isz::Image{};
		image.total_bytes = 2048;
		image.block_size  = 2048;
		image.chunks      = {
                        {0, static_cast<uint32_t>(packed.size()), kind}
                };
		std::string out    = {};
		std::string detail = {};
		EXPECT_EQ(ReadInto(image, out, &detail), Error::Corrupt) << detail;
		EXPECT_NE(detail.find("chunk 0"), std::string::npos) << detail;
		EXPECT_TRUE(out.empty());
	}
}

TEST_F(IszTest, PointerLengthsOneAndEightBothWork)
{
	// One byte leaves six bits of length: 2 KiB of one byte value deflates
	// into far less than 63.
	auto runs        = std::string(8 * 2048, 'a');
	auto one         = IszOptions{};
	one.block_size   = 2048;
	one.ptr_len      = 1;
	const auto isz_1 = MakeIsz(runs, IszChunk::Zlib, one);
	ASSERT_FALSE(isz_1.empty());
	ExpectRoundTrip(isz_1, runs);

	auto eight    = IszOptions{};
	eight.ptr_len = 8;
	ExpectRoundTrip(MakeIsz(Image(), IszChunk::Bzip2, eight), Image());
}

TEST_F(IszTest, Bzip2DecodesWithAndWithoutUltraIsosPrefix)
{
	auto plain         = IszOptions{};
	plain.mangle_bzip2 = false;
	ExpectRoundTrip(MakeIsz(Image(), IszChunk::Bzip2, plain), Image());
	ExpectRoundTrip(MakeIsz(Image(), IszChunk::Bzip2), Image());
}

TEST_F(IszTest, AFileWithoutTheTrailerEndsAtItsLastChunk)
{
	auto o    = IszOptions{};
	o.trailer = false;
	ExpectRoundTrip(MakeIsz(Image(), IszChunk::Zlib, o), Image());
}

TEST_F(IszTest, AWrongSignatureIsNotAnIsz)
{
	auto isz = MakeIsz(Image(), IszChunk::Stored);
	isz[3]   = '?';
	ExpectRefused(isz, Error::NotAnArchive, "signature");
	ExpectRefused("IsZ!", Error::Corrupt, "header");
}

TEST_F(IszTest, EncryptedAndSegmentedAreRefusedByReason)
{
	auto password = MakeIsz(Image(), IszChunk::Stored);
	PatchLe(password, IszField::HasPassword, 2, 1); // AES128
	ExpectRefused(password, Error::Encrypted, "encrypted");

	auto seg_offs = MakeIsz(Image(), IszChunk::Stored);
	PatchLe(seg_offs, IszField::SegOffs, 48, 4);
	ExpectRefused(seg_offs, Error::MultiVolume, "segment");

	auto seg_size = MakeIsz(Image(), IszChunk::Stored);
	PatchLe(seg_size, IszField::SegmentSize, 1 << 20, 8);
	ExpectRefused(seg_size, Error::MultiVolume, "segment");
}

TEST_F(IszTest, HeaderFieldsOutsideTheirRangesAreCorrupt)
{
	const auto good = MakeIsz(Image(), IszChunk::Stored);
	auto patched = [&](const size_t at, const uint64_t value, const size_t width) {
		auto isz = good;
		PatchLe(isz, at, value, width);
		return isz;
	};
	ExpectRefused(patched(IszField::HeaderSize, 40, 1), Error::Corrupt, "header size");
	ExpectRefused(patched(IszField::SectSize, 512, 2), Error::Corrupt, "sector size");
	ExpectRefused(patched(IszField::BlockSize, 0, 4), Error::Corrupt, "block size");
	ExpectRefused(patched(IszField::BlockSize, 3000, 4), Error::Corrupt, "block size");
	ExpectRefused(patched(IszField::BlockSize, 32u << 20, 4),
	              Error::Corrupt,
	              "block size");
	ExpectRefused(patched(IszField::TotalSectors, 0, 4),
	              Error::Corrupt,
	              "total sectors");
	ExpectRefused(patched(IszField::NBlocks, 4, 4), Error::Corrupt, "chunk count");
	ExpectRefused(patched(IszField::PtrLen, 0, 1), Error::Corrupt, "pointer length");
	ExpectRefused(patched(IszField::PtrLen, 9, 1), Error::Corrupt, "pointer length");
	ExpectRefused(patched(IszField::PtrOffs, good.size() - 2, 4),
	              Error::Corrupt,
	              "chunk table runs past");
}

TEST_F(IszTest, AnImageOverTheCapIsTooLarge)
{
	ExpectRefused(MakeIsz(Image(), IszChunk::Stored),
	              Error::TooLarge,
	              "image",
	              150 * 2048 - 1);
}

// 10 GiB in 2 KiB blocks is five million chunks, past the table cap; the
// check runs before any table is read.
TEST_F(IszTest, MoreChunksThanTheCapAreRefusedBeforeTheTable)
{
	auto isz = MakeIsz(Image(), IszChunk::Stored);
	PatchLe(isz, IszField::BlockSize, 2048, 4);
	PatchLe(isz, IszField::TotalSectors, 5u << 20, 4);
	PatchLe(isz, IszField::NBlocks, 5u << 20, 4);
	ExpectRefused(isz, Error::Corrupt, "chunk count");
}

TEST_F(IszTest, ChunkPointersThatBreakTheLayoutAreCorrupt)
{
	const auto good = MakeIsz(Image(), IszChunk::Zlib);
	const auto ptr  = IszOptions{}.ptr_len;

	auto over = good; // a stored length over the block size
	SetPointer(over, 0, ptr, 2, 65536 + 1);
	ExpectRefused(over, Error::Corrupt, "chunk 0");

	auto empty = good; // a zlib chunk with nothing stored
	SetPointer(empty, 1, ptr, 2, 0);
	ExpectRefused(empty, Error::Corrupt, "chunk 1");

	auto past = good; // the data end past the end of the file
	SetPointer(past, 4, ptr, 1, 65536);
	ExpectRefused(past, Error::Corrupt, "data end");

	auto cut = good.substr(0, good.size() - 68 - 100); // a truncated file
	ExpectRefused(cut, Error::Corrupt, "data end");
}

TEST_F(IszTest, AZlibChunkOfJunkFailsTheReadNotTheParse)
{
	auto isz         = MakeIsz(Image(), IszChunk::Zlib);
	const auto first = ParseBytes(isz);
	ASSERT_EQ(first.error, Error::None) << first.detail;
	const auto offset = first.image.chunks[1].offset;
	for (uint32_t i = 0; i < first.image.chunks[1].length; ++i) {
		isz[offset + i] = static_cast<char>(0xA5);
	}
	const auto parsed = ParseBytes(isz);
	ASSERT_EQ(parsed.error, Error::None) << parsed.detail;
	std::string out    = {};
	std::string detail = {};
	EXPECT_EQ(ReadInto(parsed.image, out, &detail), Error::Corrupt);
	EXPECT_NE(detail.find("chunk 1"), std::string::npos) << detail;
}

TEST_F(IszTest, AShortBzip2ChunkIsCorrupt)
{
	auto isz          = MakeIsz(Image(), IszChunk::Bzip2);
	const auto ptr    = IszOptions{}.ptr_len;
	const auto parsed = ParseBytes(isz);
	ASSERT_EQ(parsed.error, Error::None) << parsed.detail;
	SetPointer(isz, 4, ptr, 3, 3);
	const auto cut = ParseBytes(isz);
	ASSERT_EQ(cut.error, Error::None) << cut.detail;
	std::string out    = {};
	std::string detail = {};
	EXPECT_EQ(ReadInto(cut.image, out, &detail), Error::Corrupt);
	EXPECT_NE(detail.find("chunk 4"), std::string::npos) << detail;
}

// Every stored chunk in the real files fills its share exactly (seven
// specimens, 2026-10-05); a short one is damage, not padding.
TEST_F(IszTest, AStoredChunkMustFillItsShareExactly)
{
	const auto ptr = IszOptions{}.ptr_len;
	for (const auto& [index, length] : {
	             std::pair{4,   100},
                     std::pair{1,   100},
                     std::pair{4, 65536}
        }) {
		auto isz = MakeIsz(Image(), IszChunk::Stored);
		SetPointer(isz, index, ptr, 1, length);
		isz += std::string(65536, '\0');
		const auto parsed = ParseBytes(isz);
		ASSERT_EQ(parsed.error, Error::None) << parsed.detail;
		std::string out    = {};
		std::string detail = {};
		EXPECT_EQ(ReadInto(parsed.image, out, &detail), Error::Corrupt)
		        << index << " " << length;
		EXPECT_NE(detail.find("chunk " + std::to_string(index)),
		          std::string::npos)
		        << detail;
	}
}

TEST_F(IszTest, ReadAllRefusesChunksThatDoNotCoverTheImage)
{
	const auto parsed = ParseBytes(MakeIsz(Image(), IszChunk::Zlib));
	ASSERT_EQ(parsed.error, Error::None) << parsed.detail;
	auto one_short = parsed.image;
	one_short.chunks.pop_back();
	auto one_over = parsed.image;
	one_over.chunks.push_back(one_over.chunks.back());
	auto no_block       = parsed.image;
	no_block.block_size = 0;
	for (const auto& image : {one_short, one_over, no_block}) {
		std::string out = {};
		EXPECT_EQ(ReadInto(image, out), Error::Corrupt);
		EXPECT_TRUE(out.empty());
	}
}

TEST_F(IszTest, ARefusingSinkStopsTheReadAsAWriteFailure)
{
	const auto parsed = ParseBytes(MakeIsz(Image(), IszChunk::Zlib));
	ASSERT_EQ(parsed.error, Error::None) << parsed.detail;
	auto calls        = 0;
	const auto result = Isz::ReadAll(
	        fd,
	        parsed.image,
	        [&](const uint8_t*, size_t) { return ++calls < 2; },
	        nullptr);
	EXPECT_EQ(result, Error::OpenFailed);
	EXPECT_EQ(calls, 2);
}

} // namespace
