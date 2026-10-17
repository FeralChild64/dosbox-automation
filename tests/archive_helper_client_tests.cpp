// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//
#include "dos/archive/archive_helper_client.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <sstream>
#include <thread>
#include <tuple>

#include "archive_fixtures.h"
#include "dos/archive/archive_activity.h"
#include "dos/archive/archive_reader.h"
#include "dos/archive/shadow_sanitizer.h"
#include "test_temp_dir.h"

using namespace std::chrono;
using namespace ArchiveMount;
using ArchiveFixtures::FileEntry;
using ArchiveFixtures::Filter;
using ArchiveFixtures::IszChunk;
using ArchiveFixtures::Kind;
using ArchiveFixtures::WriteArchive;
using ArchiveFixtures::WriteCompressed;
using ArchiveFixtures::WriteIsz;
namespace fs = std::filesystem;

// ada-e88p, 2026-09-30: a sequential format inflates the whole stream,
// so its clock runs on the uncompressed bound at 8 KiB per ms;
// per-member formats keep the on-disk formula.
TEST(ArchiveHelperClock, SequentialFormatsAreClockedByTheCapAndPerMemberByTheFile)
{
	auto options = ArchiveHelperClient::Options{};
	auto limits  = Limits{}; // 4 GiB total by default
	// A zip: 30 s plus 1 ms per 16 KiB on disk.
	EXPECT_EQ(OpenBudget(options, Access::PerMember, 1ll << 30, limits),
	          milliseconds(30000 + 65536));
	// A tar.bz2 of any size: 30 s plus the 4 GiB cap at 8 KiB per ms.
	EXPECT_EQ(OpenBudget(options, Access::Sequential, 1ll << 30, limits),
	          milliseconds(30000 + 524288));
	// An unknown head is clocked as sequential; nothing worse can happen.
	EXPECT_EQ(OpenBudget(options, std::nullopt, 10, limits),
	          milliseconds(30000 + 524288));
	// Requests: a member's own size for a per-member format, the
	// archive's declared total for a sequential one (the walk to the
	// member is the work, whatever the member's size).
	EXPECT_EQ(RequestBudget(options, Format::Zip, Access::PerMember, 2ll << 30, 3ll << 30),
	          milliseconds(30000 + 131072));
	EXPECT_EQ(RequestBudget(options, Format::Tar, Access::Sequential, 5, 2ll << 30),
	          milliseconds(30000 + 262144));
	// A raw image.bz2 or an ISZ of bzip2 chunks decodes its member whole:
	// the sequential rate, over the member's own size.
	EXPECT_EQ(RequestBudget(options, Format::Raw, Access::PerMember, 2ll << 30, 2ll << 30),
	          milliseconds(30000 + 262144));
	EXPECT_EQ(RequestBudget(options, Format::Isz, Access::PerMember, 2ll << 30, 2ll << 30),
	          milliseconds(30000 + 262144));
	options.sequential_bytes_per_ms = 0;
	EXPECT_EQ(RequestBudget(options, Format::Tar, Access::Sequential, 5, 2ll << 30),
	          milliseconds(30000));
}

class ArchiveHelperClientTest : public testing::Test {
protected:
	fs::path tmp_dir                     = {};
	Limits limits                        = {};
	ArchiveHelperClient::Options options = {};

	void SetUp() override
	{
		tmp_dir = TestTempDir::MakeUnique("archive_helper_");
		ASSERT_FALSE(tmp_dir.empty());
		options.helper_path = DOSBOX_ARCHIVE_HELPER_PATH;
		options.scratch_dir = tmp_dir / "scratch";
		options.test_hooks  = true;
		fs::create_directories(options.scratch_dir);
	}
	void TearDown() override
	{
		std::error_code ec = {};
		fs::remove_all(tmp_dir, ec);
	}
	fs::path Built(const std::string& name, const Kind kind,
	               const std::vector<FileEntry>& entries)
	{
		const auto path = tmp_dir / name;
		auto error      = std::string();
		EXPECT_TRUE(WriteArchive(path, kind, entries, nullptr, &error))
		        << error;
		return path;
	}
	std::unique_ptr<ArchiveHelperClient> OpenOk(const fs::path& path)
	{
		auto opened = ArchiveHelperClient::Open(path, limits, options);
		EXPECT_EQ(opened.error, Error::None) << opened.detail;
		return std::move(opened.client);
	}
	static std::string Read(ArchiveSource& source, const int ordinal)
	{
		std::vector<uint8_t> out = {};
		auto detail              = std::string();
		EXPECT_EQ(source.Materialize(ordinal, out, &detail), Error::None)
		        << detail;
		return std::string(out.begin(), out.end());
	}
};

