// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//
#include "dos/drives.h"

#include "archive_fixtures.h"
#include "dos/archive/archive_cache.h"
#include "dos/archive/archive_helper_client.h"
#include "dos/archive/archive_reader.h"
#include "dos/archive/archive_tree.h"
#include "dos/dos.h"
#include "dosbox_test_fixture.h"
#include "drive_search_helper.h"
#include "misc/cross.h"
#include "test_temp_dir.h"

#include <gtest/gtest.h>

#include <ctime>
#include <filesystem>
#include <string>
#include <vector>

using namespace ArchiveMount;
using ArchiveFixtures::FileEntry;
using ArchiveFixtures::Kind;
using ArchiveFixtures::WriteArchive;

namespace {

std::vector<FileEntry> Game()
{
	return {
	        {"game/", "", true},
	        {"game/doom.exe", std::string(3000, 'x')},
	        {"game/data/", "", true},
	        {"game/data/level.wad", std::string(5000, 'w')},
	        {"game/readme.txt", "hello"},
	};
}

class ArchiveDriveTest : public ::testing::Test {
protected:
	void SetUp() override
	{
		tmp = TestTempDir::MakeUnique("drive_archive_");
		ASSERT_FALSE(tmp.empty());
		auto opened = ArchiveCache::Open(tmp / "cache", cache_options);
		ASSERT_EQ(opened.status, CacheStatus::Ok) << opened.detail;
		cache = std::move(opened.cache);
	}
	void TearDown() override
	{
		std::error_code ec = {};
		std_fs::remove_all(tmp, ec);
	}
	std_fs::path Built(const std::string& name, const Kind kind,
	                   const std::vector<FileEntry>& entries)
	{
		const auto path = tmp / name;
		auto error      = std::string();
		EXPECT_TRUE(WriteArchive(path, kind, entries, nullptr, &error))
		        << error;
		return path;
	}
	std::unique_ptr<ArchiveReader> Reader(const std_fs::path& path)
	{
		auto opened = ArchiveReader::Open(path, limits);
		EXPECT_EQ(opened.error, Error::None) << opened.detail;
		return std::move(opened.reader);
	}
	std::shared_ptr<ArchiveDrive> Drive(const std_fs::path& archive,
	                                    const std::string& root = "",
	                                    const bool with_lease   = true)
	{
		auto source                                = Reader(archive);
		std::unique_ptr<ArchiveCache::Lease> lease = {};
		if (with_lease) {
			auto got = cache->Acquire(*source, archive);
			EXPECT_EQ(got.status, CacheStatus::Ok) << got.detail;
			lease = std::move(got.lease);
		}
		auto made = ArchiveDrive::Create(std::move(source),
		                                 std::move(lease),
		                                 root,
		                                 "DOOM",
		                                 archive.string(),
		                                 drive_options);
		EXPECT_TRUE(made.drive) << made.error;
		return made.drive;
	}
	static std::vector<std::string> Names(const std::vector<ArchiveDrive::Listing>& list)
	{
		std::vector<std::string> out = {};
		for (const auto& item : list) {
			out.push_back(item.name);
		}
		return out;
	}
	static std::string ReadAll(DOS_File& file, const uint16_t chunk = 4096)
	{
		std::string out = {};
		std::vector<uint8_t> buf(chunk);
		for (;;) {
			uint16_t n = chunk;
			EXPECT_TRUE(file.Read(buf.data(), &n));
			if (n == 0) {
				break;
			}
			out.append(reinterpret_cast<const char*>(buf.data()), n);
		}
		return out;
	}

