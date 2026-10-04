// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//
#include "dos/archive/helper_protocol.h"

#include <gtest/gtest.h>

#include <filesystem>

#include "archive_fixtures.h"
#include "dos/archive/archive_reader.h"
#include "test_temp_dir.h"

using namespace ArchiveHelper;
using ArchiveMount::Access;
using ArchiveMount::Entry;
using ArchiveMount::Error;
using ArchiveMount::Format;

TEST(HelperProtocol, FrameHeaderIsTwelveBytesLittleEndian)
{
	const auto payload = std::vector<uint8_t>{1, 2, 3};
	const auto frame   = EncodeFrame(Type::Materialize, payload);
	ASSERT_EQ(frame.size(), HeaderSize + 3);
	EXPECT_EQ(frame[0], 'D');
	EXPECT_EQ(frame[1], 'B');
	EXPECT_EQ(frame[2], 'A');
	EXPECT_EQ(frame[3], 'H');
	EXPECT_EQ(Version, 2); // 2a: the open request carries a descriptor
	EXPECT_EQ(frame[4], Version & 0xff);
	EXPECT_EQ(frame[5], Version >> 8);
	EXPECT_EQ(frame[6], static_cast<uint8_t>(Type::Materialize));
	EXPECT_EQ(frame[7], 0);
	EXPECT_EQ(frame[8], 3);
	EXPECT_EQ(frame[9], 0);
	EXPECT_EQ(frame[10], 0);
	EXPECT_EQ(frame[11], 0);
	EXPECT_EQ(frame[12], 1);
}

TEST(HelperProtocol, HeaderDecodesAndRefusesTheWrongThings)
{
	auto frame      = EncodeFrame(Type::Hello, {});
	Type type       = Type::Quit;
	uint32_t length = 99;
	EXPECT_TRUE(DecodeHeader(frame, MaxRequestPayload, type, length));
	EXPECT_EQ(type, Type::Hello);
	EXPECT_EQ(length, 0u);

	auto bad_magic = frame;
	bad_magic[0]   = 'X';
	EXPECT_FALSE(DecodeHeader(bad_magic, MaxRequestPayload, type, length));

	auto bad_version = frame;
	bad_version[4]   = static_cast<uint8_t>(Version + 1);
	EXPECT_FALSE(DecodeHeader(bad_version, MaxRequestPayload, type, length));

	auto too_long = EncodeFrame(Type::Hello, {});
	too_long[8]   = 1;
	too_long[9]   = 0;
	too_long[10]  = 1; // 65537 bytes
	EXPECT_FALSE(DecodeHeader(too_long, MaxRequestPayload, type, length));
	EXPECT_TRUE(DecodeHeader(too_long, MaxReplyPayload, type, length));

	auto unknown = frame;
	unknown[6]   = 0x7e;
	EXPECT_FALSE(DecodeHeader(unknown, MaxRequestPayload, type, length));

	EXPECT_FALSE(DecodeHeader(std::span(frame).first(HeaderSize - 1),
	                          MaxRequestPayload,
	                          type,
	                          length));
}

TEST(HelperProtocol, ScalarsAndStringsRoundTrip)
{
	auto w = Writer{};
	w.U8(7);
	w.U16(0xbeef);
	w.U32(0xdeadbeef);
	w.I64(-5);
	w.Str("na\xc3\xafve");
	const auto bytes = w.Take();
	auto r           = Reader(bytes);
	uint8_t a        = 0;
	uint16_t b       = 0;
	uint32_t c       = 0;
	int64_t d        = 0;
	std::string s    = {};
	EXPECT_TRUE(r.U8(a) && r.U16(b) && r.U32(c) && r.I64(d) && r.Str(s));
	EXPECT_EQ(a, 7);
	EXPECT_EQ(b, 0xbeef);
	EXPECT_EQ(c, 0xdeadbeefu);
	EXPECT_EQ(d, -5);
	EXPECT_EQ(s, "na\xc3\xafve");
	EXPECT_TRUE(r.AtEnd());
	EXPECT_FALSE(r.U8(a));
}

TEST(HelperProtocol, AStringLongerThanTheBufferIsRefused)
{
	auto w = Writer{};
	w.U32(100);
	w.U8('x');
	const auto bytes = w.Take();
	auto r           = Reader(bytes);
	std::string s    = {};
	EXPECT_FALSE(r.Str(s));
}