TEST_F(ArchiveHelperClientTest, TheIndexMatchesTheInProcessReader)
{
	auto entries = std::vector<FileEntry>{
	        {"dir/", "", true},
	        {"dir/one.txt", "one"},
	        {"two.dat", std::string(5000, 'x')}
        };
	// Enough names that the index reply is several pipe reads long.
	for (int i = 0; i < 200; ++i) {
		entries.push_back({"many/a-rather-long-file-name-" +
		                           std::to_string(i) + ".txt",
		                   std::to_string(i)});
	}
	const auto path = Built("a.zip", Kind::ZipDeflate, entries);
	auto client     = OpenOk(path);
	ASSERT_TRUE(client);
	auto reader = ArchiveReader::Open(path, limits);
	ASSERT_EQ(reader.error, Error::None);

	EXPECT_EQ(client->GetFormat(), reader.reader->GetFormat());
	EXPECT_EQ(client->GetAccess(), reader.reader->GetAccess());
	EXPECT_EQ(client->TotalBytes(), reader.reader->TotalBytes());
	EXPECT_EQ(client->ArchiveLength(), reader.reader->ArchiveLength());
	EXPECT_EQ(client->IdentityHash(), reader.reader->IdentityHash());
	EXPECT_EQ(client->IndexedStat().size, reader.reader->IndexedStat().size);
	EXPECT_EQ(client->IndexedStat().mtime, reader.reader->IndexedStat().mtime);
	ASSERT_EQ(client->Entries().size(), reader.reader->Entries().size());
	for (size_t i = 0; i < client->Entries().size(); ++i) {
		const auto& a = client->Entries()[i];
		const auto& b = reader.reader->Entries()[i];
		EXPECT_EQ(a.ordinal, b.ordinal);
		EXPECT_EQ(a.utf8_path, b.utf8_path);
		EXPECT_EQ(a.dos_path, b.dos_path);
		EXPECT_EQ(a.is_dir, b.is_dir);
		EXPECT_EQ(a.size, b.size);
		EXPECT_EQ(a.mtime, b.mtime);
		EXPECT_EQ(a.has_crc, b.has_crc);
		EXPECT_EQ(a.crc32, b.crc32);
	}
	EXPECT_EQ(client->RespawnCount(), 0);

	// Implied directories have no header to materialize.
	auto detail = std::string();
	EXPECT_EQ(client->MaterializeToFile(ImpliedOrdinal, tmp_dir / "none", &detail),
	          Error::NotFound);
	EXPECT_EQ(client->RespawnCount(), 0);
}

TEST_F(ArchiveHelperClientTest, NamesOutsideAsciiComeThroughTheHelper)
{
	// G6: a middle dot (Giana Sisters) and a one-half sign (Spear of
	// Destiny); 7z stores UTF-16LE, which libarchive converts to the locale.
	const auto path = Built("names.7z",
	                        Kind::SevenZip,
	                        {
	                                {"LICK\xC2\xB7ME.GER", "giana"},
	                                {"3\xC2\xBD-inch.txt", "spear"},
	                                {         "plain.txt", "plain"},
        });
	auto client     = OpenOk(path);
	ASSERT_TRUE(client);
	auto names = std::vector<std::string>{};
	for (const auto& e : client->Entries()) {
		names.push_back(e.utf8_path);
	}
	std::ranges::sort(names);
	EXPECT_EQ(names,
	          (std::vector<std::string>{"3\xC2\xBD-inch.txt",
	                                    "LICK\xC2\xB7ME.GER",
	                                    "plain.txt"}));
}

TEST_F(ArchiveHelperClientTest, AnIsoComesThroughTheHelperAsTheReaderSeesIt)
{
	const auto iso  = ArchiveFixtures::IsoWithFiles({
                {"README.TXT", "read me"},
                {"GAMES/A.ZIP", std::string(100, 'z')},
        });
	const auto path = tmp_dir / "disc.iso";
	ArchiveFixtures::WriteBytes(path,
	                            std::vector<uint8_t>(iso.begin(), iso.end()));
	auto direct = ArchiveReader::Open(path, limits);
	ASSERT_EQ(direct.error, Error::None) << direct.detail;
	auto helped = OpenOk(path);
	ASSERT_TRUE(helped);
	EXPECT_EQ(helped->GetFormat(), Format::Iso9660);
	EXPECT_EQ(helped->GetAccess(), Access::PerMember);
	EXPECT_EQ(helped->IdentityHash(), direct.reader->IdentityHash());
	ASSERT_EQ(helped->Entries().size(), direct.reader->Entries().size());
	for (size_t i = 0; i < direct.reader->Entries().size(); ++i) {
		EXPECT_EQ(helped->Entries()[i].utf8_path,
		          direct.reader->Entries()[i].utf8_path);
	}
	for (const auto& e : helped->Entries()) {
		if (e.utf8_path == "README.TXT") {
			EXPECT_EQ(Read(*helped, e.ordinal), "read me");
		}
	}
}

