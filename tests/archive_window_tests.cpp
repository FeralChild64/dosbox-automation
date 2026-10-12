// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//
#include "dos/archive/archive_reader.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "archive_fixtures.h"
#include "dos/archive/descriptor.h"
#include "test_temp_dir.h"

using namespace ArchiveMount;
using ArchiveFixtures::FileEntry;
using ArchiveFixtures::Kind;
using ArchiveFixtures::StoredEntry;
using ArchiveFixtures::WriteArchive;
namespace fs = std::filesystem;

namespace {

std::string slurp(const fs::path& path)
{
	std::ifstream in(path, std::ios::binary);
	std::stringstream out;
	out << in.rdbuf();
	return out.str();
}

std::string read_member(ArchiveSource& source, const int ordinal, Error* error = nullptr)
{
	std::vector<uint8_t> out = {};
	auto detail              = std::string();
	const auto result        = source.Materialize(ordinal, out, &detail);
	if (error) {
		*error = result;
	} else {
		EXPECT_EQ(result, Error::None) << detail;
	}
	return std::string(out.begin(), out.end());
}

const Entry* by_path(const ArchiveSource& source, const std::string& path)
{
	for (const auto& e : source.Entries()) {
		if (e.utf8_path == path) {
			return &e;
		}
	}
	return nullptr;
}

} // namespace

class ArchiveWindowTest : public testing::Test {
protected:
	fs::path tmp_dir = {};
	Limits limits    = {};
	int host_fd      = -1;