TEST(HelperProtocol, OpenRequestRoundTrips)
{
	auto in                    = OpenRequest{};
	in.path                    = "/games/x.zip";
	in.limits.max_entries      = 10;
	in.limits.max_total_bytes  = 20;
	in.limits.max_member_bytes = 30;
	in.descriptor_follows      = true;
	in.handle                  = 0x1122334455667788ull;
	auto out                   = OpenRequest{};
	EXPECT_TRUE(DecodeOpenRequest(EncodeOpenRequest(in), out));
	EXPECT_EQ(out.path, in.path);
	EXPECT_EQ(out.limits.max_entries, 10);
	EXPECT_EQ(out.limits.max_total_bytes, 20);
	EXPECT_EQ(out.limits.max_member_bytes, 30);
	EXPECT_TRUE(out.descriptor_follows);
	EXPECT_EQ(out.handle, 0x1122334455667788ull);
	// The flag byte is 0 or 1, nothing else.
	auto bytes              = EncodeOpenRequest(in);
	bytes[bytes.size() - 9] = 2;
	EXPECT_FALSE(DecodeOpenRequest(bytes, out));
}

TEST(HelperProtocol, IndexReplyRoundTrips)
{
	auto in           = IndexReply{};
	in.format         = Format::SevenZip;
	in.access         = Access::Sequential;
	in.total_bytes    = 123;
	in.archive_length = 456;
	in.identity       = std::string(64, 'a');
	in.stat.size      = 456;
	in.stat.mtime     = 789;
	auto e            = Entry{};
	e.ordinal         = 3;
	e.utf8_path       = "dir/f.txt";
	e.dos_path        = "DIR\\F.TXT";
	e.size            = 9;
	e.mtime           = 631152000;
	e.has_crc         = true;
	e.crc32           = 0x1234;
	in.entries.push_back(e);
	e.ordinal   = 4;
	e.utf8_path = "dir";
	e.dos_path  = "DIR";
	e.is_dir    = true;
	e.has_crc   = false;
	e.size      = 0;
	in.entries.push_back(e);

	auto out = IndexReply{};
	EXPECT_TRUE(DecodeIndexReply(EncodeIndexReply(in), out));
	EXPECT_EQ(out.format, Format::SevenZip);
	EXPECT_EQ(out.access, Access::Sequential);
	EXPECT_EQ(out.total_bytes, 123);
	EXPECT_EQ(out.archive_length, 456);
	EXPECT_EQ(out.identity, in.identity);
	EXPECT_EQ(out.stat.size, 456);
	EXPECT_EQ(out.stat.mtime, 789);
	ASSERT_EQ(out.entries.size(), 2u);
	EXPECT_EQ(out.entries[0].ordinal, 3);
	EXPECT_EQ(out.entries[0].utf8_path, "dir/f.txt");
	EXPECT_EQ(out.entries[0].dos_path, "DIR\\F.TXT");
	EXPECT_EQ(out.entries[0].size, 9);
	EXPECT_EQ(out.entries[0].mtime, 631152000);
	EXPECT_TRUE(out.entries[0].has_crc);
	EXPECT_EQ(out.entries[0].crc32, 0x1234u);
	EXPECT_TRUE(out.entries[1].is_dir);
	EXPECT_FALSE(out.entries[1].has_crc);
}

static size_t EntryCountOffset(const std::vector<uint8_t>& bytes)
{
	auto r         = Reader(bytes);
	uint8_t f      = 0;
	uint8_t a      = 0;
	int64_t t      = 0;
	int64_t l      = 0;
	int64_t ss     = 0;
	int64_t sm     = 0;
	std::string id = {};
	EXPECT_TRUE(r.U8(f) && r.U8(a) && r.I64(t) && r.I64(l) && r.Str(id) &&
	            r.I64(ss) && r.I64(sm));
	return r.Position();
}

TEST(HelperProtocol, AnIndexReplyWithMoreEntriesThanDeclaredIsRefused)
{
	auto in                        = IndexReply{};
	in.identity                    = std::string(64, 'b');
	auto bytes                     = EncodeIndexReply(in);
	bytes[EntryCountOffset(bytes)] = 1;
	auto out                       = IndexReply{};
	EXPECT_FALSE(DecodeIndexReply(bytes, out));
}

// The helper parses hostile archives, so its replies are not trusted: a
// count the remaining bytes cannot hold is refused before any reserve.
TEST(HelperProtocol, AnEntryCountTheBytesCannotHoldIsRefused)
{
	auto in     = IndexReply{};
	in.identity = std::string(64, 'c');
	auto bytes  = EncodeIndexReply(in);
	// 1000 zero bytes parse as 29 empty entries; 500 cannot fit.
	bytes.resize(bytes.size() + 1000, 0);
	const auto at = EntryCountOffset(bytes);
	bytes[at]     = 0xf4;
	bytes[at + 1] = 0x01;
	auto out      = IndexReply{};
	EXPECT_FALSE(DecodeIndexReply(bytes, out));
	EXPECT_EQ(out.entries.capacity(), 0u);
}

