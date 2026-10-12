// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//
#include "dos/programs/mount.h"
#include "dos/programs/mount_archive_internal.h"

#include "augra/log.h"
#include "config/setup.h"
#include "dos/archive/archive_cache.h"
#include "dos/archive/archive_helper_client.h"
#include "dos/archive/archive_lock.h"
#include "dos/archive/archive_tree.h"
#include "dos/archive/chain_resolver.h"
#include "dos/archive/head_sniff.h"
#include "dos/archive/member_path.h"
#include "dos/archive/sidecar_locator.h"
#include "dos/archive/sidecar_record.h"
#include "dos/drives.h"
#include "dos/programs/mount_policy.h"
#include "misc/cross.h"
#include "misc/notifications.h"
#include "utils/checks.h"
#include "utils/fs_utils.h"
#include "utils/string_utils.h"

#include <algorithm>
#include <cassert>
#include <cctype>
#include <chrono>
#include <ctime>
#include <random>
#include <unordered_map>

CHECK_NARROWING();

using namespace ArchiveMount;
using namespace MountArchiveInternal;

namespace {

constexpr auto LogComponent = "mount";

// The engine's one store (design D14): opened and cleaned up at engine
// start (ArchiveCacheStartup), reopened when the settings change.
struct CacheStore {
	std::unique_ptr<ArchiveCache> cache = {};
	std_fs::path path                   = {};
	int64_t max_bytes                   = 0;
	int64_t min_free                    = 0;
	int64_t ttl                         = 0;
};
CacheStore store = {};

int MountedArchiveDrives()
{
	int count = 0;
	for (const auto& drive : Drives) {
		if (ArchiveDriveOf(drive)) {
			++count;
		}
	}
	return count;
}

} // namespace

