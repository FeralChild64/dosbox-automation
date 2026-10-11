// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//
#ifndef DOSBOX_ARCHIVE_MANAGEMENT_H
#define DOSBOX_ARCHIVE_MANAGEMENT_H

#include "dos/archive/archive_cache.h"
#include "dos/archive/archive_source.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

// Design 6.1: what the management commands show and do, as data. The
// engine and the command line format it; nothing here prints.
namespace ArchiveManagement {

struct CacheEntryInfo {
	std::filesystem::path directory = {};
	std::string archive_path        = {}; // UTF-8, as recorded
	std::string identity_hash       = {};
	int64_t bytes                   = 0; // regular files under files/
	int64_t last_use                = 0; // seconds since the epoch
	bool locked                     = false;
	bool damaged                    = false; // no readable record
};

struct CacheOverview {
	bool exists                       = false;
	std::filesystem::path directory   = {};
	int64_t bytes                     = 0;
	int64_t cap_bytes                 = 0;
	int64_t min_free_bytes            = 0;
	int64_t ttl_seconds               = 0;
	std::optional<int64_t> free_bytes = {}; // the file system's, when known
	std::vector<CacheEntryInfo> entries = {};
	std::string problem = {}; // non-empty: no cache there, or unreadable
};

// Never creates the directory.
CacheOverview InspectCache(const std::filesystem::path& cache_dir,
                           const ArchiveMount::CacheOptions& options);

struct EmptyReport {
	bool ok             = false;
	bool cache_exists   = false;
	int removed         = 0;
	int skipped_locked  = 0;
	int64_t bytes_after = 0;
	std::string problem = {};
};

// Every entry nobody holds, through the cache's own Purge; locked entries
// are skipped and counted. A missing cache is nothing to do (decision 1b).
EmptyReport EmptyCache(const std::filesystem::path& cache_dir,
                       const ArchiveMount::CacheOptions& options);

enum class ArchiveState { Missing, Same, Changed, Unreadable };

struct SidecarInfo {
	std::filesystem::path directory = {};
	std::string archive_path        = {};
	std::string identity_hash       = {};
	int64_t bytes                   = 0; // regular files in the sidecar
	int64_t created                 = 0;
	ArchiveState state              = ArchiveState::Missing;
};

// The archive's identity hash, nothing when it cannot be indexed. The
// command line passes the helper (D15), so no archive is parsed in-process.
using IdentifyArchive =
        std::function<std::optional<std::string>(const std::filesystem::path&)>;

// Every directory under saves with a readable record; links and
// record-less folders are skipped. identify runs once per present archive;
// problem, when given, names a saves directory that could not be read.
std::vector<SidecarInfo> ListSidecars(const std::filesystem::path& saves_dir,
                                      const IdentifyArchive& identify,
                                      std::string* problem = nullptr);

} // namespace ArchiveManagement

#endif // DOSBOX_ARCHIVE_MANAGEMENT_H