TEST(HelperProtocol, AnImpliedDirectoryKeepsItsOrdinal)
{
	auto in     = IndexReply{};
	in.identity = std::string(64, 'd');
	auto e      = Entry{};
	e.ordinal   = ArchiveMount::ImpliedOrdinal;
	e.utf8_path = "many";
	e.dos_path  = "MANY";
	e.is_dir    = true;
	in.entries.push_back(e);
	auto out = IndexReply{};
	ASSERT_TRUE(DecodeIndexReply(EncodeIndexReply(in), out));
	ASSERT_EQ(out.entries.size(), 1u);
	EXPECT_EQ(out.entries[0].ordinal, ArchiveMount::ImpliedOrdinal);
}

// Anything below ImpliedOrdinal would collide with the client's own
// negative keys; a materialize request names a real header only.
TEST(HelperProtocol, OrdinalsOutsideTheirRangeAreRefused)
{
	auto in     = IndexReply{};
	in.identity = std::string(64, 'd');
	auto e      = Entry{};
	e.utf8_path = "a";
	e.dos_path  = "A";
	e.ordinal   = -2;
	in.entries.push_back(e);
	auto out = IndexReply{};
	EXPECT_FALSE(DecodeIndexReply(EncodeIndexReply(in), out));

	auto request = MaterializeRequest{};
	for (const uint32_t ordinal : {0xffffffffu, 0x80000000u}) {
		auto w = Writer{};
		w.U32(ordinal);
		w.Str("/scratch/x");
		EXPECT_FALSE(DecodeMaterializeRequest(w.Take(), request)) << ordinal;
	}
}

TEST(HelperProtocol, StatusMaterializeAndHelloRoundTrip)
{
	auto s  = StatusReply{Error::Corrupt, "bad crc"};
	auto so = StatusReply{};
	EXPECT_TRUE(DecodeStatusReply(EncodeStatusReply(s), so));
	EXPECT_EQ(so.error, Error::Corrupt);
	EXPECT_EQ(so.detail, "bad crc");

	auto bad = EncodeStatusReply(s);
	bad[0]   = 200;
	EXPECT_FALSE(DecodeStatusReply(bad, so));

	auto m  = MaterializeRequest{7, "/scratch/x/7"};
	auto mo = MaterializeRequest{};
	EXPECT_TRUE(DecodeMaterializeRequest(EncodeMaterializeRequest(m), mo));
	EXPECT_EQ(mo.ordinal, 7);
	EXPECT_EQ(mo.target, "/scratch/x/7");

	auto a  = MaterializeAllRequest{"/scratch/all"};
	auto ao = MaterializeAllRequest{};
	EXPECT_TRUE(DecodeMaterializeAllRequest(EncodeMaterializeAllRequest(a), ao));
	EXPECT_EQ(ao.directory, "/scratch/all");

	auto h  = HelloReply{Version, 3008007, "libarchive 3.8.7"};
	auto ho = HelloReply{};
	EXPECT_TRUE(DecodeHelloReply(EncodeHelloReply(h), ho));
	EXPECT_EQ(ho.version, Version);
	EXPECT_EQ(ho.libarchive_version, 3008007u);
	EXPECT_EQ(ho.libarchive_string, "libarchive 3.8.7");
}

TEST(HelperProtocol, TrailingBytesAreRefused)
{
	auto bytes = EncodeStatusReply({Error::None, ""});
	bytes.push_back(0);
	auto out = StatusReply{};
	EXPECT_FALSE(DecodeStatusReply(bytes, out));
}

namespace {

IndexReply ValidIndex()
{
	auto index        = IndexReply{};
	index.identity    = std::string(64, 'e');
	auto dir          = Entry{};
	dir.ordinal       = ArchiveMount::ImpliedOrdinal;
	dir.utf8_path     = "game";
	dir.dos_path      = "GAME";
	dir.is_dir        = true;
	auto file         = Entry{};
	file.ordinal      = 0;
	file.utf8_path    = "game/Long Name.exe";
	file.dos_path     = "GAME\\LONGNA~1.EXE";
	file.size         = 100;
	index.entries     = {dir, file};
	index.total_bytes = 100;
	return index;
}

} // namespace

// A helper that parses a hostile archive may itself be hostile: the engine
// sizes its deadlines and buffers from the index, so nothing unchecked.
TEST(HelperIndexCheck, AnIndexAsTheReaderBuildsItPasses)
{
	EXPECT_EQ(CheckIndex(ValidIndex(), ArchiveMount::Limits{}), std::nullopt);
}