namespace MountArchiveInternal {

std_fs::path EffectiveCachePath(const ArchiveSettings& s)
{
	return s.cache_path.empty() ? get_cache_dir() / "archive-cache"
	                            : s.cache_path;
}

std_fs::path SavesDirectory()
{
	const auto section = get_section("dos");
	assert(section);
	const auto raw = section->GetString("archive_sidecar_path");
	return raw.empty() ? get_data_dir() / "saves" : resolve_home(raw);
}

CacheOptions CacheOptionsFor(const ArchiveSettings& s)
{
	CacheOptions options   = {};
	options.max_size_bytes = s.cache_max_bytes;
	options.min_free_bytes = s.cache_min_free_bytes;
	options.ttl_seconds    = s.ttl_seconds;
	return options;
}

std::string CachePathProblem(const std_fs::path& path)
{
	// The directory may not exist yet. Links above it belong to the host's
	// layout (/home on Silverblue or FreeBSD); the leaf may not be one, and
	// the system-path rule judges the canonical place.
	std::error_code ec = {};
	if (std_fs::is_symlink(std_fs::symlink_status(path, ec))) {
		return PathToUtf8(path) + " is a link";
	}
	const auto probe = std_fs::weakly_canonical(path, ec);
	if (!ec && MountPolicy::IsUnderSystemPath(probe)) {
		return PathToUtf8(path) + " is a system path";
	}
	return {};
}

ArchiveSettings ReadArchiveSettings()
{
	ArchiveSettings s  = {};
	const auto section = get_section("dos");
	assert(section);
	const auto raw_path = section->GetString("archive_cache_path");
	if (!raw_path.empty()) {
		s.cache_path = resolve_home(raw_path);
	}
	s.cache_max_bytes = section->GetInt("archive_cache_max_size") * MiB;
	s.cache_min_free_bytes = section->GetInt("archive_cache_min_free") * MiB;
	s.ttl_seconds = section->GetInt("archive_cache_ttl") * 24ll * 60 * 60;
	s.memory_limit_bytes = section->GetInt("archive_memory_limit") * MiB;
	s.max_entries        = section->GetInt("archive_max_entries");
	s.max_size_bytes     = section->GetInt("archive_max_size") * MiB;
	s.sequential_allowed = section->GetString("archive_sequential") == "cache";
	return s;
}

ArchiveCache* OpenStore(const ArchiveSettings& s, std::string* detail,
                        std::optional<ArchiveCache::CleanupReport>* cleaned)
{
	const auto path = EffectiveCachePath(s);
	if (store.cache && store.path == path && store.max_bytes == s.cache_max_bytes &&
	    store.min_free == s.cache_min_free_bytes && store.ttl == s.ttl_seconds) {
		return store.cache.get();
	}
	// Mounted drives hold leases into this store; changed settings wait
	// until the last archive drive is gone.
	if (store.cache && MountedArchiveDrives() > 0) {
		augra::log_warn(LogComponent,
		                "archive cache settings changed; they apply once no archive is mounted");
		return store.cache.get();
	}
	store = {};
	if (const auto problem = CachePathProblem(path); !problem.empty()) {
		*detail = problem;
		return nullptr;
	}
	auto opened = ArchiveCache::Open(path, CacheOptionsFor(s));
	if (opened.status != CacheStatus::Ok) {
		*detail = std::string(CacheStatusText(opened.status)) + ": " +
		          opened.detail;
		return nullptr;
	}
	const auto report = opened.cache->Cleanup();
	augra::log_info(LogComponent,
	                "archive cache %s: %d expired, %d damaged, %d over the cap removed, %d locked kept, %lld bytes in use",
	                PathToUtf8(path).c_str(),
	                report.removed_expired,
	                report.removed_damaged,
	                report.removed_for_cap,
	                report.skipped_locked,
	                static_cast<long long>(report.bytes_after));
	if (cleaned) {
		*cleaned = report;
	}
	store.cache     = std::move(opened.cache);
	store.path      = path;
	store.max_bytes = s.cache_max_bytes;
	store.min_free  = s.cache_min_free_bytes;
	store.ttl       = s.ttl_seconds;
	return store.cache.get();
}

MountVerdict Denied(const DenyReason reason)
{
	MountVerdict v = {};
	v.allowed      = false;
	v.reason       = reason;
	return v;
}

// Design D2 without the lock, shared with the image copies of 4a (D17).
SidecarPlace PlaceSidecar(const std::string& sidecar_path, const std_fs::path& archive,
                          const std::string_view identity_hash,
                          const std::string_view key_stem)
{
	SidecarPlace place = {};
	std::error_code ec = {};
	if (!sidecar_path.empty()) {
		const auto verdict = MountPolicy::ValidateDirectoryMount(
		        std_fs::path(sidecar_path),
		        MountPolicy::ConfAnchor(),
		        MountPolicy::AllowedBases(),
		        MOUNT::CurrentDirPolicy());
		if (!verdict.allowed) {
			place.refusal = verdict;
			return place;
		}
		place.directory = verdict.resolved;
		place.how       = "given";
		return place;
	}
	const auto saves = SavesDirectory();
	std_fs::create_directories(saves, ec);
	std_fs::permissions(saves, std_fs::perms::owner_all, ec);
	auto location = LocateSidecar(saves, archive, identity_hash, key_stem);
	switch (location.kind) {
	case SidecarLocation::Kind::ByPath:
		place.directory = location.directory;
		place.how       = "found by path";
		if (!location.also_matching.empty()) {
			augra::log_info(LogComponent,
			                "sidecar %s also matches by hash, path wins",
			                PathToUtf8(location.also_matching).c_str());
		}
		break;
	case SidecarLocation::Kind::ByHash:
		place.directory = location.directory;
		place.how       = "found by hash";
		break;
	case SidecarLocation::Kind::Create:
		if (!location.beside_archive.empty()) {
			const auto verdict = MountPolicy::ValidateDirectoryMount(
			        archive.parent_path(),
			        MountPolicy::ConfAnchor(),
			        MountPolicy::AllowedBases(),
			        MOUNT::CurrentDirPolicy());
			if (verdict.allowed) {
				place.directory = location.beside_archive;
				place.how       = "found beside the archive";
				break;
			}
			NOTIFY_DisplayWarning(
			        Notification::Source::Console,
			        "MOUNT",
			        "PROGRAM_MOUNT_SIDECAR_SKIPPED",
			        PathToUtf8(location.beside_archive).c_str());
		}
		if (!location.replaced_old.empty()) {
			NOTIFY_DisplayWarning(Notification::Source::Console,
			                      "MOUNT",
			                      "PROGRAM_MOUNT_SIDECAR_REPLACED",
			                      PathToUtf8(location.replaced_old).c_str());
		}
		place.directory = location.directory;
		place.how       = "created";
		place.made_now  = std_fs::create_directory(place.directory, ec);
		if (!place.made_now && ec) {
			place.refusal = Denied(DenyReason::SidecarUnavailable);
			place.detail = "cannot create " + PathToUtf8(place.directory);
			return place;
		}
		std_fs::permissions(place.directory, std_fs::perms::owner_all, ec);
		break;
	}
	return place;
}

ChainOptions ChainOptionsFor(const ArchiveSettings& settings, const Limits& limits,
                             const ArchiveHelperClient::Options& helper)
{
	ChainOptions out       = {};
	out.helper_options     = helper;
	out.limits             = limits;
	out.sequential_allowed = settings.sequential_allowed;
	return out;
}

std::optional<Entered> EnterChain(ArchiveHelperClient& host,
                                  const std_fs::path& host_path,
                                  const std::vector<std::string>& members,
                                  ArchiveCache& cache, const ChainOptions& options,
                                  Error* error, std::string* detail)
{
	assert(!members.empty());
	const auto chain = ChainOfLevels(host_path, members);
	auto result      = ResolveChain(chain, host, host_path, cache, options);
	if (result.error != Error::None || result.links.empty()) {
		if (error) {
			*error = result.error == Error::None ? Error::OpenFailed
			                                     : result.error;
		}
		if (detail) {
			*detail = result.detail;
		}
		return {};
	}
	Entered out = {};
	out.chain   = ChainPrefix(chain);
	out.window  = result.leaf_window;
	out.leaf    = std::move(result.links.back().client);
	out.links   = std::move(result.links);
	return out;
}

// A fresh record for a created sidecar; an existing one is the union's.
bool EnsureSidecarRecord(const std_fs::path& directory, const std_fs::path& archive,
                         const std::string_view identity_hash,
                         const ArchiveSource& source, std::string* detail)
{
	std::error_code ec = {};
	const auto toml    = directory / SidecarTomlName;
	if (std_fs::exists(toml, ec)) {
		return true;
	}
	SidecarRecord record = {};
	record.archive_path  = PathToUtf8(archive);
	record.identity_hash = std::string(identity_hash);
	record.entry_count   = static_cast<int64_t>(source.Entries().size());
	record.total_bytes   = source.TotalBytes();
	record.created       = std::chrono::duration_cast<std::chrono::seconds>(
                                 std::chrono::system_clock::now().time_since_epoch())
	                         .count();
	return record.Save(toml, detail);
}

} // namespace MountArchiveInternal

