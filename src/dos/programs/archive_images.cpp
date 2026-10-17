// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//
#include "dos/programs/archive_images.h"
#include "dos/archive/archive_tree.h"

#include "augra/log.h"
#include "dos/archive/archive_helper_client.h"
#include "dos/archive/chain_resolver.h"
#include "dos/archive/cue_rewrite.h"
#include "dos/archive/descriptor.h"
#include "dos/archive/exclusive_file.h"
#include "dos/archive/head_sniff.h"
#include "dos/archive/random_name.h"
#include "dos/archive/sidecar_locator.h"
#include "dos/programs/mount_archive_internal.h"
#include "utils/checks.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <map>

#if defined(WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

CHECK_NARROWING();

using namespace ArchiveMount;
using namespace MountArchiveInternal;

namespace ArchiveImages {

namespace {

namespace fs = std::filesystem;

constexpr auto LogComponent  = "mount";
constexpr auto ImagesDirName = ".images";

std::map<int, std::vector<std::shared_ptr<Hold>>> held = {};
std::vector<std::shared_ptr<Hold>> boot_held           = {};
std::map<int, DriveMode> drive_modes                   = {};

std::string lower_extension(const std::string_view name)
{
	const auto dot   = name.rfind('.');
	const auto slash = name.find_last_of("/\\");
	if (dot == std::string_view::npos ||
	    (slash != std::string_view::npos && dot < slash)) {
		return {};
	}
	auto ext = std::string(name.substr(dot + 1));
	std::transform(ext.begin(), ext.end(), ext.begin(), [](const unsigned char c) {
		return static_cast<char>(std::tolower(c));
	});
	return ext;
}

// Creates base/rel's directories one at a time, 0700, and refuses any
// component that exists as something other than a plain directory (3.2's
// rule for the sidecar, applied to .images).
bool make_plain_directories(const fs::path& base, const fs::path& rel,
                            std::string* detail,
                            std::vector<fs::path>* created = nullptr)
{
	auto dir = base;
	for (const auto& part : rel) {
		dir /= part;
		std::error_code ec = {};
		const auto status  = fs::symlink_status(dir, ec);
		if (fs::exists(status)) {
			if (!fs::is_directory(status)) {
				*detail = PathToUtf8(dir) + " is not a plain directory";
				return false;
			}
			continue;
		}
		if (!fs::create_directory(dir, ec) || ec) {
			*detail = "cannot create " + PathToUtf8(dir);
			return false;
		}
		fs::permissions(dir, fs::perms::owner_all, ec);
		if (created) {
			created->push_back(dir);
		}
	}
	return true;
}

// What one placement made in a sidecar or scratch, removed again unless
// the placement is kept: a refused member leaves no trace (ada-lm3d).
class PlacementUndo {
public:
	PlacementUndo()                                = default;
	PlacementUndo(const PlacementUndo&)            = delete;
	PlacementUndo& operator=(const PlacementUndo&) = delete;
	~PlacementUndo()
	{
		if (!kept) {
			Undo();
		}
	}
	void Keep()
	{
		kept = true;
	}