	std_fs::path tmp                    = {};
	CacheOptions cache_options          = {};
	ArchiveDrive::Options drive_options = {};
	Limits limits                       = {};
	std::unique_ptr<ArchiveCache> cache = {};
};

TEST_F(ArchiveDriveTest, ListsDirectoriesInIndexOrderWithSizesAndStamps)
{
	auto drive = Drive(Built("doom.zip", Kind::ZipStored, Game()));
	ASSERT_TRUE(drive);
	EXPECT_EQ(drive->GetType(), DosDriveType::Archive);
	// DIR searches with the directory bit set, as DOS does.
	FatAttributeFlags all                   = {};
	all.directory                           = true;
	std::vector<ArchiveDrive::Listing> root = {};
	ASSERT_TRUE(drive->List("", "????????.???", all, false, root));
	EXPECT_EQ(Names(root), std::vector<std::string>{"GAME"});
	EXPECT_TRUE(root[0].attr.directory);
	EXPECT_TRUE(root[0].attr.read_only);

	std::vector<ArchiveDrive::Listing> game = {};
	ASSERT_TRUE(drive->List("GAME", "????????.???", all, false, game));
	EXPECT_EQ(Names(game),
	          (std::vector<std::string>{".", "..", "DOOM.EXE", "DATA", "README.TXT"}));
	EXPECT_EQ(game[2].size, 3000u);
	EXPECT_FALSE(game[2].attr.directory);
	EXPECT_TRUE(game[3].attr.directory);
	EXPECT_EQ(game[4].size, 5u);
	// 1990-01-01 00:00 UTC through the engine's own packing.
	const time_t stamp = 631152000;
	struct tm local    = {};
	ASSERT_TRUE(cross::localtime_r(&stamp, &local));
	EXPECT_EQ(game[2].date, DOS_PackDate(local));
	EXPECT_EQ(game[2].time, DOS_PackTime(local));
	const auto packed = PackDosDateTime(631152000);
	EXPECT_EQ(packed.date, DOS_PackDate(local));
	EXPECT_EQ(packed.time, DOS_PackTime(local));
	EXPECT_FALSE(drive->List("NOPE", "????????.???", {}, false, game));
	EXPECT_EQ(dos.errorcode, DOSERR_PATH_NOT_FOUND);
}

TEST_F(ArchiveDriveTest, DotEntriesPatternsAndAttributeMasks)
{
	auto drive = Drive(Built("doom.zip", Kind::ZipStored, Game()));
	ASSERT_TRUE(drive);
	FatAttributeFlags dirs                  = {};
	dirs.directory                          = true;
	std::vector<ArchiveDrive::Listing> list = {};
	ASSERT_TRUE(drive->List("GAME", "????????.???", dirs, false, list));
	EXPECT_EQ(Names(list),
	          (std::vector<std::string>{".", "..", "DOOM.EXE", "DATA", "README.TXT"}));
	list.clear();
	ASSERT_TRUE(drive->List("", "????????.???", dirs, false, list));
	EXPECT_EQ(Names(list), std::vector<std::string>{"GAME"});
	list.clear();
	ASSERT_TRUE(drive->List("GAME", "????????.EXE", dirs, false, list));
	EXPECT_EQ(Names(list), std::vector<std::string>{"DOOM.EXE"});
	list.clear();
	ASSERT_TRUE(drive->List("GAME", "DOOM    .EXE", {}, false, list));
	EXPECT_EQ(Names(list), std::vector<std::string>{"DOOM.EXE"});
	list.clear();
	// Files only: no directories and no dots.
	ASSERT_TRUE(drive->List("GAME", "????????.???", {}, false, list));
	EXPECT_EQ(Names(list), (std::vector<std::string>{"DOOM.EXE", "README.TXT"}));
	list.clear();
	// The volume label at the root.
	ASSERT_TRUE(drive->List("", "????????.???", FatAttributeFlags::Volume, false, list));
	ASSERT_EQ(list.size(), 1u);
	EXPECT_EQ(list[0].name, "DOOM");
	EXPECT_TRUE(list[0].attr.volume);
	EXPECT_STREQ(drive->GetLabel(), "DOOM");
}

TEST_F(ArchiveDriveTest, OpensReadsAndSeeksAMemberInMemory)
{
	auto drive = Drive(Built("doom.zip", Kind::ZipDeflate, Game()));
	ASSERT_TRUE(drive);
	auto file = drive->FileOpen("GAME\\README.TXT", OPEN_READ);
	ASSERT_TRUE(file);
	EXPECT_TRUE(file->IsOnReadOnlyMedium());
	EXPECT_EQ(file->GetInformation(), 0x40);
	EXPECT_EQ(ReadAll(*file), "hello");
	uint32_t pos = 0;
	ASSERT_TRUE(file->Seek(&pos, DOS_SEEK_END));
	EXPECT_EQ(pos, 5u);
	pos = 1;
	ASSERT_TRUE(file->Seek(&pos, DOS_SEEK_SET));
	EXPECT_EQ(ReadAll(*file), "ello");
	// DOS may seek past the end; reads there return nothing.
	pos = 100;
	ASSERT_TRUE(file->Seek(&pos, DOS_SEEK_SET));
	EXPECT_EQ(pos, 100u);
	uint16_t n = 10;
	uint8_t buf[10];
	EXPECT_TRUE(file->Read(buf, &n));
	EXPECT_EQ(n, 0);
	// A relative seek that goes negative is refused.
	pos = static_cast<uint32_t>(-200);
	EXPECT_FALSE(file->Seek(&pos, DOS_SEEK_CUR));
	pos = static_cast<uint32_t>(-2);
	ASSERT_TRUE(file->Seek(&pos, DOS_SEEK_END));
	EXPECT_EQ(pos, 3u);
	EXPECT_EQ(ReadAll(*file), "lo");
	n = 1;
	EXPECT_FALSE(file->Write(buf, &n));
	file->Close();
	EXPECT_EQ(drive->RetainedMemoryBytes(), 5);
}

TEST_F(ArchiveDriveTest, AnEmptyExtensionNamesTheNameWithoutOne)
{
	// G8/G9: Ski or Die's runtime opens "skiega." for SKIEGA; DOS keeps the
	// bare dot through its name canonicalization, and "NAME." is NAME.
	auto drive = Drive(Built("ski.zip",
	                         Kind::ZipStored,
	                         {
	                                 {         "SKIEGA",   "ega"},
	                                 {"GAME/README.TXT", "hello"},
        }));
	ASSERT_TRUE(drive);
	auto file = drive->FileOpen("SKIEGA.", OPEN_READ);
	ASSERT_TRUE(file);
	EXPECT_EQ(ReadAll(*file), "ega");
	EXPECT_TRUE(drive->FileExists("SKIEGA."));
	FatAttributeFlags attr = {};
	EXPECT_TRUE(drive->GetFileAttr("SKIEGA.", &attr));
	EXPECT_TRUE(drive->FileOpen("GAME.\\README.TXT", OPEN_READ));
	// A real extension still has to match, and a lone dot stays no name.
	EXPECT_FALSE(drive->FileOpen("SKIEGA.X", OPEN_READ));
	EXPECT_FALSE(drive->FileOpen("SKIEGA..", OPEN_READ));
}

TEST_F(ArchiveDriveTest, OpenForWriteAndEveryWriteEntryPointAreAccessDenied)
{
	auto drive = Drive(Built("doom.zip", Kind::ZipStored, Game()));
	ASSERT_TRUE(drive);
	EXPECT_FALSE(drive->FileOpen("GAME\\README.TXT", OPEN_WRITE));
	EXPECT_EQ(dos.errorcode, DOSERR_ACCESS_DENIED);
	EXPECT_FALSE(drive->FileOpen("GAME\\DATA", OPEN_READ));
	EXPECT_EQ(dos.errorcode, DOSERR_FILE_NOT_FOUND);
	EXPECT_FALSE(drive->FileOpen("GAME\\NOPE.TXT", OPEN_READ));
	EXPECT_EQ(dos.errorcode, DOSERR_FILE_NOT_FOUND);
	EXPECT_FALSE(drive->FileCreate("NEW.TXT", {}));
	EXPECT_EQ(dos.errorcode, DOSERR_ACCESS_DENIED);
	EXPECT_FALSE(drive->FileUnlink("GAME\\README.TXT"));
	EXPECT_EQ(dos.errorcode, DOSERR_ACCESS_DENIED);
	EXPECT_FALSE(drive->MakeDir("NEWDIR"));
	EXPECT_EQ(dos.errorcode, DOSERR_ACCESS_DENIED);
	EXPECT_FALSE(drive->RemoveDir("GAME\\DATA"));
	EXPECT_EQ(dos.errorcode, DOSERR_ACCESS_DENIED);
	EXPECT_FALSE(drive->Rename("GAME\\README.TXT", "GAME\\X.TXT"));
	EXPECT_EQ(dos.errorcode, DOSERR_ACCESS_DENIED);
	EXPECT_FALSE(drive->SetFileAttr("GAME\\README.TXT", {}));
	EXPECT_EQ(dos.errorcode, DOSERR_ACCESS_DENIED);
	EXPECT_TRUE(drive->IsReadOnly());
	EXPECT_FALSE(drive->IsRemote());
	EXPECT_FALSE(drive->IsRemovable());
	EXPECT_EQ(drive->GetMediaByte(), 0xF8);

	FatAttributeFlags attr = {};
	EXPECT_TRUE(drive->GetFileAttr("GAME\\README.TXT", &attr));
	EXPECT_TRUE(attr.read_only);
	EXPECT_FALSE(attr.directory);
	EXPECT_TRUE(drive->GetFileAttr("GAME\\DATA", &attr));
	EXPECT_TRUE(attr.directory);
	EXPECT_TRUE(drive->GetFileAttr("", &attr));
	EXPECT_TRUE(attr.directory);
	EXPECT_FALSE(drive->GetFileAttr("NOPE", &attr));
	EXPECT_TRUE(drive->TestDir(""));
	EXPECT_TRUE(drive->TestDir("GAME\\DATA"));
	EXPECT_FALSE(drive->TestDir("GAME\\README.TXT"));
	EXPECT_TRUE(drive->FileExists("GAME\\README.TXT"));
	EXPECT_FALSE(drive->FileExists("GAME\\DATA"));
	EXPECT_FALSE(drive->FileExists("game\\readme.txt"));
}

TEST_F(ArchiveDriveTest, ALargeMemberIsServedFromTheCacheFile)
{
	drive_options.memory_limit_bytes = 1000;
	const auto archive = Built("doom.zip", Kind::ZipDeflate, Game());
	auto drive         = Drive(archive);
	ASSERT_TRUE(drive);
	auto wad = drive->FileOpen("GAME\\DATA\\LEVEL.WAD", OPEN_READ);
	ASSERT_TRUE(wad);
	EXPECT_EQ(ReadAll(*wad, 1234), std::string(5000, 'w'));
	EXPECT_EQ(drive->RetainedMemoryBytes(), 0);
	// Two opens of one member read independently.
	auto second = drive->FileOpen("GAME\\DATA\\LEVEL.WAD", OPEN_READ);
	ASSERT_TRUE(second);
	uint32_t pos = 4998;
	ASSERT_TRUE(wad->Seek(&pos, DOS_SEEK_SET));
	EXPECT_EQ(ReadAll(*wad), "ww");
	EXPECT_EQ(ReadAll(*second).size(), 5000u);
	wad->Close();
	second->Close();
	auto small = drive->FileOpen("GAME\\README.TXT", OPEN_READ);
	ASSERT_TRUE(small);
	EXPECT_EQ(ReadAll(*small), "hello");
	EXPECT_EQ(drive->RetainedMemoryBytes(), 5);
	const auto entry_dir = ArchiveCache::EntryDirectory(
	        cache->Directory(), "doom", drive->Source().IdentityHash());
	EXPECT_TRUE(std_fs::is_regular_file(entry_dir / FilesDirName / "3"));
	EXPECT_FALSE(std_fs::exists(entry_dir / FilesDirName / "4"));
}

TEST_F(ArchiveDriveTest, MemoryIsRetainedUpToTheBudgetButNeverEvictedFromUnderAnOpenFile)
{
	drive_options.memory_limit_bytes  = 10000;
	drive_options.memory_retain_bytes = 4000;
	auto drive = Drive(Built("doom.zip", Kind::ZipStored, Game()));
	ASSERT_TRUE(drive);
	{
		auto exe = drive->FileOpen("GAME\\DOOM.EXE", OPEN_READ);
		ASSERT_TRUE(exe);
		exe->Close();
	}
	EXPECT_EQ(drive->RetainedMemoryBytes(), 3000);
	auto wad = drive->FileOpen("GAME\\DATA\\LEVEL.WAD", OPEN_READ);
	ASSERT_TRUE(wad);
	// 3000 + 5000 is over 4000: the EXE, unheld, goes.
	EXPECT_EQ(drive->RetainedMemoryBytes(), 5000);
	auto exe = drive->FileOpen("GAME\\DOOM.EXE", OPEN_READ);
	ASSERT_TRUE(exe);
	// The WAD is held open and stays; the budget is exceeded for now.
	EXPECT_EQ(drive->RetainedMemoryBytes(), 8000);
	EXPECT_EQ(ReadAll(*wad).size(), 5000u);
	wad->Close();
	wad.reset();
	exe->Close();
	exe.reset();
	auto readme = drive->FileOpen("GAME\\README.TXT", OPEN_READ);
	ASSERT_TRUE(readme);
	// 8005 over 4000: the WAD goes first and the rest fits the budget.
	EXPECT_EQ(drive->RetainedMemoryBytes(), 3005);
}

TEST_F(ArchiveDriveTest, WithoutALeaseALargeMemberCannotBeOpened)
{
	drive_options.memory_limit_bytes = 1000;
	auto drive = Drive(Built("doom.zip", Kind::ZipStored, Game()), "", false);
	ASSERT_TRUE(drive);
	EXPECT_TRUE(drive->FileOpen("GAME\\README.TXT", OPEN_READ));
	EXPECT_FALSE(drive->FileOpen("GAME\\DOOM.EXE", OPEN_READ));
	EXPECT_EQ(dos.errorcode, DOSERR_ACCESS_DENIED);
}

TEST_F(ArchiveDriveTest, ASequentialArchiveIsMaterializedWhenTheDriveIsCreated)
{
	drive_options.memory_limit_bytes = 1000;
	const auto archive = Built("doom.tar.gz", Kind::TarGz, Game());
	auto source        = Reader(archive);
	ASSERT_EQ(source->GetAccess(), Access::Sequential);
	auto made = ArchiveDrive::Create(
	        std::move(source), {}, "", "DOOM", archive.string(), drive_options);
	EXPECT_FALSE(made.drive);
	EXPECT_NE(made.error.find("cache"), std::string::npos);
	auto drive = Drive(archive);
	ASSERT_TRUE(drive);
	const auto entry_dir = ArchiveCache::EntryDirectory(
	        cache->Directory(), "doom.tar", drive->Source().IdentityHash());
	EXPECT_TRUE(std_fs::is_regular_file(entry_dir / FilesDirName / "1"));
	EXPECT_TRUE(std_fs::is_regular_file(entry_dir / FilesDirName / "3"));
	EXPECT_TRUE(std_fs::is_regular_file(entry_dir / FilesDirName / "4"));
	auto exe = drive->FileOpen("GAME\\DOOM.EXE", OPEN_READ);
	ASSERT_TRUE(exe);
	EXPECT_EQ(ReadAll(*exe).size(), 3000u);
	auto small = drive->FileOpen("GAME\\README.TXT", OPEN_READ);
	ASSERT_TRUE(small);
	EXPECT_EQ(ReadAll(*small), "hello");
}

TEST_F(ArchiveDriveTest, ARootPathNarrowsTheDrive)
{
	auto drive = Drive(Built("doom.zip", Kind::ZipStored, Game()), "GAME");
	ASSERT_TRUE(drive);
	std::vector<ArchiveDrive::Listing> root = {};
	ASSERT_TRUE(drive->List("", "????????.???", {}, false, root));
	EXPECT_EQ(Names(root), (std::vector<std::string>{"DOOM.EXE", "README.TXT"}));
	EXPECT_TRUE(drive->TestDir("DATA"));
	EXPECT_FALSE(drive->TestDir("GAME"));
	auto file = drive->FileOpen("README.TXT", OPEN_READ);
	ASSERT_TRUE(file);
	EXPECT_EQ(ReadAll(*file), "hello");
	auto source = Reader(tmp / "doom.zip");
	auto made   = ArchiveDrive::Create(
                std::move(source), {}, "NOPE", "DOOM", "x", drive_options);
	EXPECT_FALSE(made.drive);
	EXPECT_FALSE(made.error.empty());
}

TEST_F(ArchiveDriveTest, AllocationInfoReportsTheDeclaredTotalAsUsedAndNothingFree)
{
	auto drive = Drive(Built("doom.zip", Kind::ZipStored, Game()));
	ASSERT_TRUE(drive);
	uint16_t bytes_sector = 0, total = 0, free = 0;
	uint8_t sectors_cluster = 0;
	ASSERT_TRUE(drive->AllocationInfo(&bytes_sector, &sectors_cluster, &total, &free));
	EXPECT_EQ(bytes_sector, 512);
	EXPECT_EQ(sectors_cluster, 1);
	EXPECT_EQ(total, 16); // 8005 bytes in 512-byte clusters, rounded up
	EXPECT_EQ(free, 0);
	const auto big = ArchiveDrive::ComputeAllocation(4ll * 1024 * 1024 * 1024);
	EXPECT_EQ(big.sectors_cluster, 128);
	EXPECT_EQ(big.total_clusters, 65535);
	const auto empty = ArchiveDrive::ComputeAllocation(0);
	EXPECT_EQ(empty.sectors_cluster, 1);
	EXPECT_EQ(empty.total_clusters, 0);
}

TEST_F(ArchiveDriveTest, LabelsAreFoldedToFatForm)
{
	EXPECT_EQ(ArchiveDrive::FoldLabel("doom"), "DOOM");
	EXPECT_EQ(ArchiveDrive::FoldLabel("Quest for Glory IV"), "QUEST_FOR_G");
	EXPECT_EQ(ArchiveDrive::FoldLabel("x.y-z_1"), "X_Y-Z_1");
	EXPECT_EQ(ArchiveDrive::FoldLabel(""), "ARCHIVE");
	EXPECT_EQ(ArchiveDrive::FoldLabel("..."), "___");
	EXPECT_EQ(ArchiveDrive::FoldLabel("\xc3\xa9t\xc3\xa9"), "__T__");
}

// A source that forwards to a reader until told to fail.
class FailingSource final : public ArchiveSource {
public:
	explicit FailingSource(std::unique_ptr<ArchiveReader> reader)
	        : reader(std::move(reader))
	{}
	Format GetFormat() const override
	{
		return reader->GetFormat();
	}
	Access GetAccess() const override
	{
		return reader->GetAccess();
	}
	const std::vector<Entry>& Entries() const override
	{
		return reader->Entries();
	}
	int64_t TotalBytes() const override
	{
		return reader->TotalBytes();
	}
	int64_t ArchiveLength() const override
	{
		return reader->ArchiveLength();
	}
	const std::string& IdentityHash() const override
	{
		return reader->IdentityHash();
	}
	StatPair IndexedStat() const override
	{
		return reader->IndexedStat();
	}
	Error Materialize(const int ordinal, std::vector<uint8_t>& out,
	                  std::string* detail) override
	{
		if (fail_with != Error::None) {
			if (detail) {
				*detail = "armed";
			}
			return fail_with;
		}
		return reader->Materialize(ordinal, out, detail);
	}
	Error MaterializeToFile(const int ordinal, const std_fs::path& target,
	                        std::string* detail) override
	{
		if (fail_with != Error::None) {
			return fail_with;
		}
		return reader->MaterializeToFile(ordinal, target, detail);
	}
	Error MaterializeAll(const std_fs::path& directory, std::string* detail) override
	{
		return reader->MaterializeAll(directory, detail);
	}
	std::unique_ptr<ArchiveReader> reader = {};
	Error fail_with                       = Error::None;
};

class ArchiveDriveStateTest : public ArchiveDriveTest {
protected:
	// The drive over a FailingSource with a reconnect spec whose reopen
	// hands out what `next_source` holds; `now` is the spec's clock.
	std::shared_ptr<ArchiveDrive> StatefulDrive(const std_fs::path& archive)
	{
		auto opened = ArchiveReader::Open(archive, limits);
		EXPECT_EQ(opened.error, Error::None) << opened.detail;
		auto failing = std::make_unique<FailingSource>(
		        std::move(opened.reader));
		source   = failing.get();
		auto got = cache->Acquire(*failing, archive);
		EXPECT_EQ(got.status, CacheStatus::Ok) << got.detail;
		drive_options.reconnect = ArchiveDrive::ReconnectSpec{};
		drive_options.reconnect->backoff_seconds = 5;
		drive_options.reconnect->now = [this]() { return now; };
		drive_options.reconnect->reopen =
		        [this](std::string* detail) -> ArchiveDrive::Reopened {
			++reopen_calls;
			if (!next_source) {
				if (detail) {
					*detail = "nothing to reopen";
				}
			}
			return {std::move(next_source), std::move(next_held)};
		};
		auto made = ArchiveDrive::Create(std::move(failing),
		                                 std::move(got.lease),
		                                 "",
		                                 "DOOM",
		                                 archive.string(),
		                                 drive_options);
		EXPECT_TRUE(made.drive) << made.error;
		return made.drive;
	}
	std::unique_ptr<ArchiveSource> FreshReader(const std_fs::path& archive)
	{
		auto opened = ArchiveReader::Open(archive, limits);
		EXPECT_EQ(opened.error, Error::None) << opened.detail;
		return std::move(opened.reader);
	}
	FailingSource* source                                  = nullptr;
	std::unique_ptr<ArchiveSource> next_source             = {};
	std::shared_ptr<ArchiveDrive::HeldResources> next_held = {};
	int reopen_calls                                       = 0;
	int64_t now                                            = 1'700'000'000;
};

TEST_F(ArchiveDriveStateTest, AChangedArchiveTakesTheDriveOutOfReadyAndDosSeesNotReady)
{
	const auto archive = Built("doom.zip", Kind::ZipStored, Game());
	auto drive         = StatefulDrive(archive);
	ASSERT_TRUE(drive);
	EXPECT_EQ(drive->State(), ArchiveMountState::Ready);
	source->fail_with = Error::Changed;
	EXPECT_FALSE(drive->FileOpen("GAME\\README.TXT", OPEN_READ));
	EXPECT_EQ(drive->State(), ArchiveMountState::Changed);
	EXPECT_EQ(dos.errorcode, DOSERR_DRIVE_NOT_READY);
	// Every entry point answers not ready now, without a reconnect
	// attempt inside the backoff (the first failure set the clock).
	std::vector<ArchiveDrive::Listing> list = {};
	EXPECT_FALSE(drive->List("", "????????.???", {}, false, list));
	EXPECT_EQ(dos.errorcode, DOSERR_DRIVE_NOT_READY);
	EXPECT_FALSE(drive->TestDir("GAME"));
	EXPECT_FALSE(drive->FileExists("GAME\\README.TXT"));
	FatAttributeFlags attr = {};
	EXPECT_FALSE(drive->GetFileAttr("GAME\\README.TXT", &attr));
	EXPECT_EQ(reopen_calls, 0);
	EXPECT_STREQ(ArchiveDrive::StateName(drive->State()), "changed");
}

TEST_F(ArchiveDriveStateTest, AReconnectWithTheSameIdentityRestoresTheDrive)
{
	const auto archive = Built("doom.zip", Kind::ZipStored, Game());
	auto drive         = StatefulDrive(archive);
	ASSERT_TRUE(drive);
	source->fail_with = Error::HelperFailed;
	EXPECT_FALSE(drive->FileOpen("GAME\\README.TXT", OPEN_READ));
	EXPECT_EQ(drive->State(), ArchiveMountState::Disconnected);
	// Inside the backoff: no attempt.
	now += 4;
	EXPECT_FALSE(drive->FileOpen("GAME\\README.TXT", OPEN_READ));
	EXPECT_EQ(reopen_calls, 0);
	// Past it: one attempt, which succeeds with an equal identity.
	now += 1;
	next_source = FreshReader(archive);
	auto file   = drive->FileOpen("GAME\\README.TXT", OPEN_READ);
	ASSERT_TRUE(file);
	EXPECT_EQ(reopen_calls, 1);
	EXPECT_EQ(drive->State(), ArchiveMountState::Ready);
	EXPECT_EQ(ReadAll(*file), "hello");
	// The large member goes through the rebound lease and the new source.
	drive_options.memory_limit_bytes = 1000;
	auto wad = drive->FileOpen("GAME\\DATA\\LEVEL.WAD", OPEN_READ);
	ASSERT_TRUE(wad);
	EXPECT_EQ(ReadAll(*wad).size(), 5000u);
}

TEST_F(ArchiveDriveStateTest, AReconnectThatFindsAnotherArchiveLeavesTheDriveLost)
{
	const auto archive = Built("doom.zip", Kind::ZipStored, Game());
	const auto other   = Built("other.zip",
                                 Kind::ZipStored,
	                           {
                                         {"x.txt", "x"}
        });
	auto drive         = StatefulDrive(archive);
	ASSERT_TRUE(drive);
	source->fail_with = Error::OpenFailed;
	EXPECT_FALSE(drive->FileOpen("GAME\\README.TXT", OPEN_READ));
	EXPECT_EQ(drive->State(), ArchiveMountState::Lost);
	now += 10;
	next_source = FreshReader(other);
	EXPECT_FALSE(drive->FileOpen("GAME\\README.TXT", OPEN_READ));
	EXPECT_EQ(reopen_calls, 1);
	EXPECT_EQ(drive->State(), ArchiveMountState::Lost);
	// Nothing to reopen: still Lost, and the attempt is counted.
	now += 10;
	EXPECT_FALSE(drive->FileOpen("GAME\\README.TXT", OPEN_READ));
	EXPECT_EQ(reopen_calls, 2);
	// An explicit Reconnect ignores the backoff.
	next_source = FreshReader(archive);
	EXPECT_TRUE(drive->Reconnect(nullptr));
	EXPECT_EQ(drive->State(), ArchiveMountState::Ready);
	EXPECT_TRUE(drive->FileOpen("GAME\\README.TXT", OPEN_READ));
}

TEST_F(ArchiveDriveStateTest, WithoutALeaseTheDriveItselfRefusesAnotherArchive)
{
	// The lease also compares identities; without one, only the drive's
	// own check stands between a reconnect and a swapped archive.
	const auto archive = Built("doom.zip", Kind::ZipStored, Game());
	const auto other   = Built("other.zip",
                                 Kind::ZipStored,
	                           {
                                         {"x.txt", "x"}
        });
	auto opened        = ArchiveReader::Open(archive, limits);
	ASSERT_EQ(opened.error, Error::None);
	auto failing = std::make_unique<FailingSource>(std::move(opened.reader));
	auto* raw                    = failing.get();
	drive_options.reconnect      = ArchiveDrive::ReconnectSpec{};
	drive_options.reconnect->now = [this]() { return now; };
	drive_options.reconnect->reopen = [this](std::string*) -> ArchiveDrive::Reopened {
		return {std::move(next_source), {}};
	};
	auto made = ArchiveDrive::Create(
	        std::move(failing), {}, "", "DOOM", archive.string(), drive_options);
	ASSERT_TRUE(made.drive) << made.error;
	raw->fail_with = Error::OpenFailed;
	EXPECT_FALSE(made.drive->FileOpen("GAME\\README.TXT", OPEN_READ));
	next_source = FreshReader(other);
	EXPECT_FALSE(made.drive->Reconnect(nullptr));
	EXPECT_EQ(made.drive->State(), ArchiveMountState::Lost);
}

TEST_F(ArchiveDriveStateTest, ACorruptMemberIsThatMembersFailureOnly)
{
	const auto archive = Built("doom.zip", Kind::ZipStored, Game());
	auto drive         = StatefulDrive(archive);
	ASSERT_TRUE(drive);
	source->fail_with = Error::Corrupt;
	EXPECT_FALSE(drive->FileOpen("GAME\\README.TXT", OPEN_READ));
	EXPECT_EQ(dos.errorcode, DOSERR_ACCESS_DENIED);
	EXPECT_EQ(drive->State(), ArchiveMountState::Ready);
	source->fail_with = Error::None;
	EXPECT_TRUE(drive->FileOpen("GAME\\README.TXT", OPEN_READ));
}

TEST_F(ArchiveDriveStateTest, WithoutASpecTheDriveStaysWhereItFell)
{
	const auto archive = Built("doom.zip", Kind::ZipStored, Game());
	auto opened        = ArchiveReader::Open(archive, limits);
	ASSERT_EQ(opened.error, Error::None);
	auto failing = std::make_unique<FailingSource>(std::move(opened.reader));
	auto* raw = failing.get();
	auto made = ArchiveDrive::Create(
	        std::move(failing), {}, "", "DOOM", archive.string(), drive_options);
	ASSERT_TRUE(made.drive) << made.error;
	raw->fail_with = Error::Timeout;
	EXPECT_FALSE(made.drive->FileOpen("GAME\\README.TXT", OPEN_READ));
	EXPECT_EQ(made.drive->State(), ArchiveMountState::Disconnected);
	EXPECT_FALSE(made.drive->Reconnect(nullptr));
	EXPECT_EQ(made.drive->State(), ArchiveMountState::Disconnected);
}

TEST(ArchiveHelperPathOverride, TheOverrideWinsOverTheDefault)
{
	ArchiveMount::SetHelperPathOverride("/nowhere/helper");
	EXPECT_EQ(ArchiveMount::ArchiveHelperClient::DefaultHelperPath(),
	          std_fs::path("/nowhere/helper"));
	ArchiveMount::SetHelperPathOverride({});
	EXPECT_NE(ArchiveMount::ArchiveHelperClient::DefaultHelperPath(),
	          std_fs::path("/nowhere/helper"));
}

// Through DOS itself: the DTA in emulated memory, a file handle, the
// drive installed at Z:.
class ArchiveDriveDosTest : public DOSBoxTestFixture {
protected:
	void TearDown() override
	{
		if (installed) {
			Drives.at(25) = previous;
		}
		drive.reset();
		std::error_code ec = {};
		if (!tmp.empty()) {
			std_fs::remove_all(tmp, ec);
		}
		DOSBoxTestFixture::TearDown();
	}
	// The Game() archive at Z:, restored by TearDown.
	bool MountGame()
	{
		tmp = TestTempDir::MakeUnique("drive_archive_dos_");
		if (tmp.empty()) {
			return false;
		}
		const auto archive = tmp / "doom.zip";
		auto error         = std::string();
		EXPECT_TRUE(WriteArchive(archive, Kind::ZipStored, Game(), nullptr, &error))
		        << error;
		auto opened = ArchiveReader::Open(archive, Limits{});
		EXPECT_EQ(opened.error, Error::None) << opened.detail;
		if (opened.error != Error::None) {
			return false;
		}
		auto made = ArchiveDrive::Create(std::move(opened.reader),
		                                 {},
		                                 "",
		                                 "DOOM",
		                                 archive.string(),
		                                 ArchiveDrive::Options{});
		EXPECT_TRUE(made.drive) << made.error;
		if (!made.drive) {
			return false;
		}
		drive         = made.drive;
		previous      = Drives.at(25);
		Drives.at(25) = drive;
		installed     = true;
		return true;
	}

