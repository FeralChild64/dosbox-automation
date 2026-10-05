// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//
#include "dos/programs/archive_images.h"

#include "augra/log.h"
#include "dos/archive/archive_helper_client.h"
#include "dos/archive/cue_rewrite.h"
#include "dos/archive/descriptor.h"
#include "dos/archive/exclusive_file.h"
#include "dos/archive/head_sniff.h"
#include "dos/archive/random_name.h"
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
                            std::string* detail)
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
	}
	return true;
}

// The cache's file for a member: one pass for a sequential source.
CacheStatus serve_from_cache(ArchiveCache::Lease& lease,
                             const ArchiveSource& source, const Entry& entry,
                             fs::path& out, std::string* detail)
{
	if (source.GetAccess() == Access::Sequential) {
		const auto status = lease.EnsureAll(detail);
		if (status == CacheStatus::Ok) {
			out = lease.FilesDirectory() / std::to_string(entry.ordinal);
		}
		return status;
	}
	return lease.MemberFile(entry.ordinal, out, detail);
}

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

std::string hold_key(const Request& request)
{
	return request.member.member + '\0' +
	       (request.member.lone ? "lone" : "member") + '\0' +
	       (request.read_only ? "ro" : "rw") + '\0' + request.sidecar_path;
}

std::shared_ptr<Hold> find_held(const int drive_index, const fs::path& archive,
                                const std::string& key)
{
	const auto found = held.find(drive_index);
	if (found == held.end()) {
		return {};
	}
	for (const auto& hold : found->second) {
		if (hold && hold->archive == archive && hold->key == key) {
			return hold;
		}
	}
	return {};
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

Result Resolve(const Request& request)
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
			MountPolicy::verdict_hook(request.member.archive, out.verdict);
		}
		return out;
	};
	const auto settings = ReadArchiveSettings();
	const auto checked =
	        MountPolicy::ValidateArchivePath(request.member.archive,
	                                         request.origin,
	                                         request.allowed_image_roots,
	                                         request.conf_anchor,
	                                         settings.sequential_allowed);
	if (!checked.verdict.allowed) {
		out.verdict = checked.verdict;
		out.detail  = HeadKindName(checked.sniff.kind);
		return out;
	}
	const auto archive = checked.verdict.resolved;
	const auto key     = hold_key(request);
	if (request.drive_index) {
		if (auto again = find_held(*request.drive_index, archive, key)) {
			report_allowed(again->host_file);
			Resolved resolved    = {};
			resolved.host_file   = again->host_file;
			resolved.member_name = again->member_name;
			resolved.size        = again->size;
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

	const Entry* entry = nullptr;
	if (request.member.lone) {
		// Plan 4b: a one-member container serves its only regular file.
		auto files = 0;
		for (const auto& candidate : client.Entries()) {
			if (!candidate.is_dir && candidate.ordinal >= 0) {
				++files;
				entry = &candidate;
			}
		}
		if (files != 1) {
			return refuse(DenyReason::ArchiveMemberNotFound,
			              std::to_string(files) +
			                      " files in the container, one expected");
		}
	} else {
		auto rejection = ArchiveNames::Rejection::None;
		entry = FindMember(client.Entries(), request.member.member, &rejection);
		if (!entry) {
			if (rejection != ArchiveNames::Rejection::None) {
				return refuse(DenyReason::ArchiveEntryRejected,
				              request.member.member + ": " +
				                      ArchiveNames::RejectionText(
				                              rejection));
			}
			return refuse(DenyReason::ArchiveMemberNotFound,
			              request.member.member);
		}
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

	if (kind == Kind::CdImage || request.read_only) {
		auto leased = cache->Acquire(client, archive);
		if (leased.status != CacheStatus::Ok) {
			return refuse(DenyReason::CacheUnavailable,
			              std::string(CacheStatusText(leased.status)) +
			                      ": " + leased.detail);
		}
		hold->lease = std::move(leased.lease);
		auto detail = std::string();
		auto served = fs::path();
		if (const auto status = serve_from_cache(
		            *hold->lease, client, *entry, served, &detail);
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
				const auto* file = FindMember(client.Entries(),
				                              cue_dir + std::string(name),
				                              &ignored);
				if (!file) {
					return {};
				}
				auto host = fs::path();
				if (serve_from_cache(*hold->lease,
				                     client,
				                     *file,
				                     host,
				                     &failure) != CacheStatus::Ok) {
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
		auto place = PlaceSidecar(request.sidecar_path, archive, client);
		if (place.refusal.reason != DenyReason::None) {
			return refuse(place.refusal.reason, place.detail);
		}
		auto detail = std::string();
		if (!EnsureSidecarRecord(place.directory, archive, client, &detail)) {
			return refuse(DenyReason::SidecarUnavailable, detail);
		}
		auto dos = entry->dos_path;
		std::replace(dos.begin(), dos.end(), '\\', '/');
		const auto rel    = fs::path(ImagesDirName) / fs::path(dos);
		const auto target = place.directory / rel;
		if (!make_plain_directories(place.directory, rel.parent_path(), &detail)) {
			return refuse(DenyReason::SidecarUnavailable, detail);
		}
		auto lock_path = target;
		lock_path += ".lock";
		auto lock = FileLock::TryAcquire(lock_path, FileLock::Mode::Exclusive);
		if (!lock) {
			return refuse(DenyReason::SidecarBusy, PathToUtf8(target));
		}
		std::error_code ec = {};
		const auto status  = fs::symlink_status(target, ec);
		if (fs::exists(status)) {
			if (!fs::is_regular_file(status)) {
				return refuse(DenyReason::SidecarUnavailable,
				              PathToUtf8(target) +
				                      " is not a regular file");
			}
		} else if (client.GetAccess() == Access::PerMember) {
			if (const auto error = client.MaterializeToFile(
			            entry->ordinal, target, &detail);
			    error != Error::None) {
				return refuse(MountPolicy::DenyReasonForArchiveError(
				                      error),
				              detail);
			}
		} else {
			auto leased = cache->Acquire(client, archive);
			if (leased.status != CacheStatus::Ok) {
				return refuse(DenyReason::CacheUnavailable,
				              std::string(CacheStatusText(leased.status)) +
				                      ": " + leased.detail);
			}
			auto served = fs::path();
			if (const auto served_status = serve_from_cache(
			            *leased.lease, client, *entry, served, &detail);
			    served_status != CacheStatus::Ok) {
				return refuse(reason_for(served_status, *leased.lease),
				              detail);
			}
			if (!copy_with_date(served, target, &detail)) {
				return refuse(DenyReason::SidecarUnavailable, detail);
			}
		}
		hold->image_lock   = std::move(lock);
		resolved.host_file = target;
		augra::log_info(LogComponent,
		                "%s: %s in sidecar %s (%s)",
		                PathToUtf8(archive).c_str(),
		                entry->utf8_path.c_str(),
		                PathToUtf8(place.directory).c_str(),
		                place.how);
	}
	client.Close();
	report_allowed(resolved.host_file);
	hold->archive     = archive;
	hold->key         = key;
	hold->host_file   = resolved.host_file;
	hold->member_name = resolved.member_name;
	hold->size        = resolved.size;
	resolved.hold     = std::move(hold);
	out.resolved      = std::move(resolved);
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
}

void KeepForBoot(std::vector<std::shared_ptr<Hold>> holds)
{
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
}

} // namespace ArchiveImages
