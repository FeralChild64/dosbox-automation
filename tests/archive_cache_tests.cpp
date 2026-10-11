// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//
#include "dos/archive/archive_cache.h"

#include "archive_fixtures.h"
#include "dos/archive/archive_identity.h"
#include "dos/archive/archive_lock.h"
#include "dos/archive/archive_reader.h"
#include "dos/archive/archive_toml.h"
#include "test_temp_dir.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace ArchiveMount;
using ArchiveFixtures::FileEntry;
using ArchiveFixtures::Kind;
using ArchiveFixtures::WriteArchive;

namespace {

// Forwards to a reader and counts the materializations; can lie about
// the access class and fail on demand, which the real reader cannot.
class CountingSource final : public ArchiveSource {
public:
	explicit CountingSource(std::unique_ptr<ArchiveReader> reader)
	        : reader(std::move(reader))
	{}
	Format GetFormat() const override
	{
		return reader->GetFormat();
	}
	Access GetAccess() const override
	{
		return access.value_or(reader->GetAccess());
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
		return hash.empty() ? reader->IdentityHash() : hash;
	}
	StatPair IndexedStat() const override
	{
		return reader->IndexedStat();
	}
	Error Materialize(const int ordinal, std::vector<uint8_t>& out,
	                  std::string* detail) override
	{
		++materialize_calls;
		return reader->Materialize(ordinal, out, detail);
	}
	Error MaterializeToFile(const int ordinal, const fs::path& target,
	                        std::string* detail) override
	{
		++to_file_calls;
		if (fail_with != Error::None) {
			if (detail) {
				*detail = "armed failure";
			}
			return fail_with;
		}
		return reader->MaterializeToFile(ordinal, target, detail);
	}
	Error MaterializeAll(const fs::path& directory, std::string* detail) override
	{
		++all_calls;
		return reader->MaterializeAll(directory, detail);
	}

	std::unique_ptr<ArchiveReader> reader = {};
	std::optional<Access> access          = {};
	std::string hash                      = {};
	Error fail_with                       = Error::None;
	int materialize_calls                 = 0;
	int to_file_calls                     = 0;
	int all_calls                         = 0;
};

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

class ArchiveCacheTest : public ::testing::Test {
protected:
	void SetUp() override
	{
		tmp = TestTempDir::MakeUnique("archive_cache_");
		ASSERT_FALSE(tmp.empty());
		cache_dir          = tmp / "cache";
		options.free_space = [this](const fs::path&) {
			return free_space;
		};
		options.now = [this]() { return now; };
	}
	void TearDown() override
	{
		std::error_code ec = {};
		fs::remove_all(tmp, ec);
	}
	fs::path Built(const std::string& name, const Kind kind,
	               const std::vector<FileEntry>& entries)
	{
		const auto path = tmp / name;
		auto error      = std::string();
		EXPECT_TRUE(WriteArchive(path, kind, entries, nullptr, &error))
		        << error;
		return path;
	}
	std::unique_ptr<CountingSource> Source(const fs::path& path)
	{
		auto opened = ArchiveReader::Open(path, limits);
		EXPECT_EQ(opened.error, Error::None) << opened.detail;
		return std::make_unique<CountingSource>(std::move(opened.reader));
	}
	std::unique_ptr<ArchiveCache> OpenOk()
	{
		auto opened = ArchiveCache::Open(cache_dir, options);
		EXPECT_EQ(opened.status, CacheStatus::Ok) << opened.detail;
		return std::move(opened.cache);
	}
	std::unique_ptr<ArchiveCache::Lease> AcquireOk(ArchiveCache& cache,
	                                               ArchiveSource& source,
	                                               const fs::path& archive)
	{
		auto got = cache.Acquire(source, archive);
		EXPECT_EQ(got.status, CacheStatus::Ok) << got.detail;
		return std::move(got.lease);
	}
	static int Ordinal(const ArchiveSource& source, const std::string& utf8)
	{
		for (const auto& e : source.Entries()) {
			if (e.utf8_path == utf8) {
				return e.ordinal;
			}
		}
		return -2;
	}
	static int64_t SizeOf(const fs::path& path)
	{
		std::error_code ec = {};
		const auto size    = fs::file_size(path, ec);
		return ec ? -1 : static_cast<int64_t>(size);
	}
	static void Touch(const fs::path& path, const std::string& content)
	{
		std::ofstream out(path, std::ios::binary);
		out << content;
	}