TEST_F(ArchiveHelperClientTest, MaterializeMatchesTheInProcessReader)
{
	const auto path = Built("b.zip",
	                        Kind::ZipStored,
	                        {
	                                {"one.txt", "one"},
	                                {"two.dat", std::string(70000, 'y')}
        });
	auto client     = OpenOk(path);
	ASSERT_TRUE(client);
	auto reader = ArchiveReader::Open(path, limits);
	ASSERT_EQ(reader.error, Error::None);
	for (const auto& e : client->Entries()) {
		EXPECT_EQ(Read(*client, e.ordinal), Read(*reader.reader, e.ordinal));
	}
	EXPECT_TRUE(fs::is_empty(options.scratch_dir));
}

TEST_F(ArchiveHelperClientTest, PkzipShrunkAndImplodedMembersComeThroughTheHelper)
{
	const auto legacy = fs::path("tests/files/archives/legacy");
	for (const auto* zip : {"SHRUNK.ZIP", "IMPLODED.ZIP"}) {
		auto client = OpenOk(legacy / zip);
		ASSERT_NE(client, nullptr) << zip;
		for (const auto& e : client->Entries()) {
			std::ifstream in(legacy / "inputs" / e.utf8_path,
			                 std::ios::binary);
			const std::string expected{std::istreambuf_iterator<char>(in),
			                           std::istreambuf_iterator<char>()};
			EXPECT_EQ(Read(*client, e.ordinal), expected)
			        << zip << " " << e.utf8_path;
		}
	}
}

TEST_F(ArchiveHelperClientTest, MaterializeToFileWritesTheWholeMember)
{
	const auto path = Built("c.zip",
	                        Kind::ZipDeflate,
	                        {
	                                {"f.bin", std::string(12345, 'z')}
        });
	auto client     = OpenOk(path);
	ASSERT_TRUE(client);
	const auto target = tmp_dir / "out.bin";
	auto detail       = std::string();
	EXPECT_EQ(client->MaterializeToFile(0, target, &detail), Error::None)
	        << detail;
	EXPECT_EQ(fs::file_size(target), 12345u);
}

// Plan 4b: the helper serves the single-image containers with the same
// reader, so the index and the bytes match the in-process ones.
namespace {
std::string slurp_file(const fs::path& path)
{
	std::ifstream in(path, std::ios::binary);
	std::stringstream out;
	out << in.rdbuf();
	return out.str();
}
} // namespace

TEST_F(ArchiveHelperClientTest, AWindowThroughTheHelperMatchesTheInnerZipAsAFile)
{
	const auto image = std::string(4096, 'd');
	const auto inner = Built("inner.zip",
	                         Kind::ZipStored,
	                         {
	                                 { "disk1.img", image},
                                         {"readme.txt",  "hi"}
        });
	const auto outer = Built("outer.zip",
	                         Kind::ZipStored,
	                         {
	                                 {"inner.zip", slurp_file(inner)}
        });
	auto host        = OpenOk(outer);
	ASSERT_TRUE(host);
	const Entry* member = nullptr;
	for (const auto& e : host->Entries()) {
		if (e.utf8_path == "inner.zip") {
			member = &e;
		}
	}
	ASSERT_NE(member, nullptr);
	ASSERT_GE(member->stored_offset, 0);

	auto windowed = ArchiveHelperClient::OpenWindow(*host,
	                                                member->stored_offset,
	                                                member->size,
	                                                outer / "inner.zip",
	                                                limits,
	                                                options);
	ASSERT_EQ(windowed.error, Error::None) << windowed.detail;
	auto as_file = OpenOk(inner);
	ASSERT_TRUE(as_file);
	EXPECT_EQ(windowed.client->IdentityHash(), as_file->IdentityHash());
	EXPECT_EQ(windowed.client->ArchiveLength(), member->size);
	EXPECT_EQ(windowed.client->IndexedStat().size, host->IndexedStat().size);
	ASSERT_EQ(windowed.client->Entries().size(), 2u);
	EXPECT_EQ(Read(*windowed.client, 0), image);
	EXPECT_EQ(windowed.client->Window().offset, member->stored_offset);
	EXPECT_EQ(windowed.client->Window().length, member->size);
	EXPECT_EQ(host->Window().length, 0);
}