TEST(HelperIndexCheck, SizesMustStayInsideTheLimits)
{
	auto limits             = ArchiveMount::Limits{};
	limits.max_member_bytes = 50;
	EXPECT_NE(CheckIndex(ValidIndex(), limits), std::nullopt);

	auto negative            = ValidIndex();
	negative.entries[1].size = -1;
	negative.total_bytes     = -1;
	EXPECT_NE(CheckIndex(negative, ArchiveMount::Limits{}), std::nullopt);

	auto over_total        = ValidIndex();
	limits                 = ArchiveMount::Limits{};
	limits.max_total_bytes = 99;
	EXPECT_NE(CheckIndex(over_total, limits), std::nullopt);

	auto wrong_sum        = ValidIndex();
	wrong_sum.total_bytes = 101;
	EXPECT_NE(CheckIndex(wrong_sum, ArchiveMount::Limits{}), std::nullopt);

	// The reader limits header ordinals; implied directories come on top.
	limits             = ArchiveMount::Limits{};
	limits.max_entries = 1;
	EXPECT_EQ(CheckIndex(ValidIndex(), limits), std::nullopt);

	auto past_limit               = ValidIndex();
	past_limit.entries[1].ordinal = 1;
	EXPECT_NE(CheckIndex(past_limit, limits), std::nullopt);
}

TEST(HelperIndexCheck, ImpliedDirectoriesAreBoundedByTheRealEntries)
{
	auto index = ValidIndex();
	for (int i = 0; i < 40; ++i) {
		auto dir      = Entry{};
		dir.ordinal   = ArchiveMount::ImpliedOrdinal;
		dir.utf8_path = "d" + std::to_string(i);
		dir.dos_path  = "D" + std::to_string(i);
		dir.is_dir    = true;
		index.entries.push_back(dir);
	}
	EXPECT_NE(CheckIndex(index, ArchiveMount::Limits{}), std::nullopt);
	index.entries.resize(2 + 38);
	EXPECT_EQ(CheckIndex(index, ArchiveMount::Limits{}), std::nullopt);
}

TEST(HelperIndexCheck, TheTreeMustHaveTheReadersShape)
{
	auto duplicate = ValidIndex();
	duplicate.entries.push_back(duplicate.entries[1]);
	duplicate.entries[2].ordinal   = 1;
	duplicate.entries[2].utf8_path = "game/other.exe";
	duplicate.total_bytes          = 200;
	EXPECT_NE(CheckIndex(duplicate, ArchiveMount::Limits{}), std::nullopt);

	auto orphan                 = ValidIndex();
	orphan.entries[1].dos_path  = "ELSE\\LONGNA~1.EXE";
	orphan.entries[1].utf8_path = "else/Long Name.exe";
	EXPECT_NE(CheckIndex(orphan, ArchiveMount::Limits{}), std::nullopt);

	auto under_file = ValidIndex();
	auto child      = Entry{};
	child.ordinal   = 1;
	child.utf8_path = "game/Long Name.exe/inner";
	child.dos_path  = "GAME\\LONGNA~1.EXE\\INNER";
	under_file.entries.push_back(child);
	EXPECT_NE(CheckIndex(under_file, ArchiveMount::Limits{}), std::nullopt);

	auto depth                 = ValidIndex();
	depth.entries[1].utf8_path = "game/sub/Long Name.exe";
	EXPECT_NE(CheckIndex(depth, ArchiveMount::Limits{}), std::nullopt);

	auto sized_dir               = ValidIndex();
	sized_dir.entries[0].ordinal = 1;
	sized_dir.entries[0].size    = 5;
	sized_dir.total_bytes        = 105;
	EXPECT_NE(CheckIndex(sized_dir, ArchiveMount::Limits{}), std::nullopt);

	auto explicit_dir               = ValidIndex();
	explicit_dir.entries[0].ordinal = 1;
	EXPECT_EQ(CheckIndex(explicit_dir, ArchiveMount::Limits{}), std::nullopt);
}

TEST(HelperIndexCheck, OrdinalsMustBeUniqueAndImpliedOnlyForDirectories)
{
	auto twice = ValidIndex();
	twice.entries.push_back(twice.entries[1]);
	twice.entries[2].utf8_path = "game/other.exe";
	twice.entries[2].dos_path  = "GAME\\OTHER.EXE";
	twice.total_bytes          = 200;
	EXPECT_NE(CheckIndex(twice, ArchiveMount::Limits{}), std::nullopt);

	auto implied_file               = ValidIndex();
	implied_file.entries[1].ordinal = ArchiveMount::ImpliedOrdinal;
	EXPECT_NE(CheckIndex(implied_file, ArchiveMount::Limits{}), std::nullopt);

	auto implied_size            = ValidIndex();
	implied_size.entries[0].size = 5;
	implied_size.total_bytes     = 105;
	EXPECT_NE(CheckIndex(implied_size, ArchiveMount::Limits{}), std::nullopt);
}

