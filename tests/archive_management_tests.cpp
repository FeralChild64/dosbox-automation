// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//
#include "dos/archive/archive_identity.h"
#include "dos/archive/archive_management.h"

#include "archive_fixtures.h"
#include "dos/archive/archive_cache.h"
#include "dos/archive/archive_reader.h"
#include "dos/archive/member_path.h"
#include "dos/archive/sidecar_locator.h"
#include "dos/archive/sidecar_record.h"
#include "test_temp_dir.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace ArchiveMount;
using ArchiveFixtures::FileEntry;
using ArchiveFixtures::Kind;
using ArchiveFixtures::WriteArchive;

namespace {

namespace fs = std::filesystem;

class ArchiveManagementTest : public ::testing::Test {
protected:
	fs::path tmp         = {};
	Limits limits        = {};
	CacheOptions options = {};
	int64_t now          = 1'700'000'000;
	int64_t free_space   = 100ll << 30;

	// A lease kept alive on purpose, with the source it refers to.
	std::unique_ptr<ArchiveReader> held_reader      = {};
	std::unique_ptr<ArchiveCache::Lease> held_lease = {};
	std::unique_ptr<ArchiveCache> held_cache        = {};

	void SetUp() override
	{
		tmp = TestTempDir::MakeUnique("archive_management_");
		ASSERT_FALSE(tmp.empty());
		options.free_space = [this](const fs::path&) {
			return std::optional<int64_t>(free_space);
		};
		options.now = [this]() { return now; };
	}
	void TearDown() override
	{
		held_lease.reset();
		held_reader.reset();
		held_cache.reset();
		std::error_code ec = {};
		fs::remove_all(tmp, ec);
	}
	fs::path Built(const std::string& name)
	{
		const auto path = tmp / name;
		auto error      = std::string();
		EXPECT_TRUE(WriteArchive(path,
		                         Kind::ZipDeflate,
		                         {
		                                 {"game/", "", true},
		                                 {"game/doom.exe",
		                                  std::string(3000, 'x')},
		                                 {"game/readme.txt", "hello\r\n"}
                },
		                         nullptr,
		                         &error))
		        << error;
		return path;
	}
	std::unique_ptr<ArchiveReader> Opened(const fs::path& path)
	{
		auto opened = ArchiveReader::Open(path, limits);
		EXPECT_EQ(opened.error, Error::None) << opened.detail;
		return std::move(opened.reader);
	}
	std::string HashOf(const fs::path& path)
	{
		return Opened(path)->IdentityHash();
	}
	// The command line identifies through the helper; in-process here.
	// A chain below the host is answered by LeafFor, which a test sets.
	ArchiveManagement::IdentifyArchive Identify()
	{
		return [this](const fs::path& path,
		              const std::string& chain) -> std::optional<std::string> {
			++identified;
			if (!chain.empty()) {
				return leaf_for.count(chain)
				             ? std::optional(leaf_for.at(chain))
				             : std::nullopt;
			}
			auto opened = ArchiveReader::Open(path, limits);
			if (!opened.reader) {
				return {};
			}
			return opened.reader->IdentityHash();
		};
	}
	std::map<std::string, std::string> leaf_for = {};
	int identified                              = 0;
	static int FirstFileOrdinal(const ArchiveSource& source)
	{
		for (const auto& e : source.Entries()) {
			if (!e.is_dir && e.ordinal >= 0) {
				return e.ordinal;
			}
		}
		return -1;
	}
	// An entry with one materialized member, its lease dropped.
	void MakeCacheEntry(const fs::path& archive)
	{
		auto cache = ArchiveCache::Open(tmp / "cache", options);
		ASSERT_EQ(cache.status, CacheStatus::Ok) << cache.detail;
		auto reader = Opened(archive);
		auto got    = cache.cache->Acquire(*reader, archive);
		ASSERT_EQ(got.status, CacheStatus::Ok) << got.detail;
		auto path   = fs::path();
		auto detail = std::string();
		ASSERT_EQ(got.lease->MemberFile(FirstFileOrdinal(*reader), path, &detail),
		          CacheStatus::Ok)
		        << detail;
	}
	// The same, but the lease stays alive until TearDown.
	void HoldCacheEntry(const fs::path& archive)
	{
		auto cache = ArchiveCache::Open(tmp / "cache", options);
		ASSERT_EQ(cache.status, CacheStatus::Ok) << cache.detail;
		held_cache  = std::move(cache.cache);
		held_reader = Opened(archive);
		auto got    = held_cache->Acquire(*held_reader, archive);
		ASSERT_EQ(got.status, CacheStatus::Ok) << got.detail;
		auto path   = fs::path();
		auto detail = std::string();
		ASSERT_EQ(got.lease->MemberFile(FirstFileOrdinal(*held_reader),
		                                path,
		                                &detail),
		          CacheStatus::Ok)
		        << detail;
		held_lease = std::move(got.lease);
	}
	fs::path MakeSidecar(const fs::path& archive, const std::string& hash)
	{
		const auto dir = tmp / "saves" / SidecarKey(archive, hash);
		fs::create_directories(dir);
		SidecarRecord record = {};
		record.archive_path  = PathToUtf8(archive);
		record.identity_hash = hash;
		record.entry_count   = 2;
		record.total_bytes   = 3007;
		record.created       = now;
		auto error           = std::string();
		EXPECT_TRUE(record.Save(dir / SidecarTomlName, &error)) << error;
		std::ofstream(dir / "SAVE.DAT", std::ios::binary)
		        << std::string(512, 's');
		return dir;
	}
};

TEST_F(ArchiveManagementTest, InspectReportsAMissingCacheWithoutCreatingIt)
{
	const auto report = ArchiveManagement::InspectCache(tmp / "none", options);
	EXPECT_FALSE(report.exists);
	EXPECT_TRUE(report.entries.empty());
	EXPECT_TRUE(report.problem.empty()) << report.problem;
	EXPECT_FALSE(fs::exists(tmp / "none"));
}

TEST_F(ArchiveManagementTest, InspectRefusesAFileInPlaceOfTheCache)
{
	std::ofstream(tmp / "file") << "x";
	const auto report = ArchiveManagement::InspectCache(tmp / "file", options);
	EXPECT_FALSE(report.exists);
	EXPECT_FALSE(report.problem.empty());
}

TEST_F(ArchiveManagementTest, InspectListsEntriesWithTheirArchiveAndLastUse)
{
	const auto zip = Built("game.zip");
	MakeCacheEntry(zip);
	const auto report = ArchiveManagement::InspectCache(tmp / "cache", options);
	ASSERT_TRUE(report.exists) << report.problem;
	ASSERT_EQ(report.entries.size(), 1u);
	EXPECT_EQ(report.entries[0].archive_path, PathToUtf8(fs::canonical(zip)));
	EXPECT_EQ(report.entries[0].identity_hash, HashOf(zip));
	EXPECT_EQ(report.entries[0].bytes, 3000);
	EXPECT_EQ(report.entries[0].last_use, now);
	EXPECT_FALSE(report.entries[0].locked);
	EXPECT_EQ(report.bytes, 3000);
	EXPECT_EQ(report.cap_bytes, options.max_size_bytes);
	EXPECT_EQ(report.min_free_bytes, options.min_free_bytes);
	EXPECT_EQ(report.ttl_seconds, options.ttl_seconds);
	ASSERT_TRUE(report.free_bytes);
	EXPECT_EQ(*report.free_bytes, free_space);
}

TEST_F(ArchiveManagementTest, InspectReportsACacheItCannotRead)
{
	MakeCacheEntry(Built("game.zip"));
	fs::permissions(tmp / "cache", fs::perms::none);
	std::error_code probe = {};
	fs::directory_iterator(tmp / "cache", probe);
	if (!probe) {
		fs::permissions(tmp / "cache", fs::perms::owner_all);
		GTEST_SKIP() << "a directory without permissions is still listable here";
	}
	const auto report = ArchiveManagement::InspectCache(tmp / "cache", options);
	fs::permissions(tmp / "cache", fs::perms::owner_all);
	EXPECT_FALSE(report.problem.empty());
	EXPECT_TRUE(report.entries.empty());
}

TEST_F(ArchiveManagementTest, InspectCreatesNoLockFile)
{
	MakeCacheEntry(Built("game.zip"));
	auto entry = fs::path();
	for (const auto& letter : fs::directory_iterator(tmp / "cache")) {
		if (letter.is_directory() &&
		    letter.path().filename().string().size() == 1) {
			for (const auto& e : fs::directory_iterator(letter.path())) {
				entry = e.path();
			}
		}
	}
	ASSERT_FALSE(entry.empty());
	ASSERT_TRUE(fs::remove(entry / LockName));
	const auto report = ArchiveManagement::InspectCache(tmp / "cache", options);
	ASSERT_EQ(report.entries.size(), 1u);
	EXPECT_FALSE(report.entries[0].locked);
	EXPECT_FALSE(fs::exists(entry / LockName));
}

TEST_F(ArchiveManagementTest, InspectDoesNotCountALinkPlantedUnderFiles)
{
	const auto zip = Built("game.zip");
	MakeCacheEntry(zip);
	std::ofstream(tmp / "huge", std::ios::binary) << std::string(100000, 'h');
	auto entry = fs::path();
	for (const auto& letter : fs::directory_iterator(tmp / "cache")) {
		if (letter.is_directory() &&
		    letter.path().filename().string().size() == 1) {
			for (const auto& e : fs::directory_iterator(letter.path())) {
				entry = e.path();
			}
		}
	}
	ASSERT_FALSE(entry.empty());
	fs::create_symlink(tmp / "huge", entry / FilesDirName / "9");
	const auto report = ArchiveManagement::InspectCache(tmp / "cache", options);
	ASSERT_EQ(report.entries.size(), 1u);
	EXPECT_EQ(report.entries[0].bytes, 3000);
}

TEST_F(ArchiveManagementTest, EmptyRemovesEveryUnheldEntryAndKeepsAHeldOne)
{
	MakeCacheEntry(Built("game.zip"));
	HoldCacheEntry(Built("other.zip"));
	const auto report = ArchiveManagement::EmptyCache(tmp / "cache", options);
	ASSERT_TRUE(report.ok) << report.problem;
	EXPECT_TRUE(report.cache_exists);
	EXPECT_EQ(report.removed, 1);
	EXPECT_EQ(report.skipped_locked, 1);
	const auto after = ArchiveManagement::InspectCache(tmp / "cache", options);
	ASSERT_EQ(after.entries.size(), 1u);
	EXPECT_TRUE(after.entries[0].locked);
	EXPECT_EQ(report.bytes_after, after.bytes);
}

// A clock stepped back, or a cache another host wrote, leaves last_use in
// the future; empty removes every unheld entry whatever the stamps say.
TEST_F(ArchiveManagementTest, EmptyRemovesAnEntryStampedInTheFuture)
{
	MakeCacheEntry(Built("game.zip"));
	now -= 3600;
	const auto report = ArchiveManagement::EmptyCache(tmp / "cache", options);
	ASSERT_TRUE(report.ok) << report.problem;
	EXPECT_EQ(report.removed, 1);
	EXPECT_TRUE(
	        ArchiveManagement::InspectCache(tmp / "cache", options).entries.empty());
}

// Decision 1b (2026-10-08): no cache yet is nothing to do, exit 0 like
// status; cache_exists tells the command line which line to print.
TEST_F(ArchiveManagementTest, EmptyOfAMissingCacheIsNothingToDo)
{
	const auto report = ArchiveManagement::EmptyCache(tmp / "none", options);
	EXPECT_TRUE(report.ok) << report.problem;
	EXPECT_FALSE(report.cache_exists);
	EXPECT_EQ(report.removed, 0);
	EXPECT_FALSE(fs::exists(tmp / "none"));
}

// The engine follows a linked saves root (create_directories and the
// locator's Scan go through the link), so the list does too.
TEST_F(ArchiveManagementTest, ListSidecarsFollowsALinkedSavesDirectory)
{
	const auto zip = Built("game.zip");
	MakeSidecar(zip, HashOf(zip));
	fs::create_directory_symlink(tmp / "saves", tmp / "link");
	auto problem      = std::string();
	const auto listed = ArchiveManagement::ListSidecars(tmp / "link",
	                                                    Identify(),
	                                                    &problem);
	EXPECT_TRUE(problem.empty()) << problem;
	ASSERT_EQ(listed.size(), 1u);
	EXPECT_EQ(listed[0].state, ArchiveManagement::ArchiveState::Same);
}

// A root that cannot be stat'ed (a parent without search permission) is
// a problem, not an empty list or "no cache yet".
TEST_F(ArchiveManagementTest, ListSidecarsReportsASavesRootItCannotStat)
{
	fs::create_directories(tmp / "locked" / "saves");
	fs::permissions(tmp / "locked", fs::perms::none);
	std::error_code probe = {};
	[[maybe_unused]] const auto probed = fs::symlink_status(tmp / "locked" / "saves",
	                                                        probe);
	if (!probe) {
		fs::permissions(tmp / "locked", fs::perms::owner_all);
		GTEST_SKIP() << "a directory without permissions is still searchable here";
	}
	auto problem = std::string();
	const auto listed = ArchiveManagement::ListSidecars(tmp / "locked" / "saves",
	                                                    Identify(),
	                                                    &problem);
	fs::permissions(tmp / "locked", fs::perms::owner_all);
	EXPECT_TRUE(listed.empty());
	EXPECT_FALSE(problem.empty());
}

TEST_F(ArchiveManagementTest, InspectReportsACacheRootItCannotStat)
{
	fs::create_directories(tmp / "locked" / "cache");
	fs::permissions(tmp / "locked", fs::perms::none);
	std::error_code probe = {};
	[[maybe_unused]] const auto probed = fs::symlink_status(tmp / "locked" / "cache",
	                                                        probe);
	if (!probe) {
		fs::permissions(tmp / "locked", fs::perms::owner_all);
		GTEST_SKIP() << "a directory without permissions is still searchable here";
	}
	const auto overview = ArchiveManagement::InspectCache(tmp / "locked" / "cache",
	                                                      options);
	fs::permissions(tmp / "locked", fs::perms::owner_all);
	EXPECT_FALSE(overview.exists);
	EXPECT_FALSE(overview.problem.empty());
}

// An entry directory without a readable record still holds the bytes the
// cap counts (the cache's Measure walks files/), so the total shows them.
TEST_F(ArchiveManagementTest, InspectCountsAnEntryWithoutARecordAsDamaged)
{
	const auto zip = Built("game.zip");
	MakeCacheEntry(zip);
	auto overview = ArchiveManagement::InspectCache(tmp / "cache", options);
	ASSERT_EQ(overview.entries.size(), 1u);
	EXPECT_FALSE(overview.entries[0].damaged);
	const auto entry = overview.entries[0].directory;
	const auto bytes = overview.entries[0].bytes;
	ASSERT_GT(bytes, 0);
	fs::remove(entry / EntryTomlName);
	overview = ArchiveManagement::InspectCache(tmp / "cache", options);
	ASSERT_EQ(overview.entries.size(), 1u);
	EXPECT_TRUE(overview.entries[0].damaged);
	EXPECT_EQ(overview.entries[0].directory, entry);
	EXPECT_EQ(overview.entries[0].bytes, bytes);
	EXPECT_EQ(overview.bytes, bytes);
	EXPECT_TRUE(overview.problem.empty()) << overview.problem;
}

// The cleanup retires an entry whose record has the wrong version or no
// last_use as damaged; the overview must call it the same.
TEST_F(ArchiveManagementTest, InspectCountsARecordOfTheWrongVersionAsDamaged)
{
	const auto zip = Built("game.zip");
	MakeCacheEntry(zip);
	auto overview = ArchiveManagement::InspectCache(tmp / "cache", options);
	ASSERT_EQ(overview.entries.size(), 1u);
	const auto entry = overview.entries[0].directory;
	std::ofstream(entry / EntryTomlName) << "version = 99\nlast_use = 5\n";
	overview = ArchiveManagement::InspectCache(tmp / "cache", options);
	ASSERT_EQ(overview.entries.size(), 1u);
	EXPECT_TRUE(overview.entries[0].damaged);
	std::ofstream(entry / EntryTomlName) << "version = 1\n";
	overview = ArchiveManagement::InspectCache(tmp / "cache", options);
	ASSERT_EQ(overview.entries.size(), 1u);
	EXPECT_TRUE(overview.entries[0].damaged);
}

// Every letter directory that cannot be read is named, not only the last.
TEST_F(ArchiveManagementTest, InspectNamesEveryLetterDirectoryItCannotRead)
{
	const auto zip = Built("game.zip");
	MakeCacheEntry(zip);
	const auto x = tmp / "cache" / "x";
	const auto y = tmp / "cache" / "y";
	for (const auto& letter : {x, y}) {
		fs::create_directories(letter);
		fs::permissions(letter, fs::perms::none);
	}
	std::error_code probe = {};
	fs::directory_iterator(x, probe);
	if (!probe) {
		for (const auto& letter : {x, y}) {
			fs::permissions(letter, fs::perms::owner_all);
		}
		GTEST_SKIP() << "a directory without permissions is still listable here";
	}
	const auto overview = ArchiveManagement::InspectCache(tmp / "cache", options);
	for (const auto& letter : {x, y}) {
		fs::permissions(letter, fs::perms::owner_all);
	}
	EXPECT_NE(overview.problem.find(PathToUtf8(x)), std::string::npos)
	        << overview.problem;
	EXPECT_NE(overview.problem.find(PathToUtf8(y)), std::string::npos)
	        << overview.problem;
	EXPECT_EQ(overview.entries.size(), 1u);
}

TEST_F(ArchiveManagementTest, ListSidecarsTellsSameChangedAndMissing)
{
	const auto zip     = Built("game.zip");
	const auto same    = MakeSidecar(zip, HashOf(zip));
	const auto changed = MakeSidecar(zip, std::string(64, 'f'));
	const auto missing = MakeSidecar(tmp / "gone.zip", HashOf(zip));
	auto listed = ArchiveManagement::ListSidecars(tmp / "saves", Identify());
	ASSERT_EQ(listed.size(), 3u);
	auto by_dir = std::map<fs::path, ArchiveManagement::SidecarInfo>{};
	for (const auto& s : listed) {
		by_dir[s.directory] = s;
	}
	ASSERT_TRUE(by_dir.count(same) && by_dir.count(changed) &&
	            by_dir.count(missing));
	EXPECT_EQ(by_dir[same].state, ArchiveManagement::ArchiveState::Same);
	EXPECT_EQ(by_dir[changed].state, ArchiveManagement::ArchiveState::Changed);
	EXPECT_EQ(by_dir[missing].state, ArchiveManagement::ArchiveState::Missing);
	EXPECT_EQ(by_dir[same].archive_path, PathToUtf8(zip));
	EXPECT_EQ(by_dir[same].identity_hash, HashOf(zip));
	EXPECT_EQ(by_dir[same].bytes,
	          512 + static_cast<int64_t>(fs::file_size(same / SidecarTomlName)));
	EXPECT_EQ(by_dir[same].created, now);
	EXPECT_EQ(identified, 2); // the missing archive is never identified
}

TEST_F(ArchiveManagementTest, ListSidecarsIdentifiesANestedRecordThroughItsHost)
{
	// N5: a nested record names the host and the chain and carries the
	// leaf's own identity; identify is asked for the chain below the host.
	const auto zip        = Built("game.zip");
	const auto leaf       = std::string(64, 'b');
	leaf_for["inner.zip"] = leaf;
	const auto nested     = MakeSidecar(zip / "inner.zip", leaf);
	const auto stale      = MakeSidecar(zip / "other.zip", HashOf(zip));
	auto listed = ArchiveManagement::ListSidecars(tmp / "saves", Identify());
	ASSERT_EQ(listed.size(), 2u);
	auto by_dir = std::map<fs::path, ArchiveManagement::SidecarInfo>{};
	for (const auto& s : listed) {
		by_dir[s.directory] = s;
	}
	ASSERT_TRUE(by_dir.count(nested) && by_dir.count(stale));
	EXPECT_EQ(by_dir[nested].state, ArchiveManagement::ArchiveState::Same);
	// other.zip is not a chain identify knows: unreadable, not changed.
	EXPECT_EQ(by_dir[stale].state, ArchiveManagement::ArchiveState::Unreadable);
}

TEST_F(ArchiveManagementTest, AHouseRecordIsSameWhenTheHostEntersToItsIdentity)
{
	// The record names the host file and carries the leaf's identity;
	// identify is asked with an empty chain and answers for the entered
	// archive (N3 and N5).
	const auto zip  = Built("house.zip");
	const auto leaf = std::string(64, 'c');
	const auto dir  = MakeSidecar(zip, leaf);
	auto calls      = std::vector<std::string>{};
	const auto identify =
	        [&](const fs::path& host,
	            const std::string& chain) -> std::optional<std::string> {
		calls.push_back(chain);
		return chain.empty() && host == zip ? std::optional(leaf)
		                                    : std::nullopt;
	};
	const auto listed = ArchiveManagement::ListSidecars(tmp / "saves", identify);
	ASSERT_EQ(listed.size(), 1u);
	EXPECT_EQ(listed[0].directory, dir);
	EXPECT_EQ(listed[0].state, ArchiveManagement::ArchiveState::Same);
	ASSERT_EQ(calls.size(), 1u);
	EXPECT_EQ(calls[0], "");
}

TEST_F(ArchiveManagementTest, ListSidecarsCallsAnArchiveItCannotIdentifyUnreadable)
{
	const auto zip = Built("game.zip");
	MakeSidecar(zip, HashOf(zip));
	const auto listed = ArchiveManagement::ListSidecars(
	        tmp / "saves", [](const fs::path&, const std::string&) {
		        return std::optional<std::string>();
	        });
	ASSERT_EQ(listed.size(), 1u);
	EXPECT_EQ(listed[0].state, ArchiveManagement::ArchiveState::Unreadable);
}

TEST_F(ArchiveManagementTest, ListSidecarsSkipsLinksAndFoldersWithoutARecord)
{
	fs::create_directories(tmp / "saves" / "stray");
	fs::create_directory_symlink(tmp / "saves" / "stray", tmp / "saves" / "link");
	std::ofstream(tmp / "saves" / "note.txt") << "x";
	EXPECT_TRUE(ArchiveManagement::ListSidecars(tmp / "saves", Identify()).empty());
}

TEST_F(ArchiveManagementTest, ListSidecarsNeverOpensARelativeOrEmptyArchivePath)
{
	const auto dir = tmp / "saves" / "odd-00000000";
	fs::create_directories(dir);
	SidecarRecord record = {};
	record.archive_path  = "relative/game.zip";
	record.identity_hash = std::string(64, '0');
	auto error           = std::string();
	ASSERT_TRUE(record.Save(dir / SidecarTomlName, &error)) << error;
	const auto listed = ArchiveManagement::ListSidecars(tmp / "saves",
	                                                    Identify());
	ASSERT_EQ(listed.size(), 1u);
	EXPECT_EQ(listed[0].state, ArchiveManagement::ArchiveState::Missing);
}

TEST_F(ArchiveManagementTest, ListSidecarsReportsASavesDirectoryItCannotRead)
{
	const auto zip = Built("game.zip");
	MakeSidecar(zip, HashOf(zip));
	fs::permissions(tmp / "saves", fs::perms::none);
	std::error_code probe = {};
	fs::directory_iterator(tmp / "saves", probe);
	if (!probe) {
		fs::permissions(tmp / "saves", fs::perms::owner_all);
		GTEST_SKIP() << "a directory without permissions is still listable here";
	}
	auto problem      = std::string();
	const auto listed = ArchiveManagement::ListSidecars(tmp / "saves",
	                                                    Identify(),
	                                                    &problem);
	fs::permissions(tmp / "saves", fs::perms::owner_all);
	EXPECT_TRUE(listed.empty());
	EXPECT_FALSE(problem.empty());
}

TEST_F(ArchiveManagementTest, ListSidecarsOfAMissingSavesDirectoryIsEmpty)
{
	EXPECT_TRUE(ArchiveManagement::ListSidecars(tmp / "nosaves", Identify()).empty());
	EXPECT_FALSE(fs::exists(tmp / "nosaves"));
}

} // namespace