TEST_F(ArchiveHelperClientTest, AWindowSurvivesTheHelpersRespawn)
{
	const auto inner = Built("inner.zip",
	                         Kind::ZipStored,
	                         {
	                                 {"a.txt", "aaaa"}
        });
	const auto outer = Built("outer.zip",
	                         Kind::ZipStored,
	                         {
	                                 {"inner.zip", slurp_file(inner)}
        });
	auto host        = OpenOk(outer);
	ASSERT_TRUE(host);
	const auto& m = host->Entries().front();
	auto windowed = ArchiveHelperClient::OpenWindow(
	        *host, m.stored_offset, m.size, outer / "inner.zip", limits, options);
	ASSERT_EQ(windowed.error, Error::None) << windowed.detail;
	ASSERT_TRUE(windowed.client->ArmTestCrash(0, /*persistent=*/false));
	EXPECT_EQ(Read(*windowed.client, 0), "aaaa");
	EXPECT_EQ(windowed.client->RespawnCount(), 1);
}

TEST_F(ArchiveHelperClientTest, AWindowPastTheHostFileIsRefused)
{
	const auto outer = Built("outer.zip",
	                         Kind::ZipStored,
	                         {
	                                 {"a.txt", "aaaa"}
        });
	auto host        = OpenOk(outer);
	ASSERT_TRUE(host);
	auto windowed = ArchiveHelperClient::OpenWindow(
	        *host, host->ArchiveLength() - 4, 100, outer / "x", limits, options);
	EXPECT_EQ(windowed.error, Error::OpenFailed);
	EXPECT_NE(windowed.detail.find("window"), std::string::npos)
	        << windowed.detail;
}

TEST_F(ArchiveHelperClientTest, AWindowedZipOutlivesTheHostFilesReplacement)
{
	// D16: the window lives on the kept descriptor. Renaming another
	// file over the host changes nothing the client reads.
	const auto inner = Built("inner.zip",
	                         Kind::ZipStored,
	                         {
	                                 {"a.txt", "aaaa"}
        });
	const auto outer = Built("outer.zip",
	                         Kind::ZipStored,
	                         {
	                                 {"inner.zip", slurp_file(inner)}
        });
	auto host        = OpenOk(outer);
	ASSERT_TRUE(host);
	const auto& m = host->Entries().front();
	auto windowed = ArchiveHelperClient::OpenWindow(
	        *host, m.stored_offset, m.size, outer / "inner.zip", limits, options);
	ASSERT_EQ(windowed.error, Error::None) << windowed.detail;
	const auto other   = Built("other.zip",
                                 Kind::ZipStored,
	                           {
                                         {"z.txt", std::string(9000, 'z')}
        });
	std::error_code ec = {};
	fs::rename(other, outer, ec);
	if (ec) {
		GTEST_SKIP() << "rename over an open file refused here: "
		             << ec.message();
	}
	EXPECT_EQ(Read(*windowed.client, 0), "aaaa");
}

TEST_F(ArchiveHelperClientTest, ContainersComeThroughTheHelper)
{
	std::string floppy(368640, '\0');
	for (size_t i = 0; i < floppy.size(); ++i) {
		floppy[i] = static_cast<char>((i / 512 + i) & 0xff);
	}
	const auto iso = ArchiveFixtures::MinimalIso() + std::string(65536, 'i');
	ASSERT_TRUE(WriteCompressed(tmp_dir / "disk1.img.xz", Filter::Xz, floppy));
	ASSERT_TRUE(WriteIsz(tmp_dir / "game.isz", iso, IszChunk::Bzip2));
	for (const auto& [name, bytes, format] : {
	             std::tuple{std::string("disk1.img.xz"), floppy, Format::Raw},
	             std::tuple{    std::string("game.isz"),    iso, Format::Isz}
        }) {
		auto client = OpenOk(tmp_dir / name);
		ASSERT_TRUE(client) << name;
		EXPECT_EQ(client->GetFormat(), format) << name;
		ASSERT_EQ(client->Entries().size(), 1u) << name;
		EXPECT_TRUE(Read(*client, 0) == bytes) << name;
		const auto target = tmp_dir / ("out-" + name);
		auto detail       = std::string();
		ASSERT_EQ(client->MaterializeToFile(0, target, &detail), Error::None)
		        << detail;
		std::ifstream in(target, std::ios::binary);
		const auto written = std::string(std::istreambuf_iterator<char>(in),
		                                 std::istreambuf_iterator<char>());
		EXPECT_TRUE(written == bytes) << name;
	}
}

