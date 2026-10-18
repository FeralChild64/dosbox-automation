// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//
#ifndef DOSBOX_MOUNT_ARCHIVE_INTERNAL_H
#define DOSBOX_MOUNT_ARCHIVE_INTERNAL_H

#include "dos/archive/archive_cache.h"
#include "dos/archive/archive_helper_client.h"
#include "dos/archive/archive_source.h"
#include "dos/archive/chain_resolver.h"
#include "dos/drives.h"
#include "dos/programs/mount_policy.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

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
std::string SystemPathProblem(const std::filesystem::path& path);
std::string CachePathProblem(const std::filesystem::path& path);

// The engine's one store (design D14), opened or reopened for the settings.
// cleaned, when given, gets the report of the cleanup a fresh open runs.
ArchiveMount::ArchiveCache* OpenStore(
        const ArchiveSettings& s, std::string* detail,
        std::optional<ArchiveMount::ArchiveCache::CleanupReport>* cleaned = nullptr);

MountVerdict Denied(DenyReason reason);

// A resolved chain below a host: the leaf client, what it reads through
// (held for the drive's life) and the chain prefix. Used by MOUNT's
// entering (N3), its reconnect, and the sidecar lister's identify.
struct Entered {
	std::unique_ptr<ArchiveMount::ArchiveHelperClient> leaf = {};
	std::vector<ArchiveMount::ChainLink> links              = {};
	std::string chain                                       = {};
	bool window                                             = false;
};

// Nested design 4.1: a member path whose leaf is an archive name mounts
// that leaf, with the levels and the leaf as one member list. Nothing
// for an image leaf or a lone path; too_deep past N1.
struct DriveChain {
	std::vector<std::string> members = {};
	bool too_deep                    = false;
};
std::optional<DriveChain> ArchiveDriveChain(const ArchiveMount::MemberPath& split);

// What a drive enters below its host: the spelled members with the house rule
// at every step (N3, N4), one walk within MaxNestingDepth; shared by
// MountArchive, its reopen and the sidecar lister. A plain host enters nothing.
struct DriveEntry {
	std::optional<Entered> entered = {};
	std::string house_member       = {};
	bool failed                    = false;
	ArchiveMount::Error error      = ArchiveMount::Error::None;
	std::string detail             = {};
};
DriveEntry EnterForDrive(ArchiveMount::ArchiveHelperClient& host,
                         const std::filesystem::path& host_path,
                         const std::vector<std::string>& members,
                         ArchiveMount::ArchiveCache& cache,
                         const ArchiveMount::ChainOptions& options);

// The drive's holder for a chain's links (ArchiveDrive::HeldResources).
struct ChainHold final : public ArchiveDrive::HeldResources {
	std::vector<ArchiveMount::ChainLink> links = {};
};

// The chain options MOUNT derives from the settings.
ArchiveMount::ChainOptions ChainOptionsFor(
        const ArchiveSettings& settings, const ArchiveMount::Limits& limits,
        const ArchiveMount::ArchiveHelperClient::Options& helper);

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
// by path, by hash, beside the archive when allowed, or created under
// key_stem (empty: the archive's stem; a nested member's host, N5).
SidecarPlace PlaceSidecar(const std::string& sidecar_path,
                          const std::filesystem::path& archive,
                          std::string_view identity_hash,
                          std::string_view key_stem = {});

// The record of a created sidecar; false with the detail when not written.
bool EnsureSidecarRecord(const std::filesystem::path& directory,
                         const std::filesystem::path& archive,
                         std::string_view identity_hash,
                         const ArchiveMount::ArchiveSource& source,
                         std::string* detail);

} // namespace MountArchiveInternal

#endif // DOSBOX_MOUNT_ARCHIVE_INTERNAL_H