	std_fs::path tmp                    = {};
	std::shared_ptr<ArchiveDrive> drive = {};
	std::shared_ptr<DOS_Drive> previous = {};
	bool installed                      = false;
};

TEST_F(ArchiveDriveDosTest, FindFirstFindNextAndFileHandlesWorkThroughDos)
{
	const auto tmp = TestTempDir::MakeUnique("drive_archive_dos_");
	ASSERT_FALSE(tmp.empty());
	const auto archive = tmp / "doom.zip";
	auto error         = std::string();
	ASSERT_TRUE(WriteArchive(archive, Kind::ZipStored, Game(), nullptr, &error))
	        << error;
	auto opened = ArchiveReader::Open(archive, Limits{});
	ASSERT_EQ(opened.error, Error::None) << opened.detail;
	auto made = ArchiveDrive::Create(std::move(opened.reader),
	                                 {},
	                                 "",
	                                 "DOOM",
	                                 archive.string(),
	                                 ArchiveDrive::Options{});
	ASSERT_TRUE(made.drive) << made.error;
	const auto previous = Drives.at(25);
	Drives.at(25)       = made.drive;

	FatAttributeFlags attr         = {};
	attr.directory                 = true;
	std::vector<std::string> names = {};
	ASSERT_TRUE(DOS_FindFirst("Z:\\GAME\\*.*", attr, false));
	for (;;) {
		DOS_DTA dta(dos.dta());
		DOS_DTA::Result result = {};
		dta.GetResult(result);
		names.push_back(result.name);
		if (!DOS_FindNext()) {
			break;
		}
	}
	EXPECT_EQ(names,
	          (std::vector<std::string>{".", "..", "DOOM.EXE", "DATA", "README.TXT"}));

	// The fixture has no functional PSP file table (see
	// dos_files_tests), so the handles go through FCB mode, where the
	// entry is the real file handle.
	constexpr bool fcb = true;
	uint16_t handle    = 0;
	ASSERT_TRUE(DOS_OpenFile("Z:\\GAME\\README.TXT", OPEN_READ, &handle, fcb));
	uint8_t buf[16] = {};
	uint16_t n      = sizeof(buf);
	ASSERT_TRUE(DOS_ReadFile(handle, buf, &n, fcb))
	        << "DOS error " << dos.errorcode;
	EXPECT_EQ(std::string(reinterpret_cast<const char*>(buf), n), "hello");
	n = 5;
	EXPECT_FALSE(DOS_WriteFile(handle, buf, &n, fcb));
	ASSERT_TRUE(DOS_CloseFile(handle, fcb));
	EXPECT_FALSE(DOS_OpenFile("Z:\\GAME\\README.TXT", OPEN_WRITE, &handle, fcb));

	Drives.at(25)      = previous;
	std::error_code ec = {};
	std_fs::remove_all(tmp, ec);
}

TEST_F(ArchiveDriveDosTest, ChildSearchesThatRunToTheEndLeaveTheParentSearchAlone)
{
	ASSERT_TRUE(MountGame());
	DriveSearch parent(*drive, 25, dos.dta());
	DriveSearch child(*drive, 25, dos.tables.tempdta);
	ASSERT_TRUE(parent.First("GAME"));
	std::vector<std::string> names = {parent.Name()};
	for (int i = 0; i < MAX_OPENDIRS + 10; ++i) {
		ASSERT_TRUE(child.First("GAME\\DATA")) << i;
		while (child.Next()) {
		}
	}
	while (parent.Next()) {
		names.push_back(parent.Name());
	}
	EXPECT_EQ(names,
	          (std::vector<std::string>{".", "..", "DOOM.EXE", "DATA", "README.TXT"}));
}

TEST_F(ArchiveDriveDosTest, AbandonedSearchesAreEvictedBeforeTheSearchStillReading)
{
	// IF EXIST and friends start a search and never finish it.
	ASSERT_TRUE(MountGame());
	DriveSearch parent(*drive, 25, dos.dta());
	DriveSearch abandoned(*drive, 25, dos.tables.tempdta);
	ASSERT_TRUE(parent.First("GAME"));
	std::vector<std::string> names = {parent.Name()};
	while (names.size() <= 5u) {
		for (int i = 0; i < MAX_OPENDIRS / 2; ++i) {
			ASSERT_TRUE(abandoned.First("GAME\\DATA")) << i;
		}
		if (!parent.Next()) {
			break;
		}
		names.push_back(parent.Name());
	}
	EXPECT_EQ(names,
	          (std::vector<std::string>{".", "..", "DOOM.EXE", "DATA", "README.TXT"}));
}

} // namespace