TEST_F(ArchiveHelperClientTest, MaterializeAllServesASequentialFormat)
{
	const auto path = Built("d.7z",
	                        Kind::SevenZip,
	                        {
	                                {"a.txt", "aaa"},
                                        {"b.txt",  "bb"}
        });
	auto client     = OpenOk(path);
	ASSERT_TRUE(client);
	EXPECT_EQ(client->GetAccess(), Access::Sequential);
	const auto dir = tmp_dir / "all";
	auto detail    = std::string();
	EXPECT_EQ(client->MaterializeAll(dir, &detail), Error::None) << detail;
	int files = 0;
	for (const auto& entry : fs::directory_iterator(dir)) {
		files += entry.is_regular_file() ? 1 : 0;
	}
	EXPECT_EQ(files, 2);
}

TEST_F(ArchiveHelperClientTest, RefusalsComeBackWithTheirReason)
{
	const auto path = tmp_dir / "junk.zip";
	ArchiveFixtures::WriteBytes(path, std::vector<uint8_t>(100, 0x55));
	auto opened = ArchiveHelperClient::Open(path, limits, options);
	EXPECT_FALSE(opened.client);
	EXPECT_EQ(opened.error, Error::NotAnArchive);
	EXPECT_FALSE(opened.detail.empty());
	EXPECT_EQ(ArchiveHelperClient::LiveHelpers(), 0);
}

TEST_F(ArchiveHelperClientTest, AMissingHelperIsHelperFailed)
{
	const auto path     = Built("e.zip",
                                Kind::ZipStored,
	                            {
                                        {"f.txt", "f"}
        });
	options.helper_path = fs::path(DOSBOX_ARCHIVE_HELPER_PATH ".missing");
	auto opened         = ArchiveHelperClient::Open(path, limits, options);
	EXPECT_FALSE(opened.client);
	EXPECT_EQ(opened.error, Error::HelperFailed);
}

TEST_F(ArchiveHelperClientTest, ACrashIsRetriedOnceInAFreshHelper)
{
	const auto path = Built("f.zip",
	                        Kind::ZipStored,
	                        {
	                                {"a.txt", "a"},
                                        {"b.txt", "b"}
        });
	auto client     = OpenOk(path);
	ASSERT_TRUE(client);
	ASSERT_TRUE(client->ArmTestCrash(1, /*persistent=*/false));
	EXPECT_EQ(Read(*client, 1), "b");
	EXPECT_EQ(client->RespawnCount(), 1);
	EXPECT_EQ(Read(*client, 0), "a");
	EXPECT_EQ(client->RespawnCount(), 1);
}

TEST_F(ArchiveHelperClientTest, ASecondCrashOnTheSameMemberRefusesItForGood)
{
	const auto path = Built("g.zip",
	                        Kind::ZipStored,
	                        {
	                                {"a.txt", "a"},
                                        {"b.txt", "b"}
        });
	auto client     = OpenOk(path);
	ASSERT_TRUE(client);
	ASSERT_TRUE(client->ArmTestCrash(1, /*persistent=*/true));
	std::vector<uint8_t> out = {};
	auto detail              = std::string();
	EXPECT_EQ(client->Materialize(1, out, &detail), Error::HelperFailed);
	EXPECT_NE(detail.find("twice"), std::string::npos) << detail;
	// One respawn between the two crashes; the next one waits for a request.
	EXPECT_EQ(client->RespawnCount(), 1);
	detail.clear();
	EXPECT_EQ(client->Materialize(1, out, &detail), Error::HelperFailed);
	EXPECT_NE(detail.find("twice"), std::string::npos) << detail;
	EXPECT_EQ(client->RespawnCount(), 1);
	EXPECT_EQ(Read(*client, 0), "a");
	EXPECT_EQ(client->RespawnCount(), 2);
}

TEST_F(ArchiveHelperClientTest, AHangIsKilledOnTheClock)
{
	const auto path      = Built("h.zip",
                                Kind::ZipStored,
	                             {
                                        {"a.txt", "a"},
                                        {"b.txt", "b"}
        });
	options.base_timeout = 500ms;
	auto client          = OpenOk(path);
	ASSERT_TRUE(client);
	ASSERT_TRUE(client->ArmTestHang(1, /*persistent=*/false));
	std::vector<uint8_t> out = {};
	auto detail              = std::string();
	const auto t0            = steady_clock::now();
	EXPECT_EQ(client->Materialize(1, out, &detail), Error::Timeout);
	EXPECT_LT(steady_clock::now() - t0, 5000ms);
	// Refused for the mount's life with the same reason, without a helper.
	detail.clear();
	EXPECT_EQ(client->Materialize(1, out, &detail), Error::Timeout);
	EXPECT_NE(detail.find("hung"), std::string::npos) << detail;
	EXPECT_EQ(client->RespawnCount(), 0);
	EXPECT_EQ(Read(*client, 0), "a");
	EXPECT_EQ(client->RespawnCount(), 1);
}

