// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//
#include "dos/archive/archive_cache.h"

#include "augra/log.h"
#include "dos/archive/archive_identity.h"
#include "dos/archive/archive_lock.h"
#include "dos/archive/archive_toml.h"
#include "dos/archive/exclusive_file.h"
#include "dos/archive/member_path.h"
#include "dos/archive/random_name.h"
#include "utils/checks.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <functional>
#include <unordered_map>
#include <vector>

CHECK_NARROWING();

namespace ArchiveMount {

namespace fs = std::filesystem;

namespace {

constexpr auto LogComponent = "archive_cache";

// Temporaries older than this are a dead writer's, not a live one's.
constexpr int64_t StaleTempSeconds = 60 * 60;

constexpr const char* TagText =
        "Signature: 8a477f597d28d172789f06886806bc55\n"
        "# This file is a cache directory tag created by dosbox-automation.\n"
        "# For information about cache directory tags see https://bford.info/cachedir/\n";

int64_t SystemNow()
{
	return std::chrono::duration_cast<std::chrono::seconds>(
	               std::chrono::system_clock::now().time_since_epoch())
	        .count();
}

std::optional<int64_t> SystemFreeSpace(const fs::path& path)
{
	std::error_code ec = {};
	const auto info    = fs::space(path, ec);
	if (ec) {
		return {};
	}
	return static_cast<int64_t>(info.available);
}

bool IsRegularNoFollow(const fs::path& path, std::error_code& ec)
{
	return fs::is_regular_file(fs::symlink_status(path, ec)) && !ec;
}

bool IsDirectoryNoFollow(const fs::path& path, std::error_code& ec)
{
	return fs::is_directory(fs::symlink_status(path, ec)) && !ec;
}

int64_t FileSizeOrMinusOne(const fs::path& path)
{
	std::error_code ec = {};
	const auto size    = fs::file_size(path, ec);
	return ec ? -1 : static_cast<int64_t>(size);
}

std::optional<int64_t> MtimeSeconds(const fs::path& path)
{
	std::error_code ec = {};
	const auto stamp   = fs::last_write_time(path, ec);
	if (ec) {
		return {};
	}
	// The gap between the file clock and the system clock, not
	// clock_cast: libc++ on the Mini lacks it (gotcha 2026-09-28 for the
	// other direction). A second of slack is nothing for a staleness check.
	const auto delta = std::chrono::duration_cast<std::chrono::seconds>(
	                           stamp - fs::file_time_type::clock::now())
	                           .count();
	return SystemNow() + delta;
}

bool IsTemporaryName(const std::string& name)
{
	return name.ends_with(".part") || name.ends_with(".tmp");
}

bool WriteTag(const fs::path& dir)
{
	FILE* f = OpenExclusive(dir / CacheTagName);
	if (!f) {
		return false;
	}
	const auto text    = std::string(TagText);
	const auto written = std::fwrite(text.data(), 1, text.size(), f) ==
	                     text.size();
	return (std::fclose(f) == 0) && written;
}

bool MakePrivateDirectory(const fs::path& dir)
{
	std::error_code ec = {};
	if (!fs::create_directory(dir, ec) && ec) {
		return false;
	}
	fs::permissions(dir, fs::perms::owner_all, ec);
	return !ec;
}

// Deletes the regular files of an entry and then its directories;
// anything else is left where it is and the entry stays. Returns the
// bytes it freed.
struct RemovalResult {
	bool removed  = false;
	int64_t freed = 0;
};

// The entries of dir through increment(ec): a range-for over
// directory_iterator advances with the throwing operator++.
std::vector<fs::directory_entry> ListDirectory(const fs::path& dir,
                                               std::error_code& ec)
{
	std::vector<fs::directory_entry> out = {};
	ec.clear();
	auto it = fs::directory_iterator(dir, ec);
	for (; !ec && it != fs::directory_iterator(); it.increment(ec)) {
		out.push_back(*it);
	}
	return out;
}

RemovalResult RemoveEntry(const fs::path& dir)
{
	RemovalResult result = {};
	std::error_code ec   = {};
	bool clean           = true;
	const auto files_dir = dir / FilesDirName;
	if (IsDirectoryNoFollow(files_dir, ec)) {
		for (const auto& item : ListDirectory(files_dir, ec)) {
			if (IsRegularNoFollow(item.path(), ec)) {
				const auto size = FileSizeOrMinusOne(item.path());
				if (fs::remove(item.path(), ec) && !ec) {
					result.freed += std::max<int64_t>(size, 0);
				} else {
					clean = false;
				}
			} else {
				augra::log_warn(LogComponent,
				                "leaving '%s': not a regular file",
				                PathToUtf8(item.path()).c_str());
				clean = false;
			}
		}
		if (clean) {
			clean = fs::remove(files_dir, ec) && !ec;
		}
	}
	if (clean) {
		for (const auto* name : {EntryTomlName, LockName}) {
			const auto path = dir / name;
			if (IsRegularNoFollow(path, ec)) {
				fs::remove(path, ec);
			}
		}
		result.removed = fs::remove(dir, ec) && !ec;
	}
	if (!result.removed) {
		augra::log_warn(LogComponent,
		                "entry '%s' kept: not fully ours",
		                PathToUtf8(dir).c_str());
	}
	return result;
}

// Bytes of the regular files under files/, with stale temporaries
// removed on the way when now is given.
int64_t MeasureEntry(const fs::path& dir, const std::optional<int64_t> now)
{
	int64_t bytes      = 0;
	std::error_code ec = {};
	const auto files   = dir / FilesDirName;
	if (!IsDirectoryNoFollow(files, ec)) {
		return 0;
	}
	for (const auto& item : ListDirectory(files, ec)) {
		if (!IsRegularNoFollow(item.path(), ec)) {
			continue;
		}
		const auto name = PathToUtf8(item.path().filename());
		if (now && IsTemporaryName(name)) {
			const auto stamp = MtimeSeconds(item.path());
			if (stamp && *now - *stamp > StaleTempSeconds) {
				fs::remove(item.path(), ec);
				continue;
			}
		}
		bytes += std::max<int64_t>(FileSizeOrMinusOne(item.path()), 0);
	}
	return bytes;
}

struct EntryRecord {
	fs::path dir       = {};
	int64_t last_use   = 0;
	int64_t bytes      = 0;
	bool readable_toml = false;
};

// Only what an entry is made of: regular files under files/, the toml and
// the lock. Anything else keeps the entry where it is.
bool HoldsOnlyOurs(const fs::path& dir)
{
	std::error_code ec = {};
	for (const auto& item : ListDirectory(dir, ec)) {
		const auto name = PathToUtf8(item.path().filename());
		if (name == FilesDirName && IsDirectoryNoFollow(item.path(), ec)) {
			for (const auto& file : ListDirectory(item.path(), ec)) {
				if (!IsRegularNoFollow(file.path(), ec)) {
					return false;
				}
			}
			continue;
		}
		if ((name != EntryTomlName && name != LockName) ||
		    !IsRegularNoFollow(item.path(), ec)) {
			return false;
		}
	}
	return !ec;
}

// Renamed away first so a later Acquire never adopts a dying entry.
// Windows cannot rename a directory with an open file, our lock included,
// so it releases first; a failed rename there means another engine holds it.
RemovalResult RetireEntry(const fs::path& cache_dir, const fs::path& dir,
                          std::optional<FileLock>& lock,
                          const std::function<void(const fs::path&)>& before_remove)
{
	if (!HoldsOnlyOurs(dir)) {
		augra::log_warn(LogComponent,
		                "entry '%s' kept: not fully ours",
		                PathToUtf8(dir).c_str());
		return {};
	}
	std::error_code ec    = {};
	const auto trash_base = cache_dir / TrashDirName;
	if (!IsDirectoryNoFollow(trash_base, ec) &&
	    !MakePrivateDirectory(trash_base)) {
		augra::log_warn(LogComponent,
		                "entry '%s' kept: no trash directory",
		                PathToUtf8(dir).c_str());
		return {};
	}
	auto trash = trash_base / dir.filename();
	trash += "." + RandomHex(2);
#if defined(WIN32)
	lock.reset();
	fs::rename(dir, trash, ec);
#else
	fs::rename(dir, trash, ec);
	lock.reset();
#endif
	if (ec) {
		augra::log_info(LogComponent,
		                "entry '%s' kept: %s",
		                PathToUtf8(dir).c_str(),
		                ec.message().c_str());
		return {};
	}
	if (before_remove) {
		before_remove(dir);
	}
	return RemoveEntry(trash);
}

std::vector<fs::path> Subdirectories(const fs::path& dir)
{
	std::vector<fs::path> out = {};
	std::error_code ec        = {};
	for (const auto& item : ListDirectory(dir, ec)) {
		if (IsDirectoryNoFollow(item.path(), ec)) {
			out.push_back(item.path());
		}
	}
	return out;
}

// Regular files and directories only, deepest first, never through a
// link; false when anything foreign keeps the tree.
bool RemoveTreeNoFollow(const fs::path& dir)
{
	std::error_code ec = {};
	bool clean         = true;
	for (const auto& item : ListDirectory(dir, ec)) {
		const auto& path = item.path();
		if (IsDirectoryNoFollow(path, ec)) {
			clean = RemoveTreeNoFollow(path) && clean;
		} else if (IsRegularNoFollow(path, ec)) {
			if (!fs::remove(path, ec) || ec) {
				clean = false;
			}
		} else {
			augra::log_warn(LogComponent,
			                "leaving '%s': not a regular file",
			                PathToUtf8(path).c_str());
			clean = false;
		}
	}
	if (clean && (!fs::remove(dir, ec) || ec)) {
		clean = false;
	}
	return clean;
}

// Everything but the lock file first, the released lock next, the
// directory last; anything foreign keeps the directory.
void RemoveScratch(const fs::path& dir, const std::function<void()>& release_lock)
{
	std::error_code ec = {};
	bool clean         = true;
	for (const auto& item : ListDirectory(dir, ec)) {
		const auto& path = item.path();
		if (path.filename() == LockName) {
			continue;
		}
		if (IsDirectoryNoFollow(path, ec)) {
			clean = RemoveTreeNoFollow(path) && clean;
		} else if (IsRegularNoFollow(path, ec)) {
			if (!fs::remove(path, ec) || ec) {
				clean = false;
			}
		} else {
			clean = false;
		}
	}
	release_lock();
	if (IsRegularNoFollow(dir / LockName, ec)) {
		fs::remove(dir / LockName, ec);
	}
	if (!clean || !fs::remove(dir, ec) || ec) {
		augra::log_warn(LogComponent,
		                "scratch '%s' kept: not empty",
		                PathToUtf8(dir).c_str());
	}
}

// Retired entries a dead engine did not finish, and scratch directories
// whose mount is gone: their lock is free, or they never got one and are
// older than a live mount's creation could take.
void SweepLeftovers(const fs::path& cache_dir, const int64_t now)
{
	for (const auto& retired : Subdirectories(cache_dir / TrashDirName)) {
		RemoveEntry(retired);
	}
	std::error_code ec = {};
	for (const auto& scratch : Subdirectories(cache_dir / ScratchDirName)) {
		std::optional<FileLock> lock = {};
		if (IsRegularNoFollow(scratch / LockName, ec)) {
			lock = FileLock::TryAcquire(scratch / LockName,
			                            FileLock::Mode::Exclusive);
			if (!lock) {
				continue;
			}
		} else {
			const auto stamp = MtimeSeconds(scratch);
			if (!stamp || now - *stamp <= StaleTempSeconds) {
				continue;
			}
		}
		RemoveScratch(scratch, [&] { lock.reset(); });
	}
}

std::vector<fs::path> EntryDirectories(const fs::path& cache_dir)
{
	std::vector<fs::path> out = {};
	std::error_code ec        = {};
	for (const auto& letter : ListDirectory(cache_dir, ec)) {
		if (PathToUtf8(letter.path().filename()).size() != 1 ||
		    !IsDirectoryNoFollow(letter.path(), ec)) {
			continue;
		}
		for (const auto& entry : ListDirectory(letter.path(), ec)) {
			if (IsDirectoryNoFollow(entry.path(), ec)) {
				out.push_back(entry.path());
			}
		}
	}
	std::sort(out.begin(), out.end());
	return out;
}

} // namespace

const char* CacheStatusText(const CacheStatus status)
{
	switch (status) {
	case CacheStatus::Ok: return "ok";
	case CacheStatus::Refused: return "the cache directory is refused";
	case CacheStatus::NoSpace: return "not enough space in the cache";
	case CacheStatus::Busy:
		return "the cache entry is held by another process";
	case CacheStatus::Failed: return "the cache entry cannot be used";
	case CacheStatus::SourceFailed: return "the archive could not be read";
	}
	return "unknown";
}

struct ArchiveCache::Impl {
	fs::path directory   = {};
	CacheOptions options = {};