namespace {

// "game/data/" or "GAME\DATA" as typed to the index's DOS form.
std::string NormalizeRoot(std::string root)
{
	std::replace(root.begin(), root.end(), '/', '\\');
	std::transform(root.begin(), root.end(), root.begin(), [](const unsigned char c) {
		return static_cast<char>(std::toupper(c));
	});
	while (!root.empty() && root.front() == '\\') {
		root.erase(root.begin());
	}
	while (!root.empty() && root.back() == '\\') {
		root.pop_back();
	}
	return root;
}

// A refusal the policy did not see: reported through its hook as well,
// so the tests and the API see one stream of verdicts.
void ReportSidecarDenied(const std_fs::path& raw, const DenyReason reason,
                         const std::string& detail)
{
	const auto denied = Denied(reason);
	if (MountPolicy::verdict_hook) {
		MountPolicy::verdict_hook(raw, denied);
	}
	MOUNT::ReportMountDenied(PathToUtf8(raw), denied, detail);
}

// One sidecar, one mount: the lock is cross-process, this map is the
// in-process half (3.2); keyed by the sidecar's directory.
std::unordered_map<std::string, FileLock> sidecar_locks        = {};
std::unordered_map<char, std::string> sidecar_owner            = {};
std::unordered_map<char, ArchiveCache::Scratch> scratch_mounts = {};

struct ResolvedSidecar {
	std_fs::path directory       = {};
	std::string key              = {};
	const char* how              = "created";
	std::optional<FileLock> lock = {};
};

// Design D2 in MOUNT's hands: the place, then the lock, the key and the record.
std::optional<ResolvedSidecar> ResolveSidecar(const MountParameters& params,
                                              const std_fs::path& archive,
                                              const ArchiveSource& source,
                                              const std_fs::path& raw_archive)
{
	auto place = PlaceSidecar(params.sidecar_path, archive, source.IdentityHash());
	if (place.refusal.reason != DenyReason::None) {
		if (place.refusal.reason == DenyReason::SidecarUnavailable) {
			ReportSidecarDenied(raw_archive,
			                    place.refusal.reason,
			                    place.detail);
		} else {
			MOUNT::ReportMountDenied(params.sidecar_path, place.refusal);
		}
		return {};
	}
	ResolvedSidecar out = {};
	out.directory       = place.directory;
	out.how             = place.how;
	std::error_code ec  = {};
	out.key = std_fs::weakly_canonical(out.directory, ec).string();
	if (sidecar_locks.contains(out.key)) {
		ReportSidecarDenied(raw_archive,
		                    DenyReason::SidecarBusy,
		                    PathToUtf8(out.directory));
		return {};
	}
	out.lock = FileLock::TryAcquire(out.directory / ".lock",
	                                FileLock::Mode::Exclusive);
	if (!out.lock) {
		ReportSidecarDenied(raw_archive,
		                    DenyReason::SidecarBusy,
		                    PathToUtf8(out.directory));
		return {};
	}
	auto error = std::string();
	if (!EnsureSidecarRecord(
	            out.directory, archive, source.IdentityHash(), source, &error)) {
		ReportSidecarDenied(raw_archive, DenyReason::SidecarUnavailable, error);
		return {};
	}
	return out;
}

} // namespace