TEST_F(ArchiveHelperClientTest, AChangedArchiveIsRefusedBeforeTheHelperIsAsked)
{
	const auto path = Built("i.zip",
	                        Kind::ZipStored,
	                        {
	                                {"a.txt", "a"}
        });
	auto client     = OpenOk(path);
	ASSERT_TRUE(client);
	{
		std::ofstream grow(path, std::ios::binary | std::ios::app);
		grow << "trailing";
	}
	std::vector<uint8_t> out = {};
	auto detail              = std::string();
	EXPECT_EQ(client->Materialize(0, out, &detail), Error::Changed);
	EXPECT_EQ(client->RespawnCount(), 0);
}

// The three D16 answers with the kept descriptor: a rename, a
// rename-over and a delete leave the session on the inode it started
// with, through a respawn as well. The crash hook forces the respawn.
TEST_F(ArchiveHelperClientTest, ARenamedArchiveKeepsServingThroughARespawn)
{
	const auto path = Built("r.zip",
	                        Kind::ZipStored,
	                        {
	                                {"a.txt", "a"}
        });
	auto client     = OpenOk(path);
	ASSERT_TRUE(client);
	fs::rename(path, tmp_dir / "moved.zip");
	EXPECT_EQ(Read(*client, 0), "a");
	ASSERT_TRUE(client->ArmTestCrash(0, /*persistent=*/false));
	EXPECT_EQ(Read(*client, 0), "a");
	EXPECT_EQ(client->RespawnCount(), 1);
}

TEST_F(ArchiveHelperClientTest, AnArchiveReplacedByRenameOverKeepsTheOldBytes)
{
	const auto path = Built("s.zip",
	                        Kind::ZipStored,
	                        {
	                                {"a.txt", "old"}
        });
	auto client     = OpenOk(path);
	ASSERT_TRUE(client);
	const auto replacement = Built("s-new.zip",
	                               Kind::ZipStored,
	                               {
	                                       {"a.txt", "brand new"}
        });
	// Windows' legacy rename (std::filesystem's) refuses to replace a
	// file another process holds open, share-delete or not; a POSIX
	// semantics rename would. Either way the mount keeps the old bytes.
	std::error_code rename_ec = {};
	fs::rename(replacement, path, rename_ec);
#if defined(WIN32)
	if (rename_ec) {
		EXPECT_TRUE(fs::exists(replacement)) << rename_ec.message();
	}
#else
	EXPECT_FALSE(rename_ec) << rename_ec.message();
#endif
	EXPECT_EQ(Read(*client, 0), "old");
	ASSERT_TRUE(client->ArmTestCrash(0, /*persistent=*/false));
	EXPECT_EQ(Read(*client, 0), "old");
	EXPECT_EQ(client->RespawnCount(), 1);
	// Whatever sits at the path now is not consulted: nobody stats the
	// path any more, so a replaced file is not Changed.
	std::vector<uint8_t> out = {};
	auto detail              = std::string();
	EXPECT_EQ(client->Materialize(0, out, &detail), Error::None) << detail;
}

TEST_F(ArchiveHelperClientTest, ADeletedArchiveKeepsServingThroughARespawn)
{
	const auto path = Built("t.zip",
	                        Kind::ZipStored,
	                        {
	                                {"a.txt", "a"}
        });
	auto client     = OpenOk(path);
	ASSERT_TRUE(client);
	ASSERT_TRUE(fs::remove(path));
	EXPECT_EQ(Read(*client, 0), "a");
	ASSERT_TRUE(client->ArmTestCrash(0, /*persistent=*/false));
	EXPECT_EQ(Read(*client, 0), "a");
	EXPECT_EQ(client->RespawnCount(), 1);
}

TEST_F(ArchiveHelperClientTest, TheRequestCapRespawnsTransparently)
{
	const auto path = Built("j.zip",
	                        Kind::ZipStored,
	                        {
	                                {"a.txt", "a"}
        });
	auto client     = OpenOk(path);
	ASSERT_TRUE(client);
	auto detail = std::string();
	for (int i = 0; i < 300; ++i) {
		const auto target = tmp_dir / ("cap-" + std::to_string(i));
		ASSERT_EQ(client->MaterializeToFile(0, target, &detail), Error::None)
		        << "request " << i << ": " << detail;
	}
	EXPECT_EQ(client->RespawnCount(), 1);
}

TEST_F(ArchiveHelperClientTest, CloseQuitsTheHelperWithinTwoSeconds)
{
	const auto path = Built("k.zip",
	                        Kind::ZipStored,
	                        {
	                                {"a.txt", "a"}
        });
	auto client     = OpenOk(path);
	ASSERT_TRUE(client);
	EXPECT_EQ(ArchiveHelperClient::LiveHelpers(), 1);
	EXPECT_TRUE(client->Close());
	EXPECT_EQ(ArchiveHelperClient::LiveHelpers(), 0);
	std::vector<uint8_t> out = {};
	auto detail              = std::string();
	EXPECT_EQ(client->Materialize(0, out, &detail), Error::HelperFailed);
	client.reset();
	EXPECT_EQ(ArchiveHelperClient::LiveHelpers(), 0);
}