TEST(HelperIndexCheck, PathsMustKeepTheReadersShape)
{
	const auto refused_dos = {"..\\X.EXE",
	                          "GAME\\..",
	                          "game\\a.exe",
	                          "GAME\\NINECHARS.EXE",
	                          "GAME\\A.LONG",
	                          "GAME\\A B.EXE",
	                          "GAME\\A:B",
	                          "\\GAME\\A.EXE",
	                          "GAME\\\\A.EXE",
	                          "GAME\\A.B.C",
	                          ""};
	for (const auto* dos : refused_dos) {
		auto index                = ValidIndex();
		index.entries[1].dos_path = dos;
		EXPECT_NE(CheckIndex(index, ArchiveMount::Limits{}), std::nullopt)
		        << dos;
	}
	auto too_long = ValidIndex();
	too_long.entries[1].dos_path.clear();
	for (int i = 0; i < 9; ++i) {
		too_long.entries[1].dos_path += "ABCDEFGH\\";
	}
	too_long.entries[1].dos_path += "A.EXE";
	EXPECT_NE(CheckIndex(too_long, ArchiveMount::Limits{}), std::nullopt);

	const auto refused_utf8 = {"/etc/passwd", "game/../x", "", "game//x"};
	for (const auto* utf8 : refused_utf8) {
		auto index                 = ValidIndex();
		index.entries[1].utf8_path = utf8;
		EXPECT_NE(CheckIndex(index, ArchiveMount::Limits{}), std::nullopt)
		        << utf8;
	}
	auto accepted                = ValidIndex();
	accepted.entries[1].dos_path = "GAME\\$#@~_-!.{}";
	EXPECT_EQ(CheckIndex(accepted, ArchiveMount::Limits{}), std::nullopt);
}

TEST(HelperIndexCheck, AnEntryPathMustBeInTheReadersNormalForm)
{
	for (const auto* utf8 :
	     {"game\\Long Name.exe", "game./Long Name.exe", "game/Long Name.exe/"}) {
		auto index                 = ValidIndex();
		index.entries[1].utf8_path = utf8;
		EXPECT_NE(CheckIndex(index, ArchiveMount::Limits{}), std::nullopt)
		        << utf8;
	}
}

// What the reader itself produces must pass, in the shapes CheckIndex is
// strictest about.
TEST(HelperIndexCheck, TheReadersOwnIndexesPass)
{
	namespace fs = std::filesystem;
	using ArchiveFixtures::Kind;
	const auto dir = TestTempDir::MakeUnique("index_check_");
	ASSERT_FALSE(dir.empty());
	const auto deep = std::string(
	        "aaaaaaaa/bbbbbbbb/cccccccc/dddddddd/eeeeeeee/"
	        "ffffffff/gggggggg/hhhhhhh.txt");
	const auto cases = std::vector<std::pair<Kind, std::vector<ArchiveFixtures::FileEntry>>>{
	        // 7z writes directories after the files in them
	        { Kind::SevenZip,{{"sub/", "", true}, {"sub/a.txt", "a"}}                                 },
	        {Kind::ZipStored,
	         {{"a long directory name/x.txt", "x"},
                 {"a long directory name 2/y.txt", "y"}}                    },
	        {Kind::ZipStored, {{"only/", "", true}, {"dirs/", "", true}}},
	        {      Kind::Tar,	                      {{deep, "z"}}},
	};
	int n = 0;
	for (const auto& [kind, entries] : cases) {
		const auto path = dir / ("case" + std::to_string(n++));
		auto error      = std::string();
		ASSERT_TRUE(ArchiveFixtures::WriteArchive(
		        path, kind, entries, nullptr, &error))
		        << error;
		const auto limits = ArchiveMount::Limits{};
		auto opened = ArchiveMount::ArchiveReader::Open(path, limits);
		ASSERT_EQ(opened.error, Error::None) << opened.detail;
		auto index        = IndexReply{};
		index.total_bytes = opened.reader->TotalBytes();
		index.entries     = opened.reader->Entries();
		EXPECT_EQ(CheckIndex(index, limits), std::nullopt) << path;
	}
	std::error_code ec = {};
	fs::remove_all(dir, ec);
}