	int64_t Now() const
	{
		return options.now ? options.now() : SystemNow();
	}
	std::optional<int64_t> FreeSpace() const
	{
		return options.free_space ? options.free_space(directory)
		                          : SystemFreeSpace(directory);
	}
	// NoSpace unless the reserve survives the write and the cap holds
	// after a cleanup.
	CacheStatus RoomFor(const int64_t bytes, ArchiveCache& cache,
	                    std::string* detail)
	{
		const auto available = FreeSpace();
		if (!available || *available - bytes < options.min_free_bytes) {
			if (detail) {
				*detail = "writing " + std::to_string(bytes) +
				          " bytes would leave less than the reserve of " +
				          std::to_string(options.min_free_bytes) +
				          " bytes free";
			}
			return CacheStatus::NoSpace;
		}
		if (cache.Measure().bytes + bytes > options.max_size_bytes) {
			cache.CleanupToFit(bytes);
		}
		const auto used = cache.Measure().bytes;
		if (used + bytes > options.max_size_bytes) {
			if (detail) {
				*detail = "the cache cap of " +
				          std::to_string(options.max_size_bytes) +
				          " bytes leaves no room for " +
				          std::to_string(bytes) + " more (" +
				          std::to_string(used) + " in use)";
			}
			return CacheStatus::NoSpace;
		}
		return CacheStatus::Ok;
	}
};

struct ArchiveCache::Lease::Impl {
	ArchiveCache* cache          = nullptr;
	ArchiveSource* source        = nullptr;
	fs::path dir                 = {};
	std::optional<FileLock> lock = {};
	ArchiveToml::Document toml   = {};
	bool reused                  = false;
	Error last_source_error      = Error::None;
	CacheOptions options = {}; // the store may die first (engine exit)