TEST_F(ArchiveHelperClientTest, TheMemoryCapKillsABigAllocation)
{
#if defined(__APPLE__)
	GTEST_SKIP() << "macOS refuses the RLIMIT_AS cap; the helper runs uncapped (P10)";
#endif
	if (ArchiveHelper::ShadowSanitizer) {
		GTEST_SKIP() << "a sanitizer build of the helper runs without the cap";
	}
	const auto path         = Built("l.zip",
                                Kind::ZipStored,
	                                {
                                        {"a.txt", "a"}
        });
	options.memory_limit_mb = 64;
	auto client             = OpenOk(path);
	ASSERT_TRUE(client);
	// malloc refuses (TooLarge) or the touch kills the helper
	// (HelperFailed); both prove the cap holds.
	const auto error = client->TestAllocate(256);
	EXPECT_TRUE(error == Error::TooLarge || error == Error::HelperFailed)
	        << ErrorText(error);
	EXPECT_EQ(Read(*client, 0), "a");
	// A test allocation never refuses anything for the mount.
	std::vector<uint8_t> out = {};
	auto detail              = std::string();
	EXPECT_EQ(client->MaterializeAll(tmp_dir / "all", &detail), Error::None)
	        << detail;
}

#if !defined(WIN32)
// The path's permissions cannot fail a reopen any more (the descriptor is
// kept), so the inode itself is rewritten with junk at the same size and
// mtime: the fstat check passes and the respawned helper's own open fails.
TEST_F(ArchiveHelperClientTest, AFailedReopenDoesNotLeaveAHelperWithoutAnArchive)
{
	const auto path = Built("m.zip",
	                        Kind::ZipStored,
	                        {
	                                {"a.txt", "a"}
        });
	auto client     = OpenOk(path);
	ASSERT_TRUE(client);
	ASSERT_TRUE(client->ArmTestCrash(0, /*persistent=*/false));
	auto original = std::vector<char>{};
	{
		auto in  = std::ifstream(path, std::ios::binary);
		original = std::vector<char>(std::istreambuf_iterator<char>(in),
		                             std::istreambuf_iterator<char>());
	}
	const auto stamp   = fs::last_write_time(path);
	const auto rewrite = [&](const std::vector<char>& bytes) {
		auto out = std::ofstream(path, std::ios::binary | std::ios::trunc);
		out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
		out.close();
		fs::last_write_time(path, stamp);
	};
	rewrite(std::vector<char>(original.size(), 'j'));
	std::vector<uint8_t> out = {};
	auto detail              = std::string();
	EXPECT_NE(client->Materialize(0, out, &detail), Error::None);
	EXPECT_EQ(ArchiveHelperClient::LiveHelpers(), 0);
	rewrite(original);
	EXPECT_EQ(Read(*client, 0), "a");
}
#endif

// A helper killed mid-write leaves its temp file; the reader names it
// <target>.<random>.part.
TEST_F(ArchiveHelperClientTest, AKilledHelpersPartFileIsRemoved)
{
	const auto path      = Built("n.zip",
                                Kind::ZipStored,
	                             {
                                        {"a.txt", "a"}
        });
	options.base_timeout = 500ms;
	auto client          = OpenOk(path);
	ASSERT_TRUE(client);
	ASSERT_TRUE(client->ArmTestHang(0, /*persistent=*/false));
	const auto target = tmp_dir / "out.bin";
	const auto leftover = tmp_dir / ("out.bin." + std::string(32, 'a') + ".part");
	const auto keeps = {tmp_dir / "out.bin.keep",
	                    tmp_dir / "out.bin.3f9a2c.part",
	                    tmp_dir / "out.bin.exe.part"};
	ArchiveFixtures::WriteBytes(leftover, std::vector<uint8_t>(10, 1));
	for (const auto& keep : keeps) {
		ArchiveFixtures::WriteBytes(keep, std::vector<uint8_t>(10, 1));
	}
	auto detail = std::string();
	EXPECT_EQ(client->MaterializeToFile(0, target, &detail), Error::Timeout);
	EXPECT_FALSE(fs::exists(leftover));
	for (const auto& keep : keeps) {
		EXPECT_TRUE(fs::exists(keep)) << keep;
	}
}