	fs::path tmp         = {};
	fs::path cache_dir   = {};
	CacheOptions options = {};
	Limits limits        = {};
	int64_t free_space   = 100ll * 1024 * 1024 * 1024;
	int64_t now          = 1'700'000'000;
};

TEST_F(ArchiveCacheTest, OpenCreatesTheDirectoryWithTheTag)
{
	auto cache = OpenOk();
	ASSERT_TRUE(cache);
	EXPECT_TRUE(fs::is_directory(cache_dir));
	const auto tag = cache_dir / CacheTagName;
	ASSERT_TRUE(fs::is_regular_file(tag));
	std::ifstream in(tag);
	auto first_line = std::string();
	std::getline(in, first_line);
	EXPECT_EQ(first_line, "Signature: 8a477f597d28d172789f06886806bc55");
#if !defined(WIN32)
	EXPECT_EQ(fs::status(cache_dir).permissions() & fs::perms::others_all,
	          fs::perms::none);
	EXPECT_EQ(fs::status(cache_dir).permissions() & fs::perms::group_all,
	          fs::perms::none);
#endif
	// Reopening an existing tagged directory is fine.
	EXPECT_TRUE(OpenOk());
}

TEST_F(ArchiveCacheTest, OpenRefusesAFileALinkAndAnUntaggedNonEmptyDirectory)
{
	Touch(tmp / "file", "x");
	EXPECT_EQ(ArchiveCache::Open(tmp / "file", options).status,
	          CacheStatus::Refused);
	fs::create_directories(tmp / "busy");
	Touch(tmp / "busy" / "something", "x");
	EXPECT_EQ(ArchiveCache::Open(tmp / "busy", options).status,
	          CacheStatus::Refused);
	fs::create_directories(tmp / "empty");
	EXPECT_EQ(ArchiveCache::Open(tmp / "empty", options).status, CacheStatus::Ok);
	EXPECT_TRUE(fs::is_regular_file(tmp / "empty" / CacheTagName));
#if !defined(WIN32)
	fs::create_directory_symlink(tmp / "empty", tmp / "link");
	EXPECT_EQ(ArchiveCache::Open(tmp / "link", options).status,
	          CacheStatus::Refused);
#endif
	EXPECT_EQ(ArchiveCache::Open(tmp / "file" / "below", options).status,
	          CacheStatus::Refused);
}

TEST_F(ArchiveCacheTest, EntryLayoutIsLetterThenStemAndHash)
{
	EXPECT_EQ(ArchiveCache::SpreadLetter("Doom"), 'd');
	EXPECT_EQ(ArchiveCache::SpreadLetter("doom"), 'd');
	EXPECT_EQ(ArchiveCache::SpreadLetter("7th_guest"), '0');
	EXPECT_EQ(ArchiveCache::SpreadLetter("_x"), '_');
	EXPECT_EQ(ArchiveCache::SpreadLetter(".hidden"), '_');
	EXPECT_EQ(ArchiveCache::SpreadLetter("-x"), '_');
	EXPECT_EQ(ArchiveCache::SpreadLetter(""), '_');
	const auto dir = ArchiveCache::EntryDirectory(fs::path("/c"),
	                                              "Game Disk!",
	                                              "0123456789abcdefrest");
	EXPECT_EQ(dir, fs::path("/c") / "g" / "Game_Disk_-01234567");
}

TEST_F(ArchiveCacheTest, AcquireCreatesTheEntryAndRecordsTheArchive)
{
	const auto archive      = Built("doom.zip", Kind::ZipStored, Game());
	auto source             = Source(archive);
	auto cache              = OpenOk();
	const auto expected_dir = ArchiveCache::EntryDirectory(
	        cache_dir, "doom", source->IdentityHash());
	{
		auto lease = AcquireOk(*cache, *source, archive);
		ASSERT_TRUE(lease);
		EXPECT_FALSE(lease->WasReused());
		EXPECT_EQ(lease->Directory(), expected_dir);
		EXPECT_TRUE(fs::is_directory(lease->FilesDirectory()));
		EXPECT_TRUE(fs::is_regular_file(expected_dir / LockName));
		auto error = std::string();
		const auto doc = ArchiveToml::ReadFile(expected_dir / EntryTomlName,
		                                       &error);
		ASSERT_TRUE(doc) << error;
		EXPECT_EQ(doc->root.Integer("version"), EntryTomlVersion);
		EXPECT_EQ(doc->root.String("archive_path"), archive.string());
		EXPECT_EQ(doc->root.String("identity_hash"), source->IdentityHash());
		EXPECT_EQ(doc->root.Integer("entry_count"),
		          static_cast<int64_t>(source->Entries().size()));
		EXPECT_EQ(doc->root.Integer("total_bytes"), source->TotalBytes());
		EXPECT_EQ(doc->root.Integer("archive_size"),
		          source->IndexedStat().size);
		EXPECT_EQ(doc->root.Integer("archive_mtime"),
		          source->IndexedStat().mtime);
		EXPECT_EQ(doc->root.Integer("created"), now);
		EXPECT_EQ(doc->root.Integer("last_use"), now);
		EXPECT_FALSE(lease->IsComplete());
	}
	// Released: last_use moves to the release time, the entry is reused.
	now += 100;
	{
		auto lease = AcquireOk(*cache, *source, archive);
		ASSERT_TRUE(lease);
		EXPECT_TRUE(lease->WasReused());
	}
	auto error = std::string();
	const auto doc = ArchiveToml::ReadFile(expected_dir / EntryTomlName, &error);
	ASSERT_TRUE(doc) << error;
	EXPECT_EQ(doc->root.Integer("created"), now - 100);
	EXPECT_EQ(doc->root.Integer("last_use"), now);
	EXPECT_EQ(cache->Measure().entries, 1);
}

TEST_F(ArchiveCacheTest, MemberFileMaterializesOnceAndServesTheFileAfter)
{
	const auto archive = Built("doom.zip", Kind::ZipDeflate, Game());
	auto source        = Source(archive);
	auto cache         = OpenOk();
	auto lease         = AcquireOk(*cache, *source, archive);
	const auto wad     = Ordinal(*source, "game/data/level.wad");
	fs::path path      = {};
	auto detail        = std::string();
	ASSERT_EQ(lease->MemberFile(wad, path, &detail), CacheStatus::Ok) << detail;
	EXPECT_EQ(path, lease->FilesDirectory() / std::to_string(wad));
	EXPECT_EQ(SizeOf(path), 5000);
	EXPECT_EQ(source->to_file_calls, 1);
	fs::path again = {};
	ASSERT_EQ(lease->MemberFile(wad, again, &detail), CacheStatus::Ok) << detail;
	EXPECT_EQ(again, path);
	EXPECT_EQ(source->to_file_calls, 1);
	EXPECT_FALSE(lease->IsComplete());
	EXPECT_EQ(cache->Measure().bytes, 5000);
	// A directory, an unknown ordinal and the implied ordinal are refused.
	EXPECT_EQ(lease->MemberFile(Ordinal(*source, "game/data"), path, &detail),
	          CacheStatus::Failed);
	EXPECT_EQ(lease->MemberFile(999, path, &detail), CacheStatus::Failed);
	EXPECT_EQ(lease->MemberFile(ImpliedOrdinal, path, &detail),
	          CacheStatus::Failed);
}

TEST_F(ArchiveCacheTest, ADetachedLeaseOutlivesItsSourceAndKeepsTheEntryLocked)
{
	const auto archive = Built("doom.zip", Kind::ZipDeflate, Game());
	auto source        = Source(archive);
	auto cache         = OpenOk();
	auto lease         = AcquireOk(*cache, *source, archive);
	const auto wad     = Ordinal(*source, "game/data/level.wad");
	fs::path path      = {};
	auto detail        = std::string();
	ASSERT_EQ(lease->MemberFile(wad, path, &detail), CacheStatus::Ok) << detail;
	lease->Detach();
	source.reset(); // under ASan a later use of the source is a
	                // heap-use-after-free
	EXPECT_EQ(lease->MemberFile(wad, path, &detail), CacheStatus::Failed);
	EXPECT_NE(detail.find("detached"), std::string::npos) << detail;
	EXPECT_EQ(lease->EnsureAll(&detail), CacheStatus::Failed);
	EXPECT_FALSE(lease->IsComplete());
	EXPECT_EQ(SizeOf(path), 5000);
	EXPECT_FALSE(FileLock::TryAcquire(lease->Directory() / LockName,
	                                  FileLock::Mode::Exclusive));
	const auto dir = lease->Directory();
	lease.reset();
	EXPECT_TRUE(FileLock::TryAcquire(dir / LockName, FileLock::Mode::Exclusive));
}

TEST_F(ArchiveCacheTest, ADetachedLeaseRefusesARebind)
{
	const auto archive = Built("doom.zip", Kind::ZipDeflate, Game());
	auto source        = Source(archive);
	auto again         = Source(archive);
	auto cache         = OpenOk();
	auto lease         = AcquireOk(*cache, *source, archive);
	lease->Detach();
	EXPECT_FALSE(lease->Rebind(*again));
}

TEST_F(ArchiveCacheTest, AWrongSizedOrForeignFileInTheEntryIsHandled)
{
	const auto archive = Built("doom.zip", Kind::ZipStored, Game());
	auto source        = Source(archive);
	auto cache         = OpenOk();
	auto lease         = AcquireOk(*cache, *source, archive);
	const auto exe     = Ordinal(*source, "game/doom.exe");
	const auto target  = lease->FilesDirectory() / std::to_string(exe);
	Touch(target, "short");
	fs::path path = {};
	auto detail   = std::string();
	ASSERT_EQ(lease->MemberFile(exe, path, &detail), CacheStatus::Ok) << detail;
	EXPECT_EQ(SizeOf(path), 3000);
	EXPECT_EQ(source->to_file_calls, 1);
#if !defined(WIN32)
	const auto wad = Ordinal(*source, "game/data/level.wad");
	fs::create_symlink(tmp / "nowhere",
	                   lease->FilesDirectory() / std::to_string(wad));
	EXPECT_EQ(lease->MemberFile(wad, path, &detail), CacheStatus::Failed);
	EXPECT_TRUE(fs::is_symlink(lease->FilesDirectory() / std::to_string(wad)));
#endif
}

TEST_F(ArchiveCacheTest, ASourceFailureIsReportedAndLeavesNoFile)
{
	const auto archive = Built("doom.zip", Kind::ZipStored, Game());
	auto source        = Source(archive);
	source->fail_with  = Error::Corrupt;
	auto cache         = OpenOk();
	auto lease         = AcquireOk(*cache, *source, archive);
	const auto exe     = Ordinal(*source, "game/doom.exe");
	fs::path path      = {};
	auto detail        = std::string();
	EXPECT_EQ(lease->MemberFile(exe, path, &detail), CacheStatus::SourceFailed);
	EXPECT_NE(detail.find("armed failure"), std::string::npos);
	EXPECT_FALSE(fs::exists(lease->FilesDirectory() / std::to_string(exe)));
}

TEST_F(ArchiveCacheTest, ASequentialSourceIsServedByTheOnePassOnly)
{
	const auto archive = Built("doom.tar.gz", Kind::TarGz, Game());
	auto source        = Source(archive);
	ASSERT_EQ(source->GetAccess(), Access::Sequential);
	auto cache     = OpenOk();
	auto lease     = AcquireOk(*cache, *source, archive);
	const auto exe = Ordinal(*source, "game/doom.exe");
	fs::path path  = {};
	auto detail    = std::string();
	// A short file in the entry is removed even when nothing rewrites it.
	Touch(lease->FilesDirectory() / std::to_string(exe), "short");
	EXPECT_EQ(lease->MemberFile(exe, path, &detail), CacheStatus::Failed);
	EXPECT_FALSE(fs::exists(lease->FilesDirectory() / std::to_string(exe)));
	EXPECT_EQ(source->to_file_calls, 0);
	ASSERT_EQ(lease->EnsureAll(&detail), CacheStatus::Ok) << detail;
	EXPECT_EQ(source->all_calls, 1);
	EXPECT_TRUE(lease->IsComplete());
	ASSERT_EQ(lease->MemberFile(exe, path, &detail), CacheStatus::Ok) << detail;
	EXPECT_EQ(SizeOf(path), 3000);
	EXPECT_EQ(source->to_file_calls, 0);
	// Complete: a second EnsureAll runs no pass.
	ASSERT_EQ(lease->EnsureAll(&detail), CacheStatus::Ok) << detail;
	EXPECT_EQ(source->all_calls, 1);
	EXPECT_EQ(cache->Measure().bytes, 8005);
}

TEST_F(ArchiveCacheTest, TheFreeSpaceReserveRefusesBeforeAnyByteIsWritten)
{
	const auto archive     = Built("doom.zip", Kind::ZipStored, Game());
	auto source            = Source(archive);
	options.min_free_bytes = 10000;
	auto cache             = OpenOk();
	auto lease             = AcquireOk(*cache, *source, archive);
	const auto wad         = Ordinal(*source, "game/data/level.wad");
	fs::path path          = {};
	auto detail            = std::string();
	free_space             = 10000 + 5000 - 1;
	EXPECT_EQ(lease->MemberFile(wad, path, &detail), CacheStatus::NoSpace);
	EXPECT_EQ(source->to_file_calls, 0);
	free_space = 10000 + 5000;
	EXPECT_EQ(lease->MemberFile(wad, path, &detail), CacheStatus::Ok) << detail;
	// The one pass is judged by the declared total.
	auto tar    = Source(Built("doom.tar", Kind::Tar, Game()));
	auto lease2 = AcquireOk(*cache, *tar, tmp / "doom.tar");
	free_space  = 10000 + 8005 - 1;
	EXPECT_EQ(lease2->EnsureAll(&detail), CacheStatus::NoSpace);
	EXPECT_EQ(tar->all_calls, 0);
	// An unknown free space counts as none.
	options.free_space = [](const fs::path&) {
		return std::optional<int64_t>{};
	};
	auto cache2 = OpenOk();
	auto lease3 = AcquireOk(*cache2, *tar, tmp / "doom.tar");
	EXPECT_EQ(lease3->EnsureAll(&detail), CacheStatus::NoSpace);
}

TEST_F(ArchiveCacheTest, TheCapEvictsAnUnlockedEntryAndRefusesWhenItCannot)
{
	options.max_size_bytes = 6000;
	auto cache             = OpenOk();
	const auto a           = Built("a.zip", Kind::ZipStored, Game());
	const auto b           = Built("b.zip",
                             Kind::ZipStored,
	                               {
                                     {"big.bin", std::string(5500, 'b')}
        });
	auto src_a             = Source(a);
	auto src_b             = Source(b);
	fs::path path          = {};
	auto detail            = std::string();
	{
		auto lease_a = AcquireOk(*cache, *src_a, a);
		ASSERT_EQ(lease_a->MemberFile(Ordinal(*src_a, "game/data/level.wad"),
		                              path,
		                              &detail),
		          CacheStatus::Ok)
		        << detail;
	}
	now += 10;
	auto lease_b = AcquireOk(*cache, *src_b, b);
	// 5000 held by a, 5500 wanted: a is evicted, since nobody holds it.
	ASSERT_EQ(lease_b->MemberFile(Ordinal(*src_b, "big.bin"), path, &detail),
	          CacheStatus::Ok)
	        << detail;
	EXPECT_EQ(cache->Measure().entries, 1);
	EXPECT_EQ(cache->Measure().bytes, 5500);
	// Now a wants 3000 while b is held: over the cap, refused.
	auto lease_a = AcquireOk(*cache, *src_a, a);
	EXPECT_EQ(lease_a->MemberFile(Ordinal(*src_a, "game/doom.exe"), path, &detail),
	          CacheStatus::NoSpace);
	EXPECT_NE(detail.find("cap"), std::string::npos);
}

TEST_F(ArchiveCacheTest, CleanupRemovesExpiredAndDamagedEntriesAndSkipsLockedOnes)
{
	options.ttl_seconds = 1000;
	auto cache          = OpenOk();
	const auto a        = Built("a.zip", Kind::ZipStored, Game());
	const auto b        = Built("b.zip",
                             Kind::ZipStored,
	                            {
                                     {"x.txt", "x"}
        });
	auto src_a          = Source(a);
	auto src_b          = Source(b);
	fs::path path       = {};
	auto detail         = std::string();
	{
		auto lease = AcquireOk(*cache, *src_a, a);
		ASSERT_EQ(lease->MemberFile(Ordinal(*src_a, "game/readme.txt"),
		                            path,
		                            &detail),
		          CacheStatus::Ok);
	}
	const auto dir_a = ArchiveCache::EntryDirectory(cache_dir,
	                                                "a",
	                                                src_a->IdentityHash());
	// A damaged entry: a directory under a letter with files but no toml.
	const auto damaged = cache_dir / "z" / "zombie-00000000";
	fs::create_directories(damaged / FilesDirName);
	Touch(damaged / FilesDirName / "0", "zzz");
	// A temporary inside a's files is a regular file like any other.
	Touch(dir_a / FilesDirName / "7.deadbeef.part", "half");
	auto lease_b = AcquireOk(*cache, *src_b, b);
	now += 1001;
	const auto report = cache->Cleanup();
	EXPECT_EQ(report.removed_expired, 1);
	EXPECT_EQ(report.removed_damaged, 1);
	EXPECT_EQ(report.skipped_locked, 1);
	EXPECT_FALSE(fs::exists(dir_a));
	EXPECT_FALSE(fs::exists(damaged));
	EXPECT_TRUE(fs::exists(lease_b->Directory()));
	EXPECT_EQ(cache->Measure().entries, 1);
	// b, unlocked and unused for longer than the TTL, goes next time.
	lease_b.reset();
	now += 1001;
	EXPECT_EQ(cache->Cleanup().removed_expired, 1);
	EXPECT_EQ(cache->Measure().entries, 0);
	EXPECT_TRUE(fs::is_regular_file(cache_dir / CacheTagName));
}

#if !defined(WIN32)
TEST_F(ArchiveCacheTest, CleanupLeavesAnEntryHoldingSomethingForeign)
{
	options.ttl_seconds = 1;
	auto cache          = OpenOk();
	const auto a        = Built("a.zip", Kind::ZipStored, Game());
	auto src_a          = Source(a);
	{
		auto lease = AcquireOk(*cache, *src_a, a);
	}
	const auto dir_a = ArchiveCache::EntryDirectory(cache_dir,
	                                                "a",
	                                                src_a->IdentityHash());
	fs::create_symlink(tmp / "a.zip", dir_a / FilesDirName / "link");
	now += 10;
	const auto report = cache->Cleanup();
	EXPECT_EQ(report.removed_expired, 0);
	EXPECT_TRUE(fs::is_symlink(dir_a / FilesDirName / "link"));
	EXPECT_TRUE(fs::exists(tmp / "a.zip"));
}
#endif

TEST_F(ArchiveCacheTest, AChangedIdentityStartsTheEntryOver)
{
	const auto archive = Built("doom.zip", Kind::ZipStored, Game());
	auto source        = Source(archive);
	auto cache         = OpenOk();
	fs::path path      = {};
	auto detail        = std::string();
	{
		auto lease = AcquireOk(*cache, *source, archive);
		ASSERT_EQ(lease->MemberFile(Ordinal(*source, "game/doom.exe"),
		                            path,
		                            &detail),
		          CacheStatus::Ok);
	}
	// The same key with another full hash: a hash8 collision or a
	// rewritten toml. The recorded files cannot be trusted.
	const auto dir = ArchiveCache::EntryDirectory(cache_dir,
	                                              "doom",
	                                              source->IdentityHash());
	auto error     = std::string();
	auto doc       = ArchiveToml::ReadFile(dir / EntryTomlName, &error);
	ASSERT_TRUE(doc) << error;
	doc->root.Set("archive_mtime", int64_t{1});
	ASSERT_TRUE(ArchiveToml::WriteAtomically(dir / EntryTomlName, *doc, &error))
	        << error;
	auto lease = AcquireOk(*cache, *source, archive);
	EXPECT_FALSE(lease->WasReused());
	EXPECT_FALSE(fs::exists(dir / FilesDirName /
	                        std::to_string(Ordinal(*source, "game/doom.exe"))));
	EXPECT_EQ(cache->Measure().bytes, 0);
}

TEST_F(ArchiveCacheTest, AnEntryAcquiredWhileCleanupRemovesItKeepsItsFiles)
{
	// Two engines on one store: the first's Cleanup has decided to remove
	// an expired entry, and the second acquires that archive in the gap.
	options.ttl_seconds = 1000;
	const auto archive  = Built("doom.zip", Kind::ZipStored, Game());
	auto source         = Source(archive);
	auto other_source   = Source(archive);
	const auto ordinal  = Ordinal(*source, "game/readme.txt");
	std::unique_ptr<ArchiveCache> second             = {};
	std::unique_ptr<ArchiveCache::Lease> other_lease = {};
	auto status_in_gap                               = CacheStatus::Ok;
	bool fired                                       = false;
	options.before_remove = [&](const fs::path&) {
		if (fired || !second) {
			return;
		}
		fired         = true;
		auto got      = second->Acquire(*other_source, archive);
		status_in_gap = got.status;
		other_lease   = std::move(got.lease);
	};
	auto first    = OpenOk();
	fs::path path = {};
	auto detail   = std::string();
	{
		auto lease = AcquireOk(*first, *source, archive);
		ASSERT_EQ(lease->MemberFile(ordinal, path, &detail), CacheStatus::Ok);
	}
	second = OpenOk();
	now += 1001;
	first->Cleanup();
	ASSERT_TRUE(fired);
	if (!other_lease) {
		EXPECT_EQ(status_in_gap, CacheStatus::Busy);
		return;
	}
	EXPECT_TRUE(fs::is_regular_file(other_lease->Directory() / EntryTomlName));
	EXPECT_TRUE(fs::is_regular_file(other_lease->Directory() / LockName));
	EXPECT_EQ(other_lease->MemberFile(ordinal, path, &detail), CacheStatus::Ok)
	        << detail;
	EXPECT_TRUE(fs::is_regular_file(path));
}

TEST_F(ArchiveCacheTest, CleanupSweepsTrashAndScratchNobodyHolds)
{
	auto cache = OpenOk();
	auto live  = cache->MakeScratch();
	ASSERT_TRUE(live);
	Touch(live->dir / "m1-1", "x");
	auto dead = cache->MakeScratch();
	ASSERT_TRUE(dead);
	Touch(dead->dir / "m2-2", "y");
	const auto dead_dir = dead->dir;
	dead.reset(); // its mount is gone without removing it
	// A crash between creating a scratch and locking it, two hours ago
	// and a moment ago on the store's clock.
	const auto lockless_old = cache_dir / ScratchDirName / "0000000000000000";
	const auto lockless_new = cache_dir / ScratchDirName / "1111111111111111";
	fs::create_directories(lockless_old);
	fs::create_directories(lockless_new);
	const auto age = std::chrono::seconds(
	        std::chrono::duration_cast<std::chrono::seconds>(
	                std::chrono::system_clock::now().time_since_epoch())
	                .count() -
	        (now - 7200));
	fs::last_write_time(lockless_old, fs::file_time_type::clock::now() - age);
	fs::last_write_time(lockless_new,
	                    fs::file_time_type::clock::now() - age +
	                            std::chrono::seconds(7200));
	// An entry retired by an engine that died before removing it.
	const auto trash = cache_dir / TrashDirName / "doom-12345678.abcdef01";
	fs::create_directories(trash / FilesDirName);
	Touch(trash / FilesDirName / "0", "z");

	cache->Cleanup();
	EXPECT_TRUE(fs::is_regular_file(live->dir / "m1-1"));
	EXPECT_FALSE(fs::exists(dead_dir));
	EXPECT_FALSE(fs::exists(lockless_old));
	EXPECT_TRUE(fs::exists(lockless_new));
	EXPECT_FALSE(fs::exists(trash));
}

// A union writes directories into its scratch (Phase 6.4); the sweep
// removes the whole tree, not only the files at the top.
TEST_F(ArchiveCacheTest, CleanupSweepsAScratchWithDirectories)
{
	auto cache   = OpenOk();
	auto scratch = cache->MakeScratch();
	ASSERT_TRUE(scratch);
	fs::create_directories(scratch->dir / "SAVES" / "DEEP");
	Touch(scratch->dir / "SAVES" / "DEEP" / "GAME.SAV", "x");
	Touch(scratch->dir / "TOP.TXT", "y");
	const auto dir = scratch->dir;
	scratch.reset(); // the mount is gone, the lock is free
	cache->Cleanup();
	EXPECT_FALSE(fs::exists(dir));
}

TEST_F(ArchiveCacheTest, CleanupKeepsAScratchHoldingALink)
{
	auto cache   = OpenOk();
	auto scratch = cache->MakeScratch();
	ASSERT_TRUE(scratch);
	fs::create_directories(tmp / "outside");
	Touch(tmp / "outside" / "KEEP.TXT", "z");
	fs::create_directory_symlink(tmp / "outside", scratch->dir / "LINK");
	const auto dir = scratch->dir;
	scratch.reset();
	cache->Cleanup();
	EXPECT_TRUE(fs::exists(dir / "LINK"));
	EXPECT_TRUE(fs::exists(tmp / "outside" / "KEEP.TXT"));
}

TEST_F(ArchiveCacheTest, DiscardScratchRemovesTheTreeAndReleasesTheLock)
{
	auto cache   = OpenOk();
	auto scratch = cache->MakeScratch();
	ASSERT_TRUE(scratch);
	fs::create_directories(scratch->dir / "A" / "B");
	Touch(scratch->dir / "A" / "B" / "F.DAT", "f");
	const auto dir = scratch->dir;
	ArchiveCache::DiscardScratch(*scratch);
	EXPECT_FALSE(fs::exists(dir));
	EXPECT_FALSE(scratch->lock);
	EXPECT_TRUE(scratch->dir.empty());
}

TEST_F(ArchiveCacheTest, AcquireIsBusyWhileCleanupHoldsTheEntryAndSharedBetweenTwoMounts)
{
	const auto archive = Built("doom.zip", Kind::ZipStored, Game());
	auto source        = Source(archive);
	auto cache         = OpenOk();
	const auto dir     = ArchiveCache::EntryDirectory(cache_dir,
                                                      "doom",
                                                      source->IdentityHash());
	fs::create_directories(dir);
	{
		auto exclusive = FileLock::TryAcquire(dir / LockName,
		                                      FileLock::Mode::Exclusive);
		ASSERT_TRUE(exclusive);
		EXPECT_EQ(cache->Acquire(*source, archive).status, CacheStatus::Busy);
	}
	auto first  = AcquireOk(*cache, *source, archive);
	auto second = AcquireOk(*cache, *source, archive);
	EXPECT_TRUE(first);
	EXPECT_TRUE(second);
}

TEST_F(ArchiveCacheTest, ALeaseOutlivesItsStore)
{
	// At engine exit the drive table and the cache store are statics
	// in different units, destroyed in no fixed order: a lease's
	// destructor must need nothing from the store.
	const auto archive = Built("doom.zip", Kind::ZipStored, Game());
	auto source        = Source(archive);
	auto cache         = OpenOk();
	auto lease         = AcquireOk(*cache, *source, archive);
	const auto dir     = lease->Directory();
	now += 50;
	cache.reset();
	lease.reset();
	auto error     = std::string();
	const auto doc = ArchiveToml::ReadFile(dir / EntryTomlName, &error);
	ASSERT_TRUE(doc) << error;
	EXPECT_EQ(doc->root.Integer("last_use"), now);
}

TEST_F(ArchiveCacheTest, StatusTextsAreDistinct)
{
	EXPECT_STRNE(CacheStatusText(CacheStatus::Ok),
	             CacheStatusText(CacheStatus::NoSpace));
	EXPECT_STRNE(CacheStatusText(CacheStatus::Busy),
	             CacheStatusText(CacheStatus::Refused));
	EXPECT_STRNE(CacheStatusText(CacheStatus::Failed),
	             CacheStatusText(CacheStatus::SourceFailed));
}

TEST_F(ArchiveCacheTest, ALeaseCanBeReboundToAnEqualSource)
{
	const auto archive = Built("doom.zip", Kind::ZipStored, Game());
	auto first         = Source(archive);
	auto second        = Source(archive);
	auto cache         = OpenOk();
	auto lease         = AcquireOk(*cache, *first, archive);
	fs::path path      = {};
	auto detail        = std::string();
	ASSERT_EQ(lease->MemberFile(Ordinal(*first, "game/doom.exe"), path, &detail),
	          CacheStatus::Ok)
	        << detail;
	EXPECT_TRUE(lease->Rebind(*second));
	first.reset();
	// The rebound lease serves through the second source.
	ASSERT_EQ(lease->MemberFile(Ordinal(*second, "game/data/level.wad"), path, &detail),
	          CacheStatus::Ok)
	        << detail;
	EXPECT_EQ(second->to_file_calls, 1);
	// A source with another identity is refused and the lease keeps the old
	// one.
	auto other = Source(Built("other.zip",
	                          Kind::ZipStored,
	                          {
	                                  {"x.txt", "x"}
        }));
	EXPECT_FALSE(lease->Rebind(*other));
	ASSERT_EQ(lease->MemberFile(Ordinal(*second, "game/readme.txt"), path, &detail),
	          CacheStatus::Ok)
	        << detail;
	EXPECT_EQ(second->to_file_calls, 2);
}

} // namespace