	void SetUp() override
	{
		tmp_dir = TestTempDir::MakeUnique("archive_window_");
		ASSERT_FALSE(tmp_dir.empty());
	}
	void TearDown() override
	{
		CloseDescriptor(host_fd);
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
	// The inner zip's range inside the outer, from the outer's own index.
	ByteWindow WindowOf(const fs::path& outer, const std::string& member)
	{
		auto opened = ArchiveReader::Open(outer, limits);
		EXPECT_EQ(opened.error, Error::None) << opened.detail;
		ByteWindow window = {};
		if (!opened.reader) {
			return window;
		}
		const auto* entry = by_path(*opened.reader, member);
		EXPECT_NE(entry, nullptr) << member;
		if (entry) {
			window.offset = entry->stored_offset;
			window.length = entry->size;
		}
		return window;
	}
	OpenResult OpenWindow(const fs::path& outer, const ByteWindow& window)
	{
		CloseDescriptor(host_fd);
		host_fd = OpenReadOnlyShared(outer);
		EXPECT_GE(host_fd, 0);
		return ArchiveReader::Open(host_fd, outer / "window", limits, window);
	}
};

TEST_F(ArchiveWindowTest, AStoredMemberCarriesItsDataOffset)
{
	const auto image = std::string(4096, 'd');
	const auto inner = Built("inner.zip",
	                         Kind::ZipStored,
	                         {
	                                 {"disk1.img", image}
        });
	const auto outer = Built("outer.zip",
	                         Kind::ZipStored,
	                         {
	                                 {"readme.txt",      "hello"},
	                                 { "inner.zip", slurp(inner)}
        });
	auto opened      = ArchiveReader::Open(outer, limits);
	ASSERT_EQ(opened.error, Error::None) << opened.detail;
	const auto* member = by_path(*opened.reader, "inner.zip");
	ASSERT_NE(member, nullptr);
	ASSERT_GE(member->stored_offset, 0);
	// The range holds the inner zip byte for byte.
	const auto bytes = slurp(outer);
	ASSERT_LE(static_cast<size_t>(member->stored_offset + member->size),
	          bytes.size());
	EXPECT_EQ(bytes.substr(static_cast<size_t>(member->stored_offset),
	                       static_cast<size_t>(member->size)),
	          slurp(inner));
}

TEST_F(ArchiveWindowTest, ADeflatedMemberAndADirectoryCarryNoRange)
{
	const auto outer = Built("outer.zip",
	                         Kind::ZipDeflate,
	                         {
	                                 {       "dir/",               "", true},
	                                 {"dir/big.txt", std::string(9000, 'x')}
        });
	auto opened      = ArchiveReader::Open(outer, limits);
	ASSERT_EQ(opened.error, Error::None) << opened.detail;
	for (const auto& e : opened.reader->Entries()) {
		EXPECT_EQ(e.stored_offset, -1) << e.utf8_path;
	}
}

TEST_F(ArchiveWindowTest, ADataDescriptorZipStillGetsARange)
{
	// libarchive's writer defers every size to a data descriptor (bit 3,
	// zero local sizes); the directory's sizes decide, the descriptor
	// sits after the data and outside the range.
	const auto inner = Built("inner.zip",
	                         Kind::ZipStored,
	                         {
	                                 {"a.txt", "aaaa"}
        });
	const auto outer = Built("outer.zip",
	                         Kind::ZipStored,
	                         {
	                                 {"inner.zip", slurp(inner)}
        });
	auto opened      = ArchiveReader::Open(outer, limits);
	ASSERT_EQ(opened.error, Error::None) << opened.detail;
	const auto* member = by_path(*opened.reader, "inner.zip");
	ASSERT_NE(member, nullptr);
	ASSERT_GE(member->stored_offset, 0);
	const auto bytes = slurp(outer);
	EXPECT_EQ(bytes.substr(static_cast<size_t>(member->stored_offset),
	                       static_cast<size_t>(member->size)),
	          slurp(inner));
}

TEST_F(ArchiveWindowTest, AStoredMemberThatIsNotAZipCarriesNoRange)
{
	// A 7z stored in a zip is readable in place by nobody here (N6), and
	// a disk image under an archive name is not an archive: the range
	// needs a zip signature at the member's first bytes.
	const auto seven = Built("inner.7z",
	                         Kind::SevenZip,
	                         {
	                                 {"a.txt", "aaaa"}
        });
	const auto outer = Built("outer.zip",
	                         Kind::ZipStored,
	                         {
	                                 {"inner.7z", slurp(seven)},
	                                 {"fake.zip", std::string(4096, 'd')}
        });
	auto opened      = ArchiveReader::Open(outer, limits);
	ASSERT_EQ(opened.error, Error::None) << opened.detail;
	for (const auto& e : opened.reader->Entries()) {
		EXPECT_EQ(e.stored_offset, -1) << e.utf8_path;
	}
}

TEST_F(ArchiveWindowTest, AStoredMemberWithoutAnArchiveNameCarriesNoRange)
{
	// The chain only ever opens archive-named members as levels, so the
	// head is read for those alone; a zip under an image name gets none.
	const auto inner = Built("inner.zip",
	                         Kind::ZipStored,
	                         {
	                                 {"a.txt", "aaaa"}
        });
	const auto outer = Built("outer.zip",
	                         Kind::ZipStored,
	                         {
	                                 {"disk1.img", slurp(inner)},
	                                 { "real.zip", slurp(inner)}
        });
	auto opened      = ArchiveReader::Open(outer, limits);
	ASSERT_EQ(opened.error, Error::None) << opened.detail;
	const auto* image = by_path(*opened.reader, "disk1.img");
	const auto* real  = by_path(*opened.reader, "real.zip");
	ASSERT_NE(image, nullptr);
	ASSERT_NE(real, nullptr);
	EXPECT_EQ(image->stored_offset, -1);
	EXPECT_GE(real->stored_offset, 0);
}

TEST_F(ArchiveWindowTest, AWindowIndexesTheInnerZipInPlace)
{
	const auto image  = std::string(4096, 'd');
	const auto inner  = Built("inner.zip",
                                 Kind::ZipStored,
	                          {
                                         { "disk1.img", image},
                                         {"readme.txt",  "hi"}
        });
	const auto outer  = Built("outer.zip",
                                 Kind::ZipStored,
	                          {
                                         {"inner.zip", slurp(inner)}
        });
	const auto window = WindowOf(outer, "inner.zip");
	auto opened       = OpenWindow(outer, window);
	ASSERT_EQ(opened.error, Error::None) << opened.detail;
	auto& reader = *opened.reader;
	EXPECT_EQ(reader.GetFormat(), Format::Zip);
	EXPECT_EQ(reader.GetAccess(), Access::PerMember);
	ASSERT_EQ(reader.Entries().size(), 2u);
	EXPECT_EQ(reader.ArchiveLength(), window.length);
	// 2.3: the stat pair is the host file's.
	auto host_stat = StatPair{};
	ASSERT_TRUE(StatDescriptor(host_fd, host_stat));
	EXPECT_EQ(reader.IndexedStat().size, host_stat.size);
	EXPECT_EQ(reader.IndexedStat().mtime, host_stat.mtime);
	const auto* disk = by_path(reader, "disk1.img");
	ASSERT_NE(disk, nullptr);
	EXPECT_EQ(read_member(reader, disk->ordinal), image);
}

TEST_F(ArchiveWindowTest, AWindowedZipHasTheIdentityOfTheSameZipAsAFile)
{
	const auto inner = Built("inner.zip",
	                         Kind::ZipStored,
	                         {
	                                 {"disk1.img", std::string(4096, 'd')}
        });
	const auto outer = Built("outer.zip",
	                         Kind::ZipStored,
	                         {
	                                 {"inner.zip", slurp(inner)}
        });
	auto as_file     = ArchiveReader::Open(inner, limits);
	ASSERT_EQ(as_file.error, Error::None) << as_file.detail;
	auto windowed = OpenWindow(outer, WindowOf(outer, "inner.zip"));
	ASSERT_EQ(windowed.error, Error::None) << windowed.detail;
	EXPECT_EQ(windowed.reader->IdentityHash(), as_file.reader->IdentityHash());
}

TEST_F(ArchiveWindowTest, AWindowOneByteOffAtEitherEndIsRefused)
{
	// Early or late, the window's first byte is not the zip signature;
	// short, the end record is cut and the zip head has no directory.
	const auto inner = Built("inner.zip",
	                         Kind::ZipStored,
	                         {
	                                 {"a.txt", "aaaa"}
        });
	const auto outer = Built("outer.zip",
	                         Kind::ZipStored,
	                         {
	                                 {"inner.zip", slurp(inner)}
        });
	const auto right = WindowOf(outer, "inner.zip");
	auto early       = right;
	early.offset -= 1;
	EXPECT_EQ(OpenWindow(outer, early).error, Error::NotAnArchive);
	auto late = right;
	late.offset += 1;
	late.length -= 1;
	EXPECT_EQ(OpenWindow(outer, late).error, Error::NotAnArchive);
	auto shorter = right;
	shorter.length -= 1;
	const auto cut = OpenWindow(outer, shorter);
	EXPECT_EQ(cut.error, Error::Corrupt) << cut.detail;
	EXPECT_FALSE(cut.reader);
}

TEST_F(ArchiveWindowTest, AWindowOutsideTheFileIsRefusedBeforeAnyRead)
{
	const auto inner = Built("inner.zip",
	                         Kind::ZipStored,
	                         {
	                                 {"a.txt", "aaaa"}
        });
	const auto outer = Built("outer.zip",
	                         Kind::ZipStored,
	                         {
	                                 {"inner.zip", slurp(inner)}
        });
	const auto size  = static_cast<int64_t>(fs::file_size(outer));
	auto past        = ByteWindow{};
	past.offset      = size - 10;
	past.length      = 100;
	auto opened      = OpenWindow(outer, past);
	EXPECT_EQ(opened.error, Error::OpenFailed);
	EXPECT_NE(opened.detail.find("window"), std::string::npos) << opened.detail;
	auto tiny   = ByteWindow{};
	tiny.offset = 0;
	tiny.length = 21; // one short of an end record
	EXPECT_EQ(OpenWindow(outer, tiny).error, Error::OpenFailed);
}

TEST_F(ArchiveWindowTest, OnlyAZipIsReadThroughAWindow)
{
	const auto inner = Built("inner.7z",
	                         Kind::SevenZip,
	                         {
	                                 {"a.txt", "aaaa"}
        });
	const auto outer = Built("outer.zip",
	                         Kind::ZipStored,
	                         {
	                                 {"inner.7z", slurp(inner)}
        });
	// The index gives a 7z no range; the window is built by hand.
	const auto bytes = slurp(outer);
	const auto at    = bytes.find(slurp(inner));
	ASSERT_NE(at, std::string::npos);
	auto window   = ByteWindow{};
	window.offset = static_cast<int64_t>(at);
	window.length = static_cast<int64_t>(fs::file_size(inner));
	auto opened   = OpenWindow(outer, window);
	EXPECT_EQ(opened.error, Error::Unsupported);
	EXPECT_NE(opened.detail.find("window"), std::string::npos) << opened.detail;
}

TEST_F(ArchiveWindowTest, AMemberThatRunsPastTheWindowIsAnErrorNotAShortRead)
{
	// The inner zip's directory claims 600 bytes for a member that has
	// 100: the window ends inside the member, the read must fail.
	auto lying          = StoredEntry{};
	lying.name          = "disk1.img";
	lying.data          = std::string(100, 'd');
	lying.declared_size = 600;
	const auto inner    = ArchiveFixtures::MakeStoredZip({lying});
	const auto outer    = Built("outer.zip",
                                 Kind::ZipStored,
	                            {
                                         {"inner.zip",
                                          std::string(inner.begin(), inner.end())}
        });
	const auto window   = WindowOf(outer, "inner.zip");
	auto opened         = OpenWindow(outer, window);
	// The directory read catches the lie first (a record past the end)
	// or the walk does; either way nothing is served.
	if (opened.error == Error::None) {
		const auto* disk = by_path(*opened.reader, "disk1.img");
		ASSERT_NE(disk, nullptr);
		auto error = Error::None;
		const auto bytes = read_member(*opened.reader, disk->ordinal, &error);
		EXPECT_NE(error, Error::None);
		EXPECT_LT(bytes.size(), 600u); // never the declared size
	} else {
		EXPECT_EQ(opened.error, Error::Corrupt) << opened.detail;
	}
}

TEST_F(ArchiveWindowTest, ALegacyMemberInsideAWindowDecodes)
{
	// tests/files/archives/legacy holds PKZIP 1.x zips (shrunk and
	// imploded members): wrap one in a stored outer and read it through
	// the window, ctest runs from the source tree.
	const auto legacy = fs::path("tests/files/archives/legacy/SHRUNK.ZIP");
	if (!fs::is_regular_file(legacy)) {
		GTEST_SKIP() << "legacy fixture not found from this directory";
	}
	const auto outer = Built("outer.zip",
	                         Kind::ZipStored,
	                         {
	                                 {"legacy.zip", slurp(legacy)}
        });
	auto as_file     = ArchiveReader::Open(legacy, limits);
	ASSERT_EQ(as_file.error, Error::None) << as_file.detail;
	auto windowed = OpenWindow(outer, WindowOf(outer, "legacy.zip"));
	ASSERT_EQ(windowed.error, Error::None) << windowed.detail;
	ASSERT_EQ(windowed.reader->Entries().size(),
	          as_file.reader->Entries().size());
	for (const auto& e : as_file.reader->Entries()) {
		if (e.is_dir) {
			continue;
		}
		EXPECT_EQ(read_member(*windowed.reader, e.ordinal),
		          read_member(*as_file.reader, e.ordinal))
		        << e.utf8_path;
	}
}

TEST_F(ArchiveWindowTest, MaterializeAllThroughAWindowWritesEveryMember)
{
	const auto inner = Built("inner.zip",
	                         Kind::ZipStored,
	                         {
	                                 {"a.txt", "aaaa"},
                                         {"b.txt",   "bb"}
        });
	const auto outer = Built("outer.zip",
	                         Kind::ZipStored,
	                         {
	                                 {"inner.zip", slurp(inner)}
        });
	auto windowed    = OpenWindow(outer, WindowOf(outer, "inner.zip"));
	ASSERT_EQ(windowed.error, Error::None) << windowed.detail;
	auto detail = std::string();
	ASSERT_EQ(windowed.reader->MaterializeAll(tmp_dir / "all", &detail),
	          Error::None)
	        << detail;
	EXPECT_EQ(slurp(tmp_dir / "all" / "0"), "aaaa");
	EXPECT_EQ(slurp(tmp_dir / "all" / "1"), "bb");
}