TEST_F(ArchiveHelperClientTest, AKilledOnePassLeavesNoPartFiles)
{
	const auto path      = Built("o.7z",
                                Kind::SevenZip,
	                             {
                                        {"a.txt", "aaa"}
        });
	options.base_timeout = 500ms;
	auto client          = OpenOk(path);
	ASSERT_TRUE(client);
	ASSERT_TRUE(client->ArmTestHang(-1, /*persistent=*/false));
	const auto dir      = tmp_dir / "all";
	const auto leftover = dir / ("0." + std::string(32, 'b') + ".part");
	const auto keep     = dir / "0.keep";
	fs::create_directories(dir);
	ArchiveFixtures::WriteBytes(leftover, std::vector<uint8_t>(10, 1));
	ArchiveFixtures::WriteBytes(keep, std::vector<uint8_t>(10, 1));
	auto detail = std::string();
	EXPECT_EQ(client->MaterializeAll(dir, &detail), Error::Timeout);
	EXPECT_FALSE(fs::exists(leftover));
	EXPECT_TRUE(fs::exists(keep));
}

// Plan 5 item 5.7: the webserver thread sees the request while the
// emulation thread waits on the helper.
TEST_F(ArchiveHelperClientTest, TheActivityShowsAMemberWhileTheHelperWorks)
{
	const std::vector<FileEntry> entries = {
	        {"a.txt", "a"},
	        {"big.bin", std::string(9000, 'b')},
	};
	const auto path      = Built("doom.zip", Kind::ZipStored, entries);
	options.base_timeout = 500ms;
	auto client          = OpenOk(path);
	ASSERT_TRUE(client);
	EXPECT_FALSE(Activity::Current());
	ASSERT_TRUE(client->ArmTestHang(1, /*persistent=*/false));
	const auto target = tmp_dir / "big.bin";
	std::thread worker([&] {
		auto detail = std::string();
		EXPECT_EQ(client->MaterializeToFile(1, target, &detail),
		          Error::Timeout);
	});
	std::optional<Activity::Status> seen = {};
	const auto deadline                  = steady_clock::now() + 3s;
	while (!seen && steady_clock::now() < deadline) {
		seen = Activity::CurrentStatus();
		std::this_thread::sleep_for(1ms);
	}
	worker.join();
	ASSERT_TRUE(seen);
	EXPECT_EQ(seen->activity, "materializing");
	EXPECT_EQ(seen->archive, "doom");
	EXPECT_EQ(seen->bytes_total, 9000);
	EXPECT_FALSE(Activity::Current());
}

std::vector<Activity::Snapshot> started_guards = {};
void RecordGuard(const Activity::Snapshot& snapshot)
{
	started_guards.push_back(snapshot);
}

// An early ASSERT return must not leave the hook set for the next tests.
struct ObserverScope {
	ObserverScope()
	{
		started_guards.clear();
		Activity::observer = &RecordGuard;
	}
	~ObserverScope()
	{
		Activity::observer = nullptr;
	}
	ObserverScope(const ObserverScope&)            = delete;
	ObserverScope& operator=(const ObserverScope&) = delete;
};

// The index takes under a millisecond, too short to poll for.
TEST_F(ArchiveHelperClientTest, TheIdentificationAndEachRequestSetTheActivity)
{
	const std::vector<FileEntry> entries = {
	        {"a.txt", "a"},
	        {"dir/", "", true},
	        {"dir/b.txt", "bb"},
	};
	const auto path = Built("keen.tar", Kind::Tar, entries);
	auto observing  = std::make_unique<ObserverScope>();
	auto client     = OpenOk(path);
	ASSERT_TRUE(client);
	EXPECT_EQ(Read(*client, 0), "a");
	auto detail = std::string();
	EXPECT_EQ(client->MaterializeAll(tmp_dir / "all", &detail), Error::None)
	        << detail;
	observing.reset();
	ASSERT_EQ(started_guards.size(), 3u);
	EXPECT_EQ(started_guards[0].kind, Activity::Kind::Identifying);
	EXPECT_EQ(started_guards[0].archive, "keen");
	EXPECT_EQ(started_guards[0].bytes_total,
	          static_cast<int64_t>(fs::file_size(path)));
	EXPECT_EQ(started_guards[1].kind, Activity::Kind::Materializing);
	EXPECT_EQ(started_guards[1].bytes_total, 1);
	EXPECT_FALSE(started_guards[1].target_is_folder);
	EXPECT_EQ(started_guards[1].target.parent_path(), options.scratch_dir);
	EXPECT_EQ(started_guards[2].kind, Activity::Kind::Materializing);
	EXPECT_TRUE(started_guards[2].target_is_folder);
	EXPECT_EQ(started_guards[2].target, tmp_dir / "all");
	EXPECT_EQ(started_guards[2].bytes_total, 3);
	EXPECT_FALSE(Activity::Current());
}