// --- Phase 3: what the drive holds for its source ---

namespace {

// Records destruction order: the held resources must go after the source.
struct DestructionLog {
	std::vector<std::string> order = {};
};

struct LoggedHeld final : public ArchiveDrive::HeldResources {
	DestructionLog* log = nullptr;
	~LoggedHeld() override
	{
		log->order.push_back("held");
	}
};

// Forwards every call to an inner reader; only the destructor logs.
class LoggedSource final : public ArchiveSource {
public:
	LoggedSource(std::unique_ptr<ArchiveSource> in, DestructionLog* l)
	        : inner(std::move(in)),
	          log(l)
	{}
	~LoggedSource() override
	{
		log->order.push_back("source");
	}
	Format GetFormat() const override
	{
		return inner->GetFormat();
	}
	Access GetAccess() const override
	{
		return inner->GetAccess();
	}
	const std::vector<Entry>& Entries() const override
	{
		return inner->Entries();
	}
	int64_t TotalBytes() const override
	{
		return inner->TotalBytes();
	}
	int64_t ArchiveLength() const override
	{
		return inner->ArchiveLength();
	}
	const std::string& IdentityHash() const override
	{
		return inner->IdentityHash();
	}
	StatPair IndexedStat() const override
	{
		return inner->IndexedStat();
	}
	Error Materialize(const int o, std::vector<uint8_t>& out, std::string* d) override
	{
		return inner->Materialize(o, out, d);
	}
	Error MaterializeToFile(const int o, const std::filesystem::path& t,
	                        std::string* d) override
	{
		return inner->MaterializeToFile(o, t, d);
	}
	Error MaterializeAll(const std::filesystem::path& dir, std::string* d) override
	{
		return inner->MaterializeAll(dir, d);
	}

private:
	std::unique_ptr<ArchiveSource> inner = {};
	DestructionLog* log                  = nullptr;
};

} // namespace

