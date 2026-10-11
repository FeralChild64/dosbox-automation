// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//
#ifndef DOSBOX_MOUNT_ARCHIVE_INTERNAL_H
#define DOSBOX_MOUNT_ARCHIVE_INTERNAL_H

#include "dos/archive/archive_cache.h"
#include "dos/archive/archive_source.h"
#include "dos/programs/mount_policy.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

// Shared by mount_archive.cpp, archive_images.cpp and the command line's
// management switches (main.cpp); nothing else.
namespace MountArchiveInternal {

constexpr int64_t MiB = 1024 * 1024;

struct ArchiveSettings {
	std::filesystem::path cache_path = {}; // empty: the platform default
	int64_t cache_max_bytes          = 0;
	int64_t cache_min_free_bytes     = 0;
	int64_t ttl_seconds              = 0;
	int64_t memory_limit_bytes       = 0;
	int64_t max_entries              = 0;
	int64_t max_size_bytes           = 0;
	bool sequential_allowed          = false;
};

ArchiveSettings ReadArchiveSettings();

// Shared by the engine's store and the command line's management switches;
// an empty cache path means D14's platform default, an empty problem a
// usable path.
std::filesystem::path EffectiveCachePath(const ArchiveSettings& s);
std::filesystem::path SavesDirectory();
ArchiveMount::CacheOptions CacheOptionsFor(const ArchiveSettings& s);
std::string CachePathProblem(const std::filesystem::path& path);

// The engine's one store (design D14), opened or reopened for the settings.
// cleaned, when given, gets the report of the cleanup a fresh open runs.
ArchiveMount::ArchiveCache* OpenStore(
        const ArchiveSettings& s, std::string* detail,
        std::optional<ArchiveMount::ArchiveCache::CleanupReport>* cleaned = nullptr);

MountVerdict Denied(DenyReason reason);

struct SidecarPlace {
	std::filesystem::path directory = {};
	const char* how                 = "created";
	// This call made the directory; "created" also names one that was
	// already there without a record it could read.
	bool made_now        = false;
	MountVerdict refusal = {}; // reason None when placed
	std::string detail   = {};
};

// Design D2 without the lock: -sidecar through the directory policy, else
// by path, by hash, beside the archive when the policy allows, or created.
// For a nested member the archive is the host plus the chain
// (outer.zip/inner.zip) and the identity is ChainIdentity's.
SidecarPlace PlaceSidecar(const std::string& sidecar_path,
                          const std::filesystem::path& archive,
                          std::string_view identity_hash);

// The record of a created sidecar; false with the detail when not written.
bool EnsureSidecarRecord(const std::filesystem::path& directory,
                         const std::filesystem::path& archive,
                         std::string_view identity_hash,
                         const ArchiveMount::ArchiveSource& source,
                         std::string* detail);

} // namespace MountArchiveInternal

#endif // DOSBOX_MOUNT_ARCHIVE_INTERNAL_H