	fs::path new_sidecar               = {};
	std::vector<fs::path> files        = {};
	std::vector<fs::path> created_dirs = {};
	// A lock file this placement created and holds; never another's.
	std::optional<FileLock>* lock = nullptr;
	fs::path lock_file            = {};

private:
	void Undo() const
	{
		std::error_code ec = {};
		if (lock && !lock_file.empty()) {
#if defined(WIN32)
			// No FILE_SHARE_DELETE: a held file cannot go,
			// nor one another engine opened meanwhile.
			lock->reset();
			fs::remove(lock_file, ec);
#else
			// Unlinked while held: nobody takes the old inode.
			fs::remove(lock_file, ec);
			lock->reset();
#endif
		}
		for (const auto& file : files) {
			if (fs::is_regular_file(fs::symlink_status(file, ec))) {
				fs::remove(file, ec);
			}
		}
		for (auto dir = created_dirs.rbegin(); dir != created_dirs.rend();
		     ++dir) {
			fs::remove(*dir, ec); // only when empty
		}
		if (!new_sidecar.empty()) {
			fs::remove_all(new_sidecar, ec);
		}
	}
	bool kept = false;
};

DenyReason reason_for(const CacheStatus status, const ArchiveCache::Lease& lease)
{
	return status == CacheStatus::SourceFailed
	             ? MountPolicy::DenyReasonForArchiveError(lease.LastSourceError())
	             : DenyReason::CacheUnavailable;
}

// Up to max_bytes of a regular file; nothing on any failure.
std::optional<std::string> read_whole_file(const fs::path& path, const size_t max_bytes)
{
	const int fd = OpenRegularFileNoFollow(path);
	if (fd < 0) {
		return {};
	}
	std::string text = {};
	char buffer[4096];
	while (text.size() <= max_bytes) {
#if defined(WIN32)
		const auto got = _read(fd,
		                       buffer,
		                       static_cast<unsigned>(sizeof(buffer)));
#else
		const auto got = read(fd, buffer, sizeof(buffer));
#endif
		if (got < 0) {
			CloseDescriptor(fd);
			return {};
		}
		if (got == 0) {
			break;
		}
		text.append(buffer, static_cast<size_t>(got));
	}
	CloseDescriptor(fd);
	if (text.size() > max_bytes) {
		return {};
	}
	return text;
}

// The rewritten sheet as <ordinal>.cue beside the cached tracks (ada-it53:
// the loader takes companions only from the sheet's own directory), written
// through a temporary and a rename; the same text already there is reused.
bool write_cue_beside_tracks(const fs::path& cue_path, const std::string& text,
                             std::string* detail)
{
	if (const auto present = read_whole_file(cue_path, text.size());
	    present && *present == text) {
		return true;
	}
	auto temp = cue_path;
	temp += "." + RandomHex(4) + ".tmp";
	FILE* f = OpenExclusive(temp);
	if (!f) {
		*detail = "cannot create " + PathToUtf8(temp);
		return false;
	}
	const bool written = std::fwrite(text.data(), 1, text.size(), f) ==
	                     text.size();
	const bool closed  = std::fclose(f) == 0;
	std::error_code ec = {};
	if (!written || !closed) {
		fs::remove(temp, ec);
		*detail = "cannot write " + PathToUtf8(temp);
		return false;
	}
	fs::rename(temp, cue_path, ec);
	if (ec) {
		fs::remove(temp, ec);
		*detail = "cannot rename into " + PathToUtf8(cue_path);
		return false;
	}
	return true;
}

// Copies a cache file to target through a temporary beside it, with the
// source's mtime; target must not exist (the caller holds its lock).
bool copy_with_date(const fs::path& from, const fs::path& target, std::string* detail)
{
	std::error_code ec = {};
	auto temp          = target;
	temp += "." + RandomHex(4) + ".part";
	if (!fs::copy_file(from, temp, ec) || ec) {
		fs::remove(temp, ec);
		*detail = "cannot copy into " + PathToUtf8(temp);
		return false;
	}
	const auto stamp = fs::last_write_time(from, ec);
	if (!ec) {
		fs::last_write_time(temp, stamp, ec);
	}
	if (ec) {
		augra::log_warn(LogComponent,
		                "%s: cannot carry the member's date over (%s)",
		                PathToUtf8(target).c_str(),
		                ec.message().c_str());
	}
	fs::rename(temp, target, ec);
	if (ec) {
		fs::remove(temp, ec);
		*detail = "cannot rename into " + PathToUtf8(target);
		return false;
	}
	return true;
}

// The member's spelling stays out of the key: a held member is matched by
// the entry FindMember resolves, so every spelling of it finds the same
// copy (ada-ciru item 2).
std::string hold_key(const Request& request)
{
	return std::string(request.member.lone ? "lone" : "member") + '\0' +
	       (request.read_only ? "ro" : "rw") + '\0' + request.sidecar_path +
	       '\0' + PathToUtf8(request.scratch_dir);
}

// The entry a request names in a listing: a container's only regular
// file (plan 4b), or the member by FindMember's rules. files counts the
// container's candidates for the refusal text.
const Entry* pick_entry(const std::vector<Entry>& entries, const Request& request,
                        ArchiveNames::Rejection* rejection, int* files)
{
	if (!request.member.lone) {
		return FindMember(entries, request.member.member, rejection);
	}
	const Entry* only = nullptr;
	*files            = 0;
	for (const auto& candidate : entries) {
		if (!candidate.is_dir && candidate.ordinal >= 0) {
			++*files;
			only = &candidate;
		}
	}
	return *files == 1 ? only : nullptr;
}

// A member the drive already holds, picked from the hold's own listing so
// no helper runs for a swap back (ada-ciru item 2, round 2).
std::shared_ptr<Hold> find_held(const int drive_index, const fs::path& archive,
                                const std::string& key, const Request& request)
{
	const auto found = held.find(drive_index);
	if (found == held.end()) {
		return {};
	}
	// A hold's listing is the leaf archive's: the chain must match before
	// the leaf member is looked up in it.
	const auto chained = SplitChain(request.member);
	if (!chained) {
		return {};
	}
	const auto chain  = ChainPrefix(*chained);
	auto leaf_request = request;
	if (!chained->levels.empty()) {
		leaf_request.member.member = chained->levels.back().leaf_member;
	}
	for (const auto& hold : found->second) {
		// A short path through a house archive (N4, G11, G16) leaves
		// out the store: the hold matches on the chain it was asked by.
		const bool chain_matches = hold && (hold->chain == chain ||
		                                    hold->spelled_chain == chain);
		if (!hold || !hold->entries || hold->archive != archive ||
		    hold->key != key || !chain_matches) {
			continue;
		}
		auto rejection   = ArchiveNames::Rejection::None;
		auto files       = 0;
		const auto* pick = pick_entry(*hold->entries,
		                              leaf_request,
		                              &rejection,
		                              &files);
		if (pick && pick->ordinal == hold->ordinal &&
		    pick->utf8_path == hold->member_name) {
			return hold;
		}
	}
	return {};
}

// A request naming a drive takes the drive's mode: -ro and -nosidecar hold
// through swaps, and a named sidecar keeps every disk of the same archive.
Request with_drive_mode(const Request& given, const fs::path& archive)
{
	auto request = given;
	if (!given.drive_index) {
		return request;
	}
	const auto mode = DriveModeOf(*given.drive_index);
	if (!mode) {
		return request;
	}
	request.read_only = given.read_only || mode->read_only;
	if (request.sidecar_path.empty() && mode->archive == archive) {
		request.sidecar_path = mode->sidecar_path;
	}
	// A scratch drive takes every disk, whatever its archive (Phase 6.4).
	if (request.scratch_dir.empty()) {
		request.scratch_dir = mode->scratch_dir;
	}
	return request;
}

void report_allowed(const fs::path& host_file)
{
	// The archive passed the policy above and the file is the engine's own
	// copy; the construction invariant of the mount tests matches on this.
	if (MountPolicy::verdict_hook) {
		std::error_code ec   = {};
		MountVerdict allowed = {};
		allowed.allowed      = true;
		allowed.resolved     = fs::canonical(host_file, ec);
		MountPolicy::verdict_hook(host_file, allowed);
	}
}

} // namespace

Hold::~Hold()
{
	lease.reset();
	while (!chain_links.empty()) {
		chain_links.pop_back();
	}
	image_lock.reset();
	if (!scratch_dir.empty()) {
		scratch_lock.reset();
		std::error_code ec = {};
		fs::remove_all(scratch_dir, ec);
	}
}

Kind KindOf(const std::string_view member_name)
{
	const auto ext = lower_extension(member_name);
	if (ext == "iso" || ext == "cue" || ext == "bin") {
		return Kind::CdImage;
	}
	if (ext == "mds" || ext == "ccd") {
		return Kind::CdCompanion;
	}
	return Kind::DiskImage;
}

Result Resolve(const Request& given)
{
	Result out                 = {};
	std::shared_ptr<Hold> hold = {};
	// A refusal after Acquire drops the lease before the client goes.
	auto refuse = [&](const DenyReason reason, std::string detail) {
		if (hold) {
			hold->lease.reset();
		}
		out.verdict = Denied(reason);
		out.detail  = std::move(detail);
		if (MountPolicy::verdict_hook) {
			MountPolicy::verdict_hook(given.member.archive, out.verdict);
		}
		return out;
	};
	const auto settings = ReadArchiveSettings();
	const auto checked =
	        MountPolicy::ValidateArchivePath(given.member.archive,
	                                         given.origin,
	                                         given.allowed_image_roots,
	                                         given.conf_anchor,
	                                         settings.sequential_allowed);
	if (!checked.verdict.allowed) {
		out.verdict = checked.verdict;
		out.detail  = HeadKindName(checked.sniff.kind);
		return out;
	}
	const auto archive = checked.verdict.resolved;
	const auto request = with_drive_mode(given, archive);
	const auto key     = hold_key(request);
	if (request.drive_index) {
		if (auto again = find_held(*request.drive_index, archive, key, request)) {
			report_allowed(again->host_file);
			Resolved resolved    = {};
			resolved.host_file   = again->host_file;
			resolved.member_name = again->member_name;
			resolved.size        = again->size;
			resolved.read_only   = !again->image_lock;
			resolved.hold        = std::move(again);
			out.resolved         = std::move(resolved);
			return out;
		}
	}

	auto why    = std::string();
	auto* cache = OpenStore(settings, &why);
	if (!cache) {
		return refuse(DenyReason::CacheUnavailable, why);
	}
	auto scratch = cache->MakeScratch();
	if (!scratch) {
		return refuse(DenyReason::CacheUnavailable,
		              "cannot create a scratch directory under the cache");
	}
	hold               = std::make_shared<Hold>();
	hold->scratch_dir  = scratch->dir;
	hold->scratch_lock = scratch->lock;

	Limits limits                        = {};
	limits.max_entries                   = settings.max_entries;
	limits.max_total_bytes               = settings.max_size_bytes;
	ArchiveHelperClient::Options options = {};
	options.scratch_dir                  = scratch->dir;
	auto opened = ArchiveHelperClient::Open(archive, limits, options);
	if (opened.error != Error::None) {
		return refuse(MountPolicy::DenyReasonForArchiveError(opened.error),
		              opened.detail);
	}
	auto& client = *opened.client;

	// Nested archives: split the member path into levels and resolve
	// through the cache. The leaf client replaces the host client for
	// the entry lookup below.
	auto* resolve_client = &client;
	auto resolve_archive = archive;
	auto leaf_request    = request;
	const auto chained   = SplitChain(request.member);
	if (!chained) {
		return refuse(DenyReason::ArchiveEntryRejected,
		              "nesting depth exceeds " +
		                      std::to_string(MaxNestingDepth));
	}
	// The house rule (N4) runs in the chain walk, at the host too.
	ChainOptions chain_opts       = {};
	chain_opts.helper_options     = options;
	chain_opts.limits             = limits;
	chain_opts.sequential_allowed = settings.sequential_allowed;
	auto chain_result = ResolveChain(*chained, client, archive, *cache, chain_opts);
	if (chain_result.error != Error::None) {
		return refuse(MountPolicy::DenyReasonForArchiveError(
		                      chain_result.error),
		              chain_result.detail);
	}
	const auto& effective = chain_result.chain;
	if (!effective.levels.empty()) {
		resolve_client      = chain_result.leaf_client;
		resolve_archive     = chain_result.leaf_archive;
		hold->chain_links   = std::move(chain_result.links);
		hold->chain         = ChainPrefix(effective);
		hold->spelled_chain = ChainPrefix(*chained);
		hold->house_member  = chain_result.house_member;
		augra::log_info(LogComponent,
		                "%s: nested chain %s, %zu level(s), leaf %s",
		                PathToUtf8(archive).c_str(),
		                hold->chain.c_str(),
		                hold->chain_links.size(),
		                chain_result.leaf_window ? "read in place"
		                                         : "from the cache");
		leaf_request.member = MemberPath{resolve_archive,
		                                 effective.levels.back().leaf_member};
	}

	auto entries = std::make_shared<const std::vector<Entry>>(
	        resolve_client->Entries());
	auto rejection = ArchiveNames::Rejection::None;
	auto files     = 0;
	const Entry* entry = pick_entry(*entries, leaf_request, &rejection, &files);
	if (!entry) {
		if (request.member.lone) {
			return refuse(DenyReason::ArchiveMemberNotFound,
			              std::to_string(files) +
			                      " files in the container, one expected");
		}
		if (rejection != ArchiveNames::Rejection::None) {
			return refuse(DenyReason::ArchiveEntryRejected,
			              request.member.member + ": " +
			                      ArchiveNames::RejectionText(rejection));
		}
		return refuse(DenyReason::ArchiveMemberNotFound,
		              request.member.member);
	}
	const auto kind = KindOf(entry->utf8_path);
	if (kind == Kind::CdCompanion) {
		return refuse(DenyReason::ArchiveCueRejected,
		              entry->utf8_path +
		                      ": only ISO and CUE/BIN images are served from an archive");
	}

	Resolved resolved    = {};
	resolved.member_name = entry->utf8_path;
	resolved.size        = entry->size;

	if (kind == Kind::CdImage || leaf_request.read_only) {
		auto leased = cache->Acquire(*resolve_client, resolve_archive);
		if (leased.status != CacheStatus::Ok) {
			return refuse(DenyReason::CacheUnavailable,
			              std::string(CacheStatusText(leased.status)) +
			                      ": " + leased.detail);
		}
		hold->lease = std::move(leased.lease);
		auto detail = std::string();
		auto served = fs::path();
		if (const auto status = hold->lease->ServeMember(entry->ordinal,
		                                                 served,
		                                                 &detail);
		    status != CacheStatus::Ok) {
			return refuse(reason_for(status, *hold->lease), detail);
		}
		resolved.host_file = served;
		if (lower_extension(entry->utf8_path) == "cue") {
			if (entry->size > MaxCueSheetBytes) {
				return refuse(DenyReason::ArchiveCueRejected,
				              entry->utf8_path + ": over " +
				                      std::to_string(MaxCueSheetBytes) +
				                      " bytes");
			}
			const auto text = read_whole_file(
			        served, static_cast<size_t>(MaxCueSheetBytes));
			if (!text) {
				return refuse(DenyReason::CacheUnavailable,
				              "cannot read " + PathToUtf8(served));
			}
			const auto slash   = entry->utf8_path.rfind('/');
			const auto cue_dir = slash == std::string::npos
			                           ? std::string()
			                           : entry->utf8_path.substr(0, slash + 1);
			auto failure       = std::string();
			const CueFileResolver resolve_file =
			        [&](const std::string_view name) -> std::optional<fs::path> {
				auto ignored = ArchiveNames::Rejection::None;
				const auto* file =
				        FindMember(resolve_client->Entries(),
				                   cue_dir + std::string(name),
				                   &ignored);
				if (!file) {
					return {};
				}
				auto host = fs::path();
				if (hold->lease->ServeMember(file->ordinal, host, &failure) !=
				    CacheStatus::Ok) {
					return {};
				}
				return host.filename();
			};
			const auto rewritten = RewriteCueSheet(*text, resolve_file);
			if (!rewritten.ok) {
				return refuse(DenyReason::ArchiveCueRejected,
				              entry->utf8_path + ": " +
				                      rewritten.refused +
				                      (failure.empty()
				                               ? ""
				                               : " (" + failure + ")"));
			}
			const auto cue_path = hold->lease->FilesDirectory() /
			                      (std::to_string(entry->ordinal) + ".cue");
			if (!write_cue_beside_tracks(cue_path, rewritten.text, &detail)) {
				return refuse(DenyReason::CacheUnavailable, detail);
			}
			resolved.host_file = cue_path;
		}
		hold->lease->Detach();
	} else {
		auto place_dir  = request.scratch_dir;
		const char* how = "scratch";
		auto detail     = std::string();
		// One subdirectory per archive: a scratch takes every disk of
		// its drive, and two archives can name the same member path.
		auto per_archive = fs::path();
		auto lock        = std::optional<FileLock>();
		auto undo        = PlacementUndo();
		undo.lock        = &lock;
		// N5: a nested member's sidecar carries the leaf archive's
		// identity under the host file's name; the record names the
		// host and the chain (outer.zip/inner.zip) for the lister.
		const auto sidecar_archive = ArchiveMount::PathWithChain(archive,
		                                                         hold->chain);
		const auto& sidecar_identity = resolve_client->IdentityHash();
		const auto sidecar_stem      = PathToUtf8(archive.stem());
		if (!place_dir.empty()) {
			per_archive = SidecarKeyForStem(sidecar_stem,
			                                sidecar_identity);
		} else {
			auto place = PlaceSidecar(leaf_request.sidecar_path,
			                          sidecar_archive,
			                          sidecar_identity,
			                          sidecar_stem);
			if (place.refusal.reason != DenyReason::None) {
				return refuse(place.refusal.reason, place.detail);
			}
			if (place.made_now) {
				undo.new_sidecar = place.directory;
			}
			if (!EnsureSidecarRecord(place.directory,
			                         sidecar_archive,
			                         sidecar_identity,
			                         *resolve_client,
			                         &detail)) {
				return refuse(DenyReason::SidecarUnavailable, detail);
			}
			place_dir = place.directory;
			how       = place.how;
		}
		auto dos = entry->dos_path;
		std::replace(dos.begin(), dos.end(), '\\', '/');
		const auto rel = per_archive / fs::path(ImagesDirName) /
		                 fs::path(dos);
		const auto target = place_dir / rel;
		if (!make_plain_directories(place_dir,
		                            rel.parent_path(),
		                            &detail,
		                            &undo.created_dirs)) {
			return refuse(DenyReason::SidecarUnavailable, detail);
		}
		auto lock_path = target;
		lock_path += ".lock";
		const bool lock_was_there = fs::exists(lock_path);
		lock = FileLock::TryAcquire(lock_path, FileLock::Mode::Exclusive);
		if (!lock) {
			return refuse(DenyReason::SidecarBusy, PathToUtf8(target));
		}
		if (!lock_was_there) {
			undo.lock_file = lock_path;
		}
		// A new copy is written beside the target and checked before it
		// takes the target's name.
		auto staged = target;
		staged += ".new";
		undo.files.push_back(staged);
		std::error_code ec = {};
		const auto status  = fs::symlink_status(target, ec);
		const bool fresh   = !fs::exists(status);
		if (!fresh) {
			if (!fs::is_regular_file(status)) {
				return refuse(DenyReason::SidecarUnavailable,
				              PathToUtf8(target) +
				                      " is not a regular file");
			}
		} else if (resolve_client->GetAccess() == Access::PerMember) {
			if (const auto error = resolve_client->MaterializeToFile(
			            entry->ordinal, staged, &detail);
			    error != Error::None) {
				return refuse(MountPolicy::DenyReasonForArchiveError(
				                      error),
				              detail);
			}
		} else {
			auto leased = cache->Acquire(*resolve_client, resolve_archive);
			if (leased.status != CacheStatus::Ok) {
				return refuse(DenyReason::CacheUnavailable,
				              std::string(CacheStatusText(leased.status)) +
				                      ": " + leased.detail);
			}
			auto served = fs::path();
			if (const auto served_status = leased.lease->ServeMember(
			            entry->ordinal, served, &detail);
			    served_status != CacheStatus::Ok) {
				return refuse(reason_for(served_status, *leased.lease),
				              detail);
			}
			if (!copy_with_date(served, staged, &detail)) {
				return refuse(DenyReason::SidecarUnavailable, detail);
			}
		}
		if (fresh) {
			if (!MountPolicy::ValidateDiskImageStructure(staged)) {
				return refuse(DenyReason::NotADiskImage,
				              entry->utf8_path + ": not a disk image");
			}
			fs::rename(staged, target, ec);
			if (ec) {
				return refuse(DenyReason::SidecarUnavailable,
				              "cannot rename " + PathToUtf8(staged) +
				                      ": " + ec.message());
			}
		}
		undo.Keep();
		hold->image_lock   = std::move(lock);
		resolved.host_file = target;
		augra::log_info(LogComponent,
		                "%s: %s in %s (%s)",
		                PathToUtf8(archive).c_str(),
		                entry->utf8_path.c_str(),
		                PathToUtf8(place_dir).c_str(),
		                how);
	}
	client.Close();
	for (auto& link : hold->chain_links) {
		link.client->Close();
		link.client.reset();
	}
	report_allowed(resolved.host_file);
	hold->archive      = archive;
	hold->key          = key;
	hold->host_file    = resolved.host_file;
	hold->member_name  = resolved.member_name;
	hold->ordinal      = entry->ordinal;
	hold->size         = resolved.size;
	hold->entries      = std::move(entries);
	resolved.read_only = !hold->image_lock;
	resolved.hold      = std::move(hold);
	out.resolved       = std::move(resolved);
	return out;
}

void Keep(const int drive_index, std::vector<std::shared_ptr<Hold>> holds)
{
	auto& list = held[drive_index];
	for (auto& hold : holds) {
		if (std::find(list.begin(), list.end(), hold) == list.end()) {
			list.push_back(std::move(hold));
		}
	}
}

void Release(const int drive_index)
{
	held.erase(drive_index);
	drive_modes.erase(drive_index);
}

void SetDriveMode(const int drive_index, const DriveMode& mode)
{
	drive_modes[drive_index] = mode;
}

std::optional<DriveMode> DriveModeOf(const int drive_index)
{
	const auto found = drive_modes.find(drive_index);
	if (found == drive_modes.end()) {
		return {};
	}
	return found->second;
}

void KeepForBoot(std::vector<std::shared_ptr<Hold>> holds)
{
	// In boot_held too, so an unmount of A or B cannot release what the
	// booted system uses.
	Keep(0, holds);
	Keep(1, holds);
	for (auto& hold : holds) {
		boot_held.push_back(std::move(hold));
	}
}

size_t HeldCountForTests(const int drive_index)
{
	const auto found = held.find(drive_index);
	return found == held.end() ? 0 : found->second.size();
}

void ReleaseAllForTests()
{
	held.clear();
	boot_held.clear();
	drive_modes.clear();
}

} // namespace ArchiveImages