void MOUNT::ReleaseSidecar(const char drive_letter)
{
	if (const auto scratch = scratch_mounts.find(drive_letter);
	    scratch != scratch_mounts.end()) {
		ArchiveCache::DiscardScratch(scratch->second);
		scratch_mounts.erase(scratch);
	}
	const auto owner = sidecar_owner.find(drive_letter);
	if (owner == sidecar_owner.end()) {
		return;
	}
	sidecar_locks.erase(owner->second);
	sidecar_owner.erase(owner);
}

void MOUNT::KeepScratch(const char drive_letter, ArchiveCache::Scratch&& scratch)
{
	scratch_mounts.insert_or_assign(drive_letter, std::move(scratch));
}

bool MOUNT::EnsureScratch(MountParameters& params, std::string* why)
{
	if (params.scratch_made) {
		return true;
	}
	auto* cache = OpenStore(ReadArchiveSettings(), why);
	if (!cache) {
		return false;
	}
	auto scratch = cache->MakeScratch();
	if (!scratch) {
		*why = "cannot create a scratch directory under the cache";
		return false;
	}
	params.scratch_made = std::move(scratch);
	return true;
}

void MOUNT::ArchiveCacheStartup()
{
	auto detail = std::string();
	if (!OpenStore(ReadArchiveSettings(), &detail)) {
		augra::log_warn(LogComponent,
		                "archive cache not opened at start: %s",
		                detail.c_str());
	}
}

void MOUNT::ResetArchiveCacheForTests()
{
	// A crash does not discard: the locks go, the directories stay.
	scratch_mounts.clear();
	store = {};
}