	int64_t Now() const
	{
		return options.now ? options.now() : SystemNow();
	}
	std::unordered_map<int, size_t> by_ordinal = {};

	const Entry* FileEntry(const int ordinal) const
	{
		if (!source || ordinal == ImpliedOrdinal) {
			return nullptr;
		}
		const auto found = by_ordinal.find(ordinal);
		if (found == by_ordinal.end()) {
			return nullptr;
		}
		const auto& entry = source->Entries()[found->second];
		return entry.is_dir ? nullptr : &entry;
	}
	fs::path MemberPath(const int ordinal) const
	{
		return dir / FilesDirName / std::to_string(ordinal);
	}
	// Ok when the file is present and whole; Failed when something that
	// is not a regular file sits there; NoSpace-free "missing" otherwise.
	enum class Presence { Whole, Missing, Foreign };
	Presence Check(const Entry& entry) const
	{
		std::error_code ec = {};
		const auto path    = MemberPath(entry.ordinal);
		const auto status  = fs::symlink_status(path, ec);
		if (ec || !fs::exists(status)) {
			return Presence::Missing;
		}
		if (!fs::is_regular_file(status)) {
			return Presence::Foreign;
		}
		if (FileSizeOrMinusOne(path) == entry.size) {
			return Presence::Whole;
		}
		fs::remove(path, ec);
		return Presence::Missing;
	}
};

ArchiveCache::ArchiveCache() : impl(std::make_unique<Impl>()) {}
ArchiveCache::~ArchiveCache() = default;

ArchiveCache::OpenResult ArchiveCache::Open(const fs::path& directory,
                                            const CacheOptions& options)
{
	OpenResult result  = {};
	std::error_code ec = {};
	const auto status  = fs::symlink_status(directory, ec);
	auto refuse        = [&](const std::string& why) {
                result.status = CacheStatus::Refused;
                result.detail = PathToUtf8(directory) + ": " + why;
                return std::move(result);
	};
	if (!ec && fs::exists(status)) {
		if (!fs::is_directory(status)) {
			return refuse("not a directory");
		}
		bool empty = true;
		for (const auto& item : ListDirectory(directory, ec)) {
			(void)item;
			empty = false;
			break;
		}
		if (ec) {
			return refuse("cannot list");
		}
		if (!empty && !IsRegularNoFollow(directory / CacheTagName, ec)) {
			return refuse("a non-empty directory without " +
			              std::string(CacheTagName));
		}
		if (empty && !WriteTag(directory)) {
			return refuse("cannot write the tag");
		}
	} else {
		if (!MakePrivateDirectory(directory) || !WriteTag(directory)) {
			return refuse("cannot create");
		}
	}
	auto cache = std::unique_ptr<ArchiveCache>(new ArchiveCache());
	cache->impl->directory = directory;
	cache->impl->options   = options;
	result.cache           = std::move(cache);
	return result;
}

const fs::path& ArchiveCache::Directory() const
{
	return impl->directory;
}

const CacheOptions& ArchiveCache::Options() const
{
	return impl->options;
}

char ArchiveCache::SpreadLetter(const std::string_view sanitized_stem)
{
	if (sanitized_stem.empty()) {
		return '_';
	}
	const auto c = sanitized_stem.front();
	if (c >= 'a' && c <= 'z') {
		return c;
	}
	if (c >= 'A' && c <= 'Z') {
		return static_cast<char>(c - 'A' + 'a');
	}
	if (c >= '0' && c <= '9') {
		return '0';
	}
	return '_';
}

fs::path ArchiveCache::EntryDirectory(const fs::path& cache_dir,
                                      const std::string_view stem,
                                      const std::string_view hash_hex)
{
	const auto sanitized = ArchiveIdentity::SanitizeStem(stem);
	const auto key       = ArchiveIdentity::ShortKey(stem, hash_hex);
	return cache_dir / std::string(1, SpreadLetter(sanitized)) / key;
}

ArchiveCache::Usage ArchiveCache::Measure() const
{
	Usage usage        = {};
	std::error_code ec = {};
	for (const auto& dir : EntryDirectories(impl->directory)) {
		if (IsRegularNoFollow(dir / EntryTomlName, ec)) {
			++usage.entries;
		}
		usage.bytes += MeasureEntry(dir, {});
	}
	return usage;
}

ArchiveCache::CleanupReport ArchiveCache::Cleanup()
{
	return CleanupToFit(0);
}

ArchiveCache::CleanupReport ArchiveCache::Purge()
{
	return CleanupToFit(0, true);
}

ArchiveCache::CleanupReport ArchiveCache::CleanupToFit(const int64_t extra_bytes,
                                                       const bool retire_all)
{
	CleanupReport report = {};
	const auto now       = impl->Now();
	SweepLeftovers(impl->directory, now);
	std::vector<EntryRecord> kept = {};
	std::vector<FileLock> held    = {};
	for (const auto& dir : EntryDirectories(impl->directory)) {
		auto lock = FileLock::TryAcquire(dir / LockName,
		                                 FileLock::Mode::Exclusive);
		if (!lock) {
			++report.skipped_locked;
			continue;
		}
		auto error = std::string();
		const auto doc = ArchiveToml::ReadFile(dir / EntryTomlName, &error);
		EntryRecord record = {};
		record.dir         = dir;
		record.bytes       = MeasureEntry(dir, now);
		if (!doc || doc->root.Integer("version") != EntryTomlVersion ||
		    !doc->root.Integer("last_use")) {
			if (RetireEntry(impl->directory,
			                dir,
			                lock,
			                impl->options.before_remove)
			            .removed) {
				++report.removed_damaged;
			}
			continue;
		}
		record.readable_toml = true;
		record.last_use      = *doc->root.Integer("last_use");
		if (retire_all || now - record.last_use > impl->options.ttl_seconds) {
			if (RetireEntry(impl->directory,
			                dir,
			                lock,
			                impl->options.before_remove)
			            .removed) {
				++report.removed_expired;
			}
			continue;
		}
		kept.push_back(record);
		held.push_back(std::move(*lock));
	}
	// Oldest first until under the cap, each lock handed to its removal.
	std::vector<size_t> order(kept.size());
	for (size_t i = 0; i < order.size(); ++i) {
		order[i] = i;
	}
	std::stable_sort(order.begin(), order.end(), [&](const size_t a, const size_t b) {
		return kept[a].last_use < kept[b].last_use;
	});
	// Locked entries count against the cap too, so measure everything.
	int64_t total = Measure().bytes;
	for (const auto index : order) {
		if (total + extra_bytes <= impl->options.max_size_bytes) {
			break;
		}
		std::optional<FileLock> release = std::move(held[index]);
		const auto removal              = RetireEntry(impl->directory,
                                                 kept[index].dir,
                                                 release,
                                                 impl->options.before_remove);
		if (removal.removed) {
			++report.removed_for_cap;
		}
		total -= removal.freed;
	}
	held.clear();
	report.bytes_after = Measure().bytes;
	return report;
}

std::optional<ArchiveCache::Scratch> ArchiveCache::MakeScratch() const
{
	std::error_code ec = {};
	const auto base    = impl->directory / ScratchDirName;
	if (!IsDirectoryNoFollow(base, ec) && !MakePrivateDirectory(base)) {
		return {};
	}
	const auto dir = base / RandomHex(2);
	if (!MakePrivateDirectory(dir)) {
		return {};
	}
	auto lock = FileLock::TryAcquire(dir / LockName, FileLock::Mode::Exclusive);
	if (!lock) {
		fs::remove(dir, ec);
		return {};
	}
	return Scratch{dir, std::make_shared<FileLock>(std::move(*lock))};
}

ArchiveCache::LeaseResult ArchiveCache::Acquire(ArchiveSource& source,
                                                const fs::path& archive_path)
{
	LeaseResult result = {};
	auto fail = [&](const CacheStatus status, const std::string& why) {
		result.status = status;
		result.detail = why;
		return std::move(result);
	};
	const auto stem = PathToUtf8(archive_path.stem());
	const auto dir = EntryDirectory(impl->directory, stem, source.IdentityHash());
	std::error_code ec = {};
	if (!IsDirectoryNoFollow(dir.parent_path(), ec) &&
	    !MakePrivateDirectory(dir.parent_path())) {
		return fail(CacheStatus::Failed,
		            "cannot create " + PathToUtf8(dir.parent_path()));
	}
	if (!IsDirectoryNoFollow(dir, ec) && !MakePrivateDirectory(dir)) {
		return fail(CacheStatus::Failed, "cannot create " + PathToUtf8(dir));
	}
	auto lock = FileLock::TryAcquire(dir / LockName, FileLock::Mode::Shared);
	if (!lock) {
		return fail(CacheStatus::Busy,
		            PathToUtf8(dir) + " is held exclusively");
	}
	// impl->cache stays null until the lease is whole, so a failed
	// Acquire destroys a lease that writes nothing.
	auto lease           = std::unique_ptr<Lease>(new Lease());
	lease->impl->source  = &source;
	lease->impl->options = impl->options;
	lease->impl->dir     = dir;
	lease->impl->lock    = std::move(lock);
	const auto& entries  = source.Entries();
	for (size_t i = 0; i < entries.size(); ++i) {
		lease->impl->by_ordinal.emplace(entries[i].ordinal, i);
	}
	const auto stat    = source.IndexedStat();
	const auto now     = impl->Now();
	auto error         = std::string();
	auto doc           = ArchiveToml::ReadFile(dir / EntryTomlName, &error);
	const bool matches = doc &&
	                     doc->root.Integer("version") == EntryTomlVersion &&
	                     doc->root.String("identity_hash") ==
	                             source.IdentityHash() &&
	                     doc->root.Integer("archive_size") == stat.size &&
	                     doc->root.Integer("archive_mtime") == stat.mtime &&
	                     doc->root.Integer("entry_count") ==
	                             static_cast<int64_t>(entries.size()) &&
	                     doc->root.Integer("total_bytes") ==
	                             source.TotalBytes() &&
	                     doc->root.Integer("created");
	if (matches) {
		lease->impl->reused = true;
		lease->impl->toml   = *doc;
	} else {
		if (doc) {
			augra::log_info(LogComponent,
			                "%s: recorded archive differs, starting the entry over",
			                PathToUtf8(dir).c_str());
		}
		// Only regular files are ours to delete; anything else fails
		// the lease.
		const auto files = dir / FilesDirName;
		if (IsDirectoryNoFollow(files, ec)) {
			for (const auto& item : ListDirectory(files, ec)) {
				if (!IsRegularNoFollow(item.path(), ec) ||
				    !fs::remove(item.path(), ec)) {
					return fail(CacheStatus::Failed,
					            "cannot clear " +
					                    PathToUtf8(item.path()));
				}
			}
		}
		ArchiveToml::Document fresh = {};
		fresh.root.Set("version", int64_t{EntryTomlVersion});
		fresh.root.Set("archive_path", PathToUtf8(archive_path));
		fresh.root.Set("identity_hash", source.IdentityHash());
		fresh.root.Set("entry_count", static_cast<int64_t>(entries.size()));
		fresh.root.Set("total_bytes", source.TotalBytes());
		fresh.root.Set("archive_size", stat.size);
		fresh.root.Set("archive_mtime", stat.mtime);
		fresh.root.Set("created", now);
		fresh.root.Set("last_use", now);
		if (!ArchiveToml::WriteAtomically(dir / EntryTomlName, fresh, &error)) {
			return fail(CacheStatus::Failed, error);
		}
		lease->impl->toml = fresh;
	}
	if (!IsDirectoryNoFollow(dir / FilesDirName, ec) &&
	    !MakePrivateDirectory(dir / FilesDirName)) {
		return fail(CacheStatus::Failed,
		            "cannot create " + PathToUtf8(dir / FilesDirName));
	}
	lease->impl->cache = this;
	result.lease       = std::move(lease);
	return result;
}

ArchiveCache::Lease::Lease() : impl(std::make_unique<Impl>()) {}

ArchiveCache::Lease::~Lease()
{
	if (!impl->cache) {
		return;
	}
	impl->toml.root.Set("last_use", impl->Now());
	auto error = std::string();
	if (!ArchiveToml::WriteAtomically(impl->dir / EntryTomlName, impl->toml, &error)) {
		augra::log_warn(LogComponent, "%s", error.c_str());
	}
	impl->lock.reset();
}

const fs::path& ArchiveCache::Lease::Directory() const
{
	return impl->dir;
}

fs::path ArchiveCache::Lease::FilesDirectory() const
{
	return impl->dir / FilesDirName;
}

bool ArchiveCache::Lease::WasReused() const
{
	return impl->reused;
}

bool ArchiveCache::Lease::IsComplete() const
{
	if (!impl->source) {
		return false;
	}
	for (const auto& entry : impl->source->Entries()) {
		if (entry.is_dir) {
			continue;
		}
		if (impl->Check(entry) != Impl::Presence::Whole) {
			return false;
		}
	}
	return true;
}

bool ArchiveCache::Lease::Rebind(ArchiveSource& source)
{
	if (!impl->source) {
		return false;
	}
	if (source.IdentityHash() != impl->source->IdentityHash()) {
		return false;
	}
	impl->source = &source;
	impl->by_ordinal.clear();
	const auto& entries = source.Entries();
	for (size_t i = 0; i < entries.size(); ++i) {
		impl->by_ordinal.emplace(entries[i].ordinal, i);
	}
	return true;
}

void ArchiveCache::Lease::Detach()
{
	impl->source = nullptr;
	impl->by_ordinal.clear();
}

Error ArchiveCache::Lease::LastSourceError() const
{
	return impl->last_source_error;
}

CacheStatus ArchiveCache::Lease::MemberFile(const int ordinal, fs::path& out,
                                            std::string* detail)
{
	if (!impl->source) {
		if (detail) {
			*detail = "the lease is detached from its source";
		}
		return CacheStatus::Failed;
	}
	const auto* entry = impl->FileEntry(ordinal);
	if (!entry) {
		if (detail) {
			*detail = "no such member: " + std::to_string(ordinal);
		}
		return CacheStatus::Failed;
	}
	const auto path = impl->MemberPath(ordinal);
	switch (impl->Check(*entry)) {
	case Impl::Presence::Whole: out = path; return CacheStatus::Ok;
	case Impl::Presence::Foreign:
		if (detail) {
			*detail = PathToUtf8(path) + " is not a regular file";
		}
		return CacheStatus::Failed;
	case Impl::Presence::Missing: break;
	}
	if (impl->source->GetAccess() == Access::Sequential) {
		if (detail) {
			*detail = "a sequential archive is served by its one pass only";
		}
		return CacheStatus::Failed;
	}
	if (const auto room = impl->cache->impl->RoomFor(entry->size, *impl->cache, detail);
	    room != CacheStatus::Ok) {
		return room;
	}
	auto why = std::string();
	if (const auto error = impl->source->MaterializeToFile(ordinal, path, &why);
	    error != Error::None) {
		impl->last_source_error = error;
		if (detail) {
			*detail = std::string(ErrorText(error)) + ": " + why;
		}
		return CacheStatus::SourceFailed;
	}
	if (impl->Check(*entry) != Impl::Presence::Whole) {
		if (detail) {
			*detail = PathToUtf8(path) +
			          " is not whole after materialization";
		}
		return CacheStatus::Failed;
	}
	out = path;
	return CacheStatus::Ok;
}

CacheStatus ArchiveCache::Lease::ServeMember(const int ordinal, fs::path& out,
                                             std::string* detail)
{
	if (impl->source && impl->source->GetAccess() == Access::Sequential) {
		const auto status = EnsureAll(detail);
		if (status == CacheStatus::Ok) {
			out = FilesDirectory() / std::to_string(ordinal);
		}
		return status;
	}
	return MemberFile(ordinal, out, detail);
}

CacheStatus ArchiveCache::Lease::EnsureAll(std::string* detail)
{
	if (!impl->source) {
		if (detail) {
			*detail = "the lease is detached from its source";
		}
		return CacheStatus::Failed;
	}
	if (IsComplete()) {
		return CacheStatus::Ok;
	}
	if (const auto room = impl->cache->impl->RoomFor(impl->source->TotalBytes(),
	                                                 *impl->cache,
	                                                 detail);
	    room != CacheStatus::Ok) {
		return room;
	}
	auto why = std::string();
	if (const auto error = impl->source->MaterializeAll(FilesDirectory(), &why);
	    error != Error::None) {
		impl->last_source_error = error;
		if (detail) {
			*detail = std::string(ErrorText(error)) + ": " + why;
		}
		return CacheStatus::SourceFailed;
	}
	if (!IsComplete()) {
		if (detail) {
			*detail = "the one pass left members missing or short";
		}
		return CacheStatus::Failed;
	}
	return CacheStatus::Ok;
}

void ArchiveCache::DiscardScratch(Scratch& scratch)
{
	if (scratch.dir.empty()) {
		return;
	}
	RemoveScratch(scratch.dir, [&] { scratch.lock.reset(); });
	scratch.dir.clear();
}

} // namespace ArchiveMount
