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

// Shared by mount_archive.cpp and archive_images.cpp; nothing else.
namespace MountArchiveInternal {

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

// The engine's one store (design D14), opened or reopened for the settings.
ArchiveMount::ArchiveCache* OpenStore(const ArchiveSettings& s, std::string* detail);

MountVerdict Denied(DenyReason reason);

struct SidecarPlace {
	std::filesystem::path directory = {};
	const char* how                 = "created";
	MountVerdict refusal            = {}; // reason None when placed
	std::string detail              = {};
};

// Design D2 without the lock: -sidecar through the directory policy, else
// by path, by hash, beside the archive when the policy allows, or created.
SidecarPlace PlaceSidecar(const std::string& sidecar_path,
                          const std::filesystem::path& archive,
                          const ArchiveMount::ArchiveSource& source);

// The record of a created sidecar; false with the detail when not written.
bool EnsureSidecarRecord(const std::filesystem::path& directory,
                         const std::filesystem::path& archive,
                         const ArchiveMount::ArchiveSource& source,
                         std::string* detail);

} // namespace MountArchiveInternal

#endif // DOSBOX_MOUNT_ARCHIVE_INTERNAL_H