bool MOUNT::MountArchive(MountParameters& params)
{
	assert(!params.paths.empty());
	if (params.paths.empty()) {
		return false;
	}
	const auto settings = ReadArchiveSettings();
	const auto raw      = std_fs::path(params.paths[0]);
	const auto verdict =
	        MountPolicy::ValidateArchivePath(raw,
	                                         MountOrigin::GuestCommand,
	                                         MountPolicy::AllowedImageRoots(),
	                                         MountPolicy::ConfAnchor(),
	                                         settings.sequential_allowed);
	if (!verdict.verdict.allowed) {
		ReportMountDenied(params.paths[0],
		                  verdict.verdict,
		                  HeadKindName(verdict.sniff.kind));
		return false;
	}
	const auto resolved = verdict.verdict.resolved;
	// Plan 4b: a single-image container mounts as its image, never as a drive.
	if (ArchiveMount::IsSingleImageContainer(resolved)) {
		ReportSidecarDenied(raw,
		                    DenyReason::NotAnArchive,
		                    "a single image: mount it as the image");
		return false;
	}
	params.paths[0] = resolved.string();
	WriteOut(MSG_Get("PROGRAM_MOUNT_ARCHIVE_IDENTIFYING"),
	         resolved.filename().string().c_str());

	auto detail = std::string();
	auto* cache = OpenStore(settings, &detail);
	if (!cache) {
		ReportMountDenied(params.paths[0],
		                  Denied(DenyReason::CacheUnavailable),
		                  detail);
		return false;
	}
	auto scratch = cache->MakeScratch();
	if (!scratch) {
		ReportMountDenied(params.paths[0],
		                  Denied(DenyReason::CacheUnavailable),
		                  "cannot create a scratch directory under the cache");
		return false;
	}
	auto drop_scratch = [&]() {
		scratch->lock.reset();
		std::error_code ec = {};
		std_fs::remove_all(scratch->dir, ec);
	};

	Limits limits                        = {};
	limits.max_entries                   = settings.max_entries;
	limits.max_total_bytes               = settings.max_size_bytes;
	ArchiveHelperClient::Options options = {};
	options.scratch_dir                  = scratch->dir;

	auto opened = ArchiveHelperClient::Open(resolved, limits, options);
	if (opened.error != Error::None) {
		drop_scratch();
		const auto reason = MountPolicy::DenyReasonForArchiveError(
		        opened.error);
		const auto denied = Denied(reason);
		if (MountPolicy::verdict_hook) {
			MountPolicy::verdict_hook(raw, denied);
		}
		ReportMountDenied(params.paths[0], denied, opened.detail);
		return false;
	}

	// N3: a house archive is entered into its one member; the leaf
	// client is the drive's source, the chain's links ride with the drive.
	auto held                             = std::make_shared<ChainHold>();
	std::unique_ptr<ArchiveSource> source = {};
	auto source_path                      = resolved;
	auto house_member                     = std::string();
	if (const auto* house = HouseArchiveMember(opened.client->Entries())) {
		house_member = house->utf8_path;
		auto error   = Error::None;
		auto entered = EnterChain(*opened.client,
		                          resolved,
		                          {house_member},
		                          *cache,
		                          ChainOptionsFor(settings, limits, options),
		                          &error,
		                          &detail);
		if (!entered) {
			drop_scratch();
			const auto denied = Denied(
			        MountPolicy::DenyReasonForArchiveError(error));
			if (MountPolicy::verdict_hook) {
				MountPolicy::verdict_hook(raw, denied);
			}
			ReportMountDenied(params.paths[0], denied, detail);
			return false;
		}
		WriteOut(MSG_Get("PROGRAM_MOUNT_ARCHIVE_ENTERED"),
		         house_member.c_str());
		augra::log_info(LogComponent,
		                "%s: entered %s (N3, %s)",
		                PathToUtf8(resolved).c_str(),
		                house_member.c_str(),
		                entered->window ? "read in place" : "from the cache");
		source      = std::move(entered->leaf);
		held->links = std::move(entered->links);
		source_path = resolved / std_fs::path(house_member);
		opened.client->Close();
		opened.client.reset();
	} else {
		source = std::move(opened.client);
	}

	auto leased = cache->Acquire(*source, source_path);
	if (leased.status != CacheStatus::Ok) {
		drop_scratch();
		ReportMountDenied(params.paths[0],
		                  Denied(DenyReason::CacheUnavailable),
		                  std::string(CacheStatusText(leased.status)) +
		                          ": " + leased.detail);
		return false;
	}

	auto root = NormalizeRoot(params.archive_root);
	if (root.empty()) {
		root = SoloRootDirectory(source->Entries());
		if (!root.empty()) {
			WriteOut(MSG_Get("PROGRAM_MOUNT_ARCHIVE_ROOT_USED"),
			         root.c_str());
			augra::log_info(LogComponent,
			                "%s: the root is %s (D10)",
			                PathToUtf8(source_path).c_str(),
			                root.c_str());
		}
	}
	params.archive_root = root;
	const auto label    = params.label.empty() ? resolved.stem().string()
	                                           : params.label;

	ArchiveDrive::Options drive_options = {};
	drive_options.memory_limit_bytes    = settings.memory_limit_bytes;
	drive_options.owned_scratch_dir     = scratch->dir;
	drive_options.scratch_lock          = scratch->lock;
	drive_options.held                  = held;
	drive_options.reconnect             = ArchiveDrive::ReconnectSpec{};
	drive_options.reconnect->reopen =
	        [resolved, limits, options, settings, house_member](
	                std::string* why) -> ArchiveDrive::Reopened {
		auto again = ArchiveHelperClient::Open(resolved, limits, options);
		if (again.error != Error::None) {
			if (why) {
				*why = std::string(ErrorText(again.error)) +
				       ": " + again.detail;
			}
			return {};
		}
		if (house_member.empty()) {
			return {std::move(again.client), {}};
		}
		// An entered drive re-enters; the drive compares identities and
		// takes the fresh links in place of the old (a changed host has
		// a new cache entry).
		const auto* house = HouseArchiveMember(again.client->Entries());
		if (!house || house->utf8_path != house_member) {
			if (why) {
				*why = "the archive no longer enters " + house_member;
			}
			return {};
		}
		auto store_why = std::string();
		auto* store    = OpenStore(settings, &store_why);
		if (!store) {
			if (why) {
				*why = store_why;
			}
			return {};
		}
		auto error   = Error::None;
		auto entered = EnterChain(*again.client,
		                          resolved,
		                          {house_member},
		                          *store,
		                          ChainOptionsFor(settings, limits, options),
		                          &error,
		                          why);
		if (!entered) {
			return {};
		}
		auto fresh_held   = std::make_shared<ChainHold>();
		fresh_held->links = std::move(entered->links);
		return {std::move(entered->leaf), std::move(fresh_held)};
	};

	auto made = ArchiveDrive::Create(std::move(source),
	                                 std::move(leased.lease),
	                                 root,
	                                 label,
	                                 PathToUtf8(source_path),
	                                 drive_options);
	if (!made.drive) {
		drop_scratch();
		using Failure = ArchiveDrive::CreateResult::Failure;
		switch (made.failure) {
		case Failure::RootNotFound:
			ReportMountDenied(params.paths[0],
			                  Denied(DenyReason::ArchiveRootNotFound),
			                  root);
			break;
		case Failure::TreeInvalid:
			ReportMountDenied(params.paths[0],
			                  Denied(DenyReason::ArchiveCorrupt),
			                  made.error);
			break;
		default:
			ReportMountDenied(params.paths[0],
			                  Denied(DenyReason::CacheUnavailable),
			                  made.error);
			break;
		}
		return false;
	}

	std::shared_ptr<DOS_Drive> to_register = made.drive;
	SidecarRecord identity                 = {};
	identity.archive_path                  = PathToUtf8(resolved);
	identity.identity_hash = made.drive->Source().IdentityHash();
	identity.entry_count   = static_cast<int64_t>(
                made.drive->Source().Entries().size());
	identity.total_bytes = made.drive->Source().TotalBytes();
	if (params.scratch) {
		auto why = std::string();
		if (!EnsureScratch(params, &why)) {
			ReportSidecarDenied(raw, DenyReason::CacheUnavailable, why);
			return false;
		}
		auto error   = std::string();
		auto unioned = UnionDrive::Create(made.drive,
		                                  params.scratch_made->dir,
		                                  params.drive,
		                                  identity,
		                                  &error);
		if (!unioned) {
			ArchiveCache::DiscardScratch(*params.scratch_made);
			params.scratch_made.reset();
			ReportSidecarDenied(raw, DenyReason::CacheUnavailable, error);
			return false;
		}
		augra::log_info(LogComponent,
		                "%s mounted with scratch %s",
		                PathToUtf8(resolved).c_str(),
		                PathToUtf8(params.scratch_made->dir).c_str());
		KeepScratch(params.drive, std::move(*params.scratch_made));
		params.scratch_made.reset();
		to_register = unioned;
	} else if (!params.no_sidecar && !params.roflag) {
		auto sidecar = ResolveSidecar(params, resolved, made.drive->Source(), raw);
		if (!sidecar) {
			return false; // reported inside
		}
		auto error   = std::string();
		auto unioned = UnionDrive::Create(made.drive,
		                                  sidecar->directory,
		                                  params.drive,
		                                  identity,
		                                  &error);
		if (!unioned) {
			ReportSidecarDenied(raw, DenyReason::SidecarUnavailable, error);
			return false;
		}
		sidecar_locks.emplace(sidecar->key, std::move(*sidecar->lock));
		sidecar_owner[params.drive] = sidecar->key;
		augra::log_info(LogComponent,
		                "%s mounted with sidecar %s (%s)",
		                PathToUtf8(resolved).c_str(),
		                PathToUtf8(sidecar->directory).c_str(),
		                sidecar->how);
		to_register = unioned;
	}
	const auto idx = drive_index(params.drive);
	DriveManager::RegisterFilesystemImage(idx, to_register);
	Drives.at(idx) = to_register;
	mem_writeb(RealToPhysical(dos.tables.mediaid) + idx * 9,
	           to_register->GetMediaByte());
	WriteOut(MSG_Get("PROGRAM_MOUNT_STATUS_2"),
	         to_register->GetInfoString().c_str(),
	         params.drive);
	if (params.no_sidecar || params.roflag) {
		WriteOut(MSG_Get("PROGRAM_MOUNT_READONLY"));
	}
	if (params.scratch) {
		WriteOut(MSG_Get("PROGRAM_MOUNT_ARCHIVE_SCRATCH"), params.drive);
	}
	return true;
}

