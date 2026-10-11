// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//
#ifndef DOSBOX_ARCHIVE_CACHE_H
#define DOSBOX_ARCHIVE_CACHE_H

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "dos/archive/archive_source.h"

namespace ArchiveMount {

enum class CacheStatus {
	Ok,
	Refused,      // the directory cannot be a cache: link, file, untagged
	NoSpace,      // the reserve or the cap; detail names which
	Busy,         // another holder has the entry exclusively
	Failed,       // an I/O failure or a foreign object in the entry
	SourceFailed, // the source refused; detail carries its text
};

const char* CacheStatusText(CacheStatus status);

constexpr const char* CacheTagName  = "CACHEDIR.TAG";
constexpr const char* EntryTomlName = ".source-archive.toml";
constexpr const char* LockName      = ".lock";
constexpr const char* FilesDirName  = "files";
// Beside the spread letters: removed entries pass through the trash,
// each mount's in-memory materializations go to a scratch directory.
constexpr const char* TrashDirName   = "trash";
constexpr const char* ScratchDirName = "scratch";

class FileLock;
constexpr int EntryTomlVersion = 1;

struct CacheOptions {
	int64_t max_size_bytes = 4096ll * 1024 * 1024; // design D14
	int64_t min_free_bytes = 2048ll * 1024 * 1024;
	int64_t ttl_seconds    = 7 * 24 * 60 * 60;
	// Test hooks; empty means std::filesystem::space and the system clock.
	std::function<std::optional<int64_t>(const std::filesystem::path&)> free_space = {};
	std::function<int64_t()> now = {};
	// Called with an entry Cleanup has decided to remove, just before its
	// files go.
	std::function<void(const std::filesystem::path& entry)> before_remove = {};
};

// The store for materialized members (design D14, 2.1): a tagged
// directory, one entry per archive under a one-letter spread, a shared
// lock per mounted entry, a cap, a free-space reserve and a TTL.
class ArchiveCache {
public:
	struct OpenResult {
		std::unique_ptr<ArchiveCache> cache = {};
		CacheStatus status                  = CacheStatus::Ok;
		std::string detail                  = {};
	};
	// Creates the directory (0700) and the tag when missing or empty.
	// Refuses a link, a file, and a non-empty directory without the tag.
	// The policy's system-path check runs before this.
	static OpenResult Open(const std::filesystem::path& directory,
	                       const CacheOptions& options);
	~ArchiveCache();
	ArchiveCache(const ArchiveCache&)            = delete;
	ArchiveCache& operator=(const ArchiveCache&) = delete;

	const std::filesystem::path& Directory() const;
	const CacheOptions& Options() const;

	struct Usage {
		int64_t bytes = 0;
		int entries   = 0;
	};
	Usage Measure() const;

	struct CleanupReport {
		int removed_expired = 0;
		int removed_for_cap = 0;
		int removed_damaged = 0;
		int skipped_locked  = 0;
		int64_t bytes_after = 0;
	};
	// Entries past the TTL, then the oldest until under the cap; locked
	// entries are skipped; only regular files are ever deleted. Trash and
	// scratch directories nobody holds are swept first.
	CleanupReport Cleanup();
	// Every entry nobody holds, whatever its last use says (design 6.1);
	// counted as expired.
	CleanupReport Purge();

	// A scratch directory of one mount's own, locked while the lock lives.
	struct Scratch {
		std::filesystem::path dir      = {};
		std::shared_ptr<FileLock> lock = {};
	};
	std::optional<Scratch> MakeScratch() const;
	// Removes the scratch's tree (regular files and directories, never
	// through a link), releases its lock and empties the struct. A tree
	// that cannot be removed is left for the next SweepLeftovers.
	static void DiscardScratch(Scratch& scratch);

	class Lease;
	struct LeaseResult {
		std::unique_ptr<Lease> lease = {};
		CacheStatus status           = CacheStatus::Ok;
		std::string detail           = {};
	};
	// The entry for this archive, locked shared for the lease's life.
	// A recorded identity that differs from the source's starts the
	// entry over. The lease refers to the source until Detach.
	LeaseResult Acquire(ArchiveSource& source,
	                    const std::filesystem::path& archive_path);

	// The spread letter of D14: a lower-case letter, '0' for a digit,
	// '_' for anything else.
	static char SpreadLetter(std::string_view sanitized_stem);
	// <cache>/<letter>/<sanitized stem>-<hash8>
	static std::filesystem::path EntryDirectory(const std::filesystem::path& cache_dir,
	                                            std::string_view stem,
	                                            std::string_view hash_hex);

private:
	ArchiveCache();
	// Cleanup that also makes room for extra_bytes under the cap.
	CleanupReport CleanupToFit(int64_t extra_bytes, bool retire_all = false);
	struct Impl;
	std::unique_ptr<Impl> impl;
};

class ArchiveCache::Lease {
public:
	~Lease(); // records the last use, releases the lock
	Lease(const Lease&)            = delete;
	Lease& operator=(const Lease&) = delete;

	const std::filesystem::path& Directory() const;
	std::filesystem::path FilesDirectory() const;
	bool WasReused() const;

	// files/<ordinal> for a regular-file entry, materialized now when
	// missing; refused with Failed for a sequential source, which only
	// EnsureAll serves.
	CacheStatus MemberFile(int ordinal, std::filesystem::path& out,
	                       std::string* detail);
	// The cache's file for a member whatever the source's access: the one
	// pass for a sequential source, MemberFile otherwise.
	CacheStatus ServeMember(int ordinal, std::filesystem::path& out,
	                        std::string* detail);
	// The one pass of a sequential source, run only when a member is
	// missing; judged by the declared total against the reserve and the cap.
	CacheStatus EnsureAll(std::string* detail);
	// Every regular-file entry present with its declared size.
	bool IsComplete() const;
	// Points the lease at another source for the same archive (the
	// reconnect of design D16): refused when the identity hash differs.
	bool Rebind(ArchiveSource& source);
	// Ends the lease's use of the source; the entry stays locked and every
	// call that needs the source answers Failed (Rebind false) from here on.
	void Detach();
	// The source's error behind the last SourceFailed answer.
	Error LastSourceError() const;

private:
	friend class ArchiveCache;
	Lease();
	struct Impl;
	std::unique_ptr<Impl> impl;
};

} // namespace ArchiveMount

#endif // DOSBOX_ARCHIVE_CACHE_H