TEST_F(ArchiveDriveTest, HeldResourcesAreReleasedAfterTheSource)
{
	DestructionLog log = {};
	auto held          = std::make_shared<LoggedHeld>();
	held->log          = &log;
	auto source        = std::make_unique<LoggedSource>(
                Reader(Built("doom.zip", Kind::ZipStored, Game())), &log);
	ArchiveDrive::Options options = {};
	options.held                  = held;
	held.reset();
	auto made = ArchiveDrive::Create(
	        std::move(source), nullptr, "", "DOOM", "doom.zip", options);
	ASSERT_TRUE(made.drive) << made.error;
	options.held.reset(); // the drive holds the only reference now
	made.drive.reset();
	ASSERT_EQ(log.order.size(), 2u);
	EXPECT_EQ(log.order[0], "source");
	EXPECT_EQ(log.order[1], "held");
}

namespace {
struct NamedHeld final : public ArchiveDrive::HeldResources {
	std::vector<std::string>* log = nullptr;
	std::string name              = {};
	~NamedHeld() override
	{
		log->push_back(name);
	}
};
} // namespace

TEST_F(ArchiveDriveStateTest, AReconnectReplacesTheHeldResources)
{
	// What the old source read through goes with it; what the fresh one
	// reads through stays for the drive's life.
	auto log           = std::vector<std::string>{};
	auto old_held      = std::make_shared<NamedHeld>();
	old_held->log      = &log;
	old_held->name     = "old";
	drive_options.held = old_held;
	old_held.reset();
	const auto archive = Built("doom.zip", Kind::ZipStored, Game());
	auto drive         = StatefulDrive(archive);
	ASSERT_TRUE(drive);
	drive_options.held.reset();
	// A Ready drive reconnects nothing; a Changed one does.
	source->fail_with = Error::Changed;
	EXPECT_FALSE(drive->FileOpen("GAME\\README.TXT", OPEN_READ));
	ASSERT_EQ(drive->State(), ArchiveMountState::Changed);
	auto fresh_held  = std::make_shared<NamedHeld>();
	fresh_held->log  = &log;
	fresh_held->name = "fresh";
	next_held        = fresh_held;
	fresh_held.reset();
	next_source = FreshReader(archive);
	auto detail = std::string();
	ASSERT_TRUE(drive->Reconnect(&detail)) << detail;
	EXPECT_EQ(log, std::vector<std::string>{"old"});
	drive.reset();
	EXPECT_EQ(log, (std::vector<std::string>{"old", "fresh"}));
}