// The status lines are built here and shown as they are: the test binary
// links the message stubs, so a text a test must see lives in the code.
std::string MOUNT::CacheOverviewText(const ArchiveManagement::CacheOverview& overview)
{
	const auto mib = [](const int64_t bytes) {
		return std::to_string(bytes / MiB);
	};
	auto text = "Archive cache: " + PathToUtf8(overview.directory) + "\n";
	if (!overview.problem.empty()) {
		return text + "  " + overview.problem + "\n";
	}
	if (!overview.exists) {
		return text + "  no cache yet\n";
	}
	text += "  " + mib(overview.bytes) + " MiB of " +
	        mib(overview.cap_bytes) + " MiB used";
	if (overview.free_bytes) {
		text += ", " + mib(*overview.free_bytes) + " MiB free (" +
		        mib(overview.min_free_bytes) + " MiB kept)";
	}
	text += ", unused entries live " +
	        std::to_string(overview.ttl_seconds / (24 * 60 * 60)) + " days\n";
	auto entries = overview.entries;
	std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
		return a.last_use > b.last_use;
	});
	const auto now = static_cast<int64_t>(std::time(nullptr));
	for (const auto& e : entries) {
		text += "  " + PathToUtf8(e.directory.filename()) + "  " +
		        mib(e.bytes) + " MiB  ";
		if (e.damaged) {
			text += "damaged";
		} else {
			const auto days = std::max<int64_t>(0,
			                                    (now - e.last_use) /
			                                            (24 * 60 * 60));
			text += std::to_string(days) + " days ago  " + e.archive_path;
		}
		text += (e.locked ? "  (in use)" : "") + std::string("\n");
	}
	if (entries.empty()) {
		text += "  no entries\n";
	}
	return text;
}

