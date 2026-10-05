// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//
#include "dos/archive/legacy_zip.h"

#include "dos/archive/zip_central_directory.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

const fs::path Legacy = fs::path("tests/files/archives/legacy");

std::vector<uint8_t> Slurp(const fs::path& path)
{
	std::ifstream in(path, std::ios::binary);
	return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

uint16_t Le16(const std::vector<uint8_t>& b, const size_t at)
{
	return static_cast<uint16_t>(b[at] | (b[at + 1] << 8));
}

struct Member {
	std::string name                   = {};
	ZipCentralDirectory::Record record = {};
	std::vector<uint8_t> data          = {};
};

std::vector<Member> Members(const fs::path& zip)
{
	const auto bytes  = Slurp(zip);
	const auto parsed = ZipCentralDirectory::Read(zip, 100);
	EXPECT_EQ(parsed.error, ZipCentralDirectory::Error::None) << zip;
	std::vector<Member> out = {};
	for (const auto& rec : parsed.records) {
		const auto at    = static_cast<size_t>(rec.local_header_offset);
		const auto start = at + 30 + Le16(bytes, at + 26) + Le16(bytes, at + 28);
		Member m = {};
		m.name   = rec.raw_name;
		m.record = rec;
		m.data.assign(bytes.begin() + static_cast<std::ptrdiff_t>(start),
		              bytes.begin() +
		                      static_cast<std::ptrdiff_t>(start + rec.compressed_size));
		out.push_back(std::move(m));
	}
	return out;
}

// Hands out the bytes in chunks of `step`, so refills are exercised.
LegacyZip::Source From(const std::vector<uint8_t>& data, const size_t step = 7)
{
	auto pos = std::make_shared<size_t>(0);
	return [&data, pos, step](uint8_t* buffer, const size_t capacity) {
		const auto n = std::min({step, capacity, data.size() - *pos});
		if (n > 0) {
			std::memcpy(buffer, data.data() + *pos, n);
		}
		*pos += n;
		return n;
	};
}

LegacyZip::Result DecodeWith(const Member& m, const LegacyZip::Source& in,
                      const LegacyZip::Sink& out, std::string* detail)
{
	if (m.record.method == LegacyZip::MethodShrink) {
		return LegacyZip::Unshrink(in, m.record.uncompressed_size, out, detail);
	}
	return LegacyZip::Explode(in,
	                          m.record.compressed_size,
	                          m.record.uncompressed_size,
	                          (m.record.flags & 2) != 0,
	                          (m.record.flags & 4) != 0,
	                          out,
	                          detail);
}

std::optional<std::vector<uint8_t>> Decode(const Member& m, std::string* detail)
{
	std::vector<uint8_t> out   = {};
	const LegacyZip::Sink sink = [&](const uint8_t* d, const size_t n) {
		out.insert(out.end(), d, d + n);
		return true;
	};
	if (DecodeWith(m, From(m.data), sink, detail) != LegacyZip::Result::Ok) {
		return {};
	}
	return out;
}

void ExpectAllMembersDecode(const fs::path& zip, const uint16_t method)
{
	int decoded = 0;
	for (const auto& m : Members(zip)) {
		if (m.record.method == 0) {
			continue; // stored by PKZIP's own choice
		}
		auto detail       = std::string();
		const auto output = Decode(m, &detail);
		ASSERT_TRUE(output) << m.name << ": " << detail;
		EXPECT_EQ(*output, Slurp(Legacy / "inputs" / m.name)) << m.name;
		decoded += m.record.method == method ? 1 : 0;
	}
	EXPECT_GE(decoded, 4) << zip;
}

const Member& Largest(const std::vector<Member>& ms, const uint16_t method)
{
	const Member* best = nullptr;
	for (const auto& m : ms) {
		if (m.record.method == method &&
		    (!best || m.record.uncompressed_size > best->record.uncompressed_size)) {
			best = &m;
		}
	}
	EXPECT_NE(best, nullptr) << "no member with method " << method;
	// A missing fixture fails the caller's asserts; it never reads past
	// an empty list.
	static const Member none = {};
	return best ? *best : none;
}

const LegacyZip::Sink Discard = [](const uint8_t*, size_t) { return true; };

TEST(LegacyZip, Crc32MatchesTheStandardCheckValueInPieces)
{
	// CRC-32/ISO-HDLC check value, as the zip format uses it.
	const std::string text = "123456789";
	const auto* bytes      = reinterpret_cast<const uint8_t*>(text.data());
	EXPECT_EQ(LegacyZip::Crc32(0, bytes, text.size()), 0xCBF43926u);
	EXPECT_EQ(LegacyZip::Crc32(LegacyZip::Crc32(0, bytes, 4), bytes + 4, 5), 0xCBF43926u);
	EXPECT_EQ(LegacyZip::Crc32(0, nullptr, 0), 0u);
}

TEST(LegacyZip, PkzipShrunkMembersDecode)
{
	ExpectAllMembersDecode(Legacy / "SHRUNK.ZIP", LegacyZip::MethodShrink);
}

TEST(LegacyZip, PkzipImplodedMembersDecode)
{
	ExpectAllMembersDecode(Legacy / "IMPLODED.ZIP", LegacyZip::MethodImplode);
}

TEST(LegacyZip, ATruncatedStreamIsCorrupt)
{
	for (const auto method : {LegacyZip::MethodShrink, LegacyZip::MethodImplode}) {
		const auto ms = Members(Legacy / (method == 1 ? "SHRUNK.ZIP" : "IMPLODED.ZIP"));
		auto m        = Largest(ms, method);
		m.data.resize(m.data.size() / 2);
		m.record.compressed_size = m.data.size();
		auto detail              = std::string();
		EXPECT_EQ(DecodeWith(m, From(m.data), Discard, &detail), LegacyZip::Result::Corrupt)
		        << "method " << method;
	}
}

TEST(LegacyZip, MoreOutputThanDeclaredIsCorrupt)
{
	for (const auto method : {LegacyZip::MethodShrink, LegacyZip::MethodImplode}) {
		const auto ms = Members(Legacy / (method == 1 ? "SHRUNK.ZIP" : "IMPLODED.ZIP"));
		auto m        = Largest(ms, method);
		m.record.uncompressed_size -= 1;
		uint64_t produced          = 0;
		const LegacyZip::Sink count = [&](const uint8_t*, const size_t n) {
			produced += n;
			return true;
		};
		auto detail = std::string();
		EXPECT_EQ(DecodeWith(m, From(m.data), count, &detail), LegacyZip::Result::Corrupt)
		        << "method " << method;
		EXPECT_LE(produced, m.record.uncompressed_size) << "method " << method;
	}
}

// UnZip's rule (explode.c:361, extract.c:1798): bytes left over after the
// declared size is out are a warning and the member extracts; a stream that
// needed bytes past its end is an error.
TEST(LegacyZip, TrailingCompressedBytesStillDecodeForExplode)
{
	const auto ms = Members(Legacy / "IMPLODED.ZIP");
	auto m        = Largest(ms, LegacyZip::MethodImplode);
	m.data.insert(m.data.end(), {0, 0, 0, 0});
	m.record.compressed_size = m.data.size();
	auto detail              = std::string();
	const auto output        = Decode(m, &detail);
	ASSERT_TRUE(output) << detail;
	EXPECT_EQ(*output, Slurp(Legacy / "inputs" / m.name));
}

TEST(LegacyZip, AnImplodeStreamOneByteShortIsCorrupt)
{
	const auto ms = Members(Legacy / "IMPLODED.ZIP");
	for (const auto& base : ms) {
		if (base.record.method != LegacyZip::MethodImplode) {
			continue;
		}
		auto m = base;
		m.data.pop_back();
		m.record.compressed_size = m.data.size();
		auto detail              = std::string();
		EXPECT_EQ(DecodeWith(m, From(m.data), Discard, &detail), LegacyZip::Result::Corrupt)
		        << m.name;
	}
}

TEST(LegacyZip, ASinkThatRefusesStopsTheDecoder)
{
	for (const auto method : {LegacyZip::MethodShrink, LegacyZip::MethodImplode}) {
		const auto ms = Members(Legacy / (method == 1 ? "SHRUNK.ZIP" : "IMPLODED.ZIP"));
		const auto& m = Largest(ms, method);
		auto detail   = std::string();
		EXPECT_EQ(DecodeWith(m, From(m.data), [](const uint8_t*, size_t) { return false; }, &detail),
		          LegacyZip::Result::SinkFailed)
		        << "method " << method;
	}
}

TEST(LegacyZip, AnEmptyImplodeStreamIsCorrupt)
{
	const std::vector<uint8_t> none = {};
	auto detail                     = std::string();
	EXPECT_EQ(LegacyZip::Explode(From(none), 0, 100, true, true, Discard, &detail),
	          LegacyZip::Result::Corrupt);
}

TEST(LegacyZip, AnOversubscribedTreeIsCorrupt)
{
	// get_tree reads pairs-1, then (count-1) << 4 | (bits-1) per pair: four
	// pairs of sixteen 1-bit codes are 64 codes no prefix code can hold.
	std::vector<uint8_t> stream = {0x03, 0xF0, 0xF0, 0xF0, 0xF0};
	stream.resize(64, 0);
	auto detail = std::string();
	EXPECT_EQ(LegacyZip::Explode(From(stream), stream.size(), 100, false, false, Discard, &detail),
	          LegacyZip::Result::Corrupt);
	EXPECT_NE(detail.find("length code"), std::string::npos) << detail;
}

TEST(LegacyZip, AnIncompleteTreeIsCorrupt)
{
	// 64 codes of 16 bits fill 64 of 65536 patterns: huft_build builds the
	// tables and answers 1, which UnZip's explode refuses as well.
	std::vector<uint8_t> stream = {0x03, 0xFF, 0xFF, 0xFF, 0xFF};
	stream.resize(64, 0);
	auto detail = std::string();
	EXPECT_EQ(LegacyZip::Explode(From(stream), stream.size(), 100, false, false, Discard, &detail),
	          LegacyZip::Result::Corrupt);
	EXPECT_NE(detail.find("length code"), std::string::npos) << detail;
}

TEST(LegacyZip, ATreeDescriptionLongerThanItsCodesIsCorrupt)
{
	// Five pairs of sixteen codes for a 64-code tree.
	std::vector<uint8_t> stream = {0x04, 0xF5, 0xF5, 0xF5, 0xF5, 0xF5};
	stream.resize(64, 0);
	auto detail = std::string();
	EXPECT_EQ(LegacyZip::Explode(From(stream), stream.size(), 100, false, false, Discard, &detail),
	          LegacyZip::Result::Corrupt);
}

TEST(LegacyZip, AnEmptyShrinkStreamDecodesToNothing)
{
	const std::vector<uint8_t> none = {};
	auto detail                     = std::string();
	EXPECT_EQ(LegacyZip::Unshrink(From(none), 0, Discard, &detail), LegacyZip::Result::Ok);
	EXPECT_EQ(LegacyZip::Unshrink(From(none), 1, Discard, &detail),
	          LegacyZip::Result::Corrupt);
}

int Rounds()
{
	const char* env = std::getenv("LEGACY_ZIP_ROUNDS");
	return env ? std::max(1, std::atoi(env)) : 2000;
}

TEST(LegacyZip, RandomDamageNeverCrashesOrOverruns)
{
	std::mt19937 rng(19900315);
	const auto rounds = Rounds();
	for (const auto* zip : {"SHRUNK.ZIP", "IMPLODED.ZIP"}) {
		for (const auto& base : Members(Legacy / zip)) {
			if (base.record.method == 0 || base.data.empty()) {
				continue;
			}
			for (int round = 0; round < rounds; ++round) {
				auto m = base;
				std::uniform_int_distribution<size_t> pos(0, m.data.size() - 1);
				for (int flips = 1 + round % 4; flips > 0; --flips) {
					m.data[pos(rng)] ^= static_cast<uint8_t>(1u << (rng() % 8));
				}
				if (round % 5 == 0) {
					m.data.resize(pos(rng));
					m.record.compressed_size = m.data.size();
				}
				uint64_t produced           = 0;
				const LegacyZip::Sink count = [&](const uint8_t*, const size_t n) {
					produced += n;
					return true;
				};
				auto detail = std::string();
				DecodeWith(m, From(m.data, 4096), count, &detail);
				ASSERT_LE(produced, m.record.uncompressed_size)
				        << zip << " " << m.name << " round " << round;
			}
		}
	}
}

} // namespace