std::string MOUNT::CacheOverviewForSettings()
{
	const auto settings = ReadArchiveSettings();
	const auto path     = EffectiveCachePath(settings);
	if (const auto problem = CachePathProblem(path); !problem.empty()) {
		ArchiveManagement::CacheOverview refused = {};
		refused.directory                        = path;
		refused.problem                          = problem;
		return CacheOverviewText(refused);
	}
	return CacheOverviewText(
	        ArchiveManagement::InspectCache(path, CacheOptionsFor(settings)));
}

std::optional<ArchiveCache::CleanupReport> MOUNT::TrimCache(std::string* detail)
{
	auto cleaned = std::optional<ArchiveCache::CleanupReport>();
	auto* store  = OpenStore(ReadArchiveSettings(), detail, &cleaned);
	if (!store) {
		return {};
	}
	return cleaned ? *cleaned : store->Cleanup();
}

bool MOUNT::HandleCache()
{
	if (!cmd->FindExist("-cache", false)) {
		return false;
	}
	std::string word = {};
	if (!cmd->FindString("-cache", word, false)) {
		word.clear();
	}
	if (word.empty()) {
		// Not WriteOut: its 16 KiB format buffer cuts a long listing.
		WriteOut_NoParsing(CacheOverviewForSettings());
		return true;
	}
	if (iequals(word, "trim")) {
		auto detail       = std::string();
		const auto report = TrimCache(&detail);
		if (!report) {
			WriteOut(MSG_Get("PROGRAM_MOUNT_CACHE_UNAVAILABLE"),
			         detail.c_str());
			return true;
		}
		WriteOut(MSG_Get("PROGRAM_MOUNT_CACHE_TRIMMED"),
		         report->removed_expired,
		         report->removed_for_cap,
		         report->removed_damaged,
		         report->skipped_locked,
		         static_cast<long long>(report->bytes_after / MiB));
		return true;
	}
	WriteOut(MSG_Get("PROGRAM_MOUNT_CACHE_BAD_ARGUMENT"), word.c_str());
	return true;
}

bool MOUNT::HandleReconnect()
{
	std::string target = {};
	if (!cmd->FindString("-reconnect", target, false)) {
		return false;
	}
	if (target.empty()) {
		WriteOut(MSG_Get("PROGRAM_MOUNT_DRIVEID_ERROR"));
		return true;
	}
	const char letter = static_cast<char>(
	        std::toupper(static_cast<unsigned char>(target[0])));
	if (letter < 'A' || letter > 'Z') {
		WriteOut(MSG_Get("PROGRAM_MOUNT_DRIVEID_ERROR"));
		return true;
	}
	const auto drive = ArchiveDriveOf(Drives.at(drive_index(letter)));
	if (!drive) {
		WriteOut(MSG_Get("PROGRAM_MOUNT_ARCHIVE_NOT_ARCHIVE"), letter);
		return true;
	}
	auto detail = std::string();
	if (drive->Reconnect(&detail)) {
		WriteOut(MSG_Get("PROGRAM_MOUNT_ARCHIVE_RECONNECTED"), letter);
	} else {
		WriteOut(MSG_Get("PROGRAM_MOUNT_ARCHIVE_RECONNECT_FAILED"),
		         letter,
		         detail.c_str());
	}
	return true;
}
