// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//
#include "dos/archive/archive_management.h"
#include "dos/archive/archive_identity.h"

#include "dos/archive/archive_lock.h"
#include "dos/archive/archive_toml.h"
#include "dos/archive/member_path.h"
#include "dos/archive/sidecar_record.h"
#include "utils/checks.h"

#include <system_error>

CHECK_NARROWING();

namespace ArchiveManagement {

namespace {

namespace fs = std::filesystem;
using namespace ArchiveMount;

bool is_plain_directory(const fs::path& path)
{
	std::error_code ec = {};
	const auto status  = fs::symlink_status(path, ec);
	return !ec && fs::is_directory(status);
}

// Regular files directly under dir and below; links and anything else
// weigh nothing, as the cache's own Measure counts.
int64_t regular_bytes_under(const fs::path& dir)
{
	int64_t total      = 0;
	std::error_code ec = {};
	auto it            = fs::recursive_directory_iterator(dir, ec);
	const auto end     = fs::recursive_directory_iterator();
	for (; !ec && it != end; it.increment(ec)) {
		std::error_code status_ec = {};
		const auto status = fs::symlink_status(it->path(), status_ec);
		if (status_ec) {
			continue;
		}
		if (fs::is_regular_file(status)) {
			std::error_code size_ec = {};
			const auto size = fs::file_size(it->path(), size_ec);
			if (!size_ec) {
				total += static_cast<int64_t>(size);
			}
		}
	}
	return total;
}

// An entry the cleanup would retire as damaged (no record, another
// version, no last_use) is damaged here too, not absent: its files still
// count against the cap, as the cache's own Measure counts them.
CacheEntryInfo read_entry(const fs::path& entry)
{
	CacheEntryInfo info = {};
	info.directory      = entry;
	info.bytes          = regular_bytes_under(entry / FilesDirName);
	info.locked         = FileLock::IsHeld(entry / LockName);
	auto error          = std::string();
	const auto doc = ArchiveToml::ReadFile(entry / EntryTomlName, &error);
	if (!doc || doc->root.Integer("version") != EntryTomlVersion ||
	    !doc->root.Integer("last_use")) {
		info.damaged = true;
		return info;
	}
	info.archive_path  = doc->root.String("archive_path").value_or("");
	info.identity_hash = doc->root.String("identity_hash").value_or("");
	info.last_use      = *doc->root.Integer("last_use");
	return info;
}

// A root that is not there is one answer (no cache, no sidecars); a root
// that cannot be examined is a problem to report.
enum class RootState { Missing, Directory, NotADirectory, Unreadable };

RootState root_state(const fs::path& root, const bool follow_links, std::string* problem)
{
	std::error_code ec = {};
	const auto status  = follow_links ? fs::status(root, ec)
	                                  : fs::symlink_status(root, ec);
	if (ec == std::errc::no_such_file_or_directory ||
	    (!ec && !fs::exists(status))) {
		return RootState::Missing;
	}
	if (ec) {
		*problem = "cannot read " + PathToUtf8(root) + ": " + ec.message();
		return RootState::Unreadable;
	}
	if (!fs::is_directory(status)) {
		*problem = PathToUtf8(root) + " is not a directory";
		return RootState::NotADirectory;
	}
	return RootState::Directory;
}

void add_problem(std::string& problems, const std::string& problem)
{
	if (!problems.empty()) {
		problems += "; ";
	}
	problems += problem;
}

ArchiveState state_of(const SidecarRecord& record, const IdentifyArchive& identify)
{
	auto archive = PathFromUtf8(record.archive_path);
	if (!archive || archive->empty() || !archive->is_absolute()) {
		return ArchiveState::Missing;
	}
	// A nested member's record names the host and the chain below it
	// (outer.zip/inner.zip) and carries the leaf's identity (N5).
	auto chain         = std::string();
	std::error_code ec = {};
	if (!fs::is_regular_file(fs::symlink_status(*archive, ec)) || ec) {
		const auto split = SplitMemberPath(record.archive_path);
		if (!split || split->member.empty()) {
			return ArchiveState::Missing;
		}
		archive = split->archive;
		chain   = split->member;
	}
	const auto hash = identify(*archive, chain);
	if (!hash) {
		return ArchiveState::Unreadable;
	}
	return *hash == record.identity_hash ? ArchiveState::Same
	                                     : ArchiveState::Changed;
}

} // namespace

CacheOverview InspectCache(const fs::path& cache_dir, const CacheOptions& options)
{
	CacheOverview out  = {};
	out.directory      = cache_dir;
	out.cap_bytes      = options.max_size_bytes;
	out.min_free_bytes = options.min_free_bytes;
	out.ttl_seconds    = options.ttl_seconds;
	// A linked cache root is refused before this by CachePathProblem.
	if (root_state(cache_dir, false, &out.problem) != RootState::Directory) {
		return out;
	}
	std::error_code ec = {};
	out.exists         = true;
	if (options.free_space) {
		out.free_bytes = options.free_space(cache_dir);
	} else {
		const auto space = fs::space(cache_dir, ec);
		if (!ec) {
			out.free_bytes = static_cast<int64_t>(space.available);
		}
	}
	const auto unreadable = [&](const fs::path& dir, const std::error_code& why) {
		add_problem(out.problem,
		            "cannot read " + PathToUtf8(dir) + ": " + why.message());
	};
	// D14: one-letter spread directories, one entry directory each.
	std::error_code list_ec = {};
	auto letters            = fs::directory_iterator(cache_dir, list_ec);
	const auto dir_end      = fs::directory_iterator();
	for (; !list_ec && letters != dir_end; letters.increment(list_ec)) {
		const auto letter = letters->path();
		if (!is_plain_directory(letter) ||
		    PathToUtf8(letter.filename()).size() != 1) {
			continue;
		}
		std::error_code inner_ec = {};
		auto entries = fs::directory_iterator(letter, inner_ec);
		for (; !inner_ec && entries != dir_end; entries.increment(inner_ec)) {
			if (!is_plain_directory(entries->path())) {
				continue;
			}
			auto info = read_entry(entries->path());
			out.bytes += info.bytes;
			out.entries.push_back(std::move(info));
		}
		if (inner_ec) {
			unreadable(letter, inner_ec);
		}
	}
	if (list_ec) {
		unreadable(cache_dir, list_ec);
	}
	return out;
}

EmptyReport EmptyCache(const fs::path& cache_dir, const CacheOptions& options)
{
	EmptyReport out = {};
	switch (root_state(cache_dir, false, &out.problem)) {
	case RootState::Missing: out.ok = true; return out;
	case RootState::Directory: break;
	case RootState::NotADirectory:
	case RootState::Unreadable: return out;
	}
	out.cache_exists = true;
	auto opened      = ArchiveCache::Open(cache_dir, options);
	if (opened.status != CacheStatus::Ok || !opened.cache) {
		out.problem = opened.detail;
		return out;
	}
	const auto report = opened.cache->Purge();
	out.ok            = true;
	out.removed       = report.removed_expired + report.removed_for_cap +
	              report.removed_damaged;
	out.skipped_locked = report.skipped_locked;
	out.bytes_after    = report.bytes_after;
	return out;
}

std::vector<SidecarInfo> ListSidecars(const fs::path& saves_dir,
                                      const IdentifyArchive& identify,
                                      std::string* problem)
{
	auto out      = std::vector<SidecarInfo>{};
	auto root_why = std::string();
	// The engine follows a linked saves root (the locator's Scan and
	// create_directories do), so the list follows it too.
	if (root_state(saves_dir, true, &root_why) != RootState::Directory) {
		if (problem && !root_why.empty()) {
			*problem = root_why;
		}
		return out;
	}
	std::error_code ec = {};
	auto it            = fs::directory_iterator(saves_dir, ec);
	const auto end     = fs::directory_iterator();
	for (; !ec && it != end; it.increment(ec)) {
		const auto dir = it->path();
		if (!is_plain_directory(dir)) {
			continue;
		}
		auto error = std::string();
		const auto record = SidecarRecord::Load(dir / SidecarTomlName, &error);
		if (!record) {
			continue;
		}
		SidecarInfo info   = {};
		info.directory     = dir;
		info.archive_path  = record->archive_path;
		info.identity_hash = record->identity_hash;
		info.created       = record->created;
		info.bytes         = regular_bytes_under(dir);
		info.state         = state_of(*record, identify);
		out.push_back(std::move(info));
	}
	if (ec && problem) {
		*problem = "cannot read " + PathToUtf8(saves_dir) + ": " +
		           ec.message();
	}
	return out;
}

} // namespace ArchiveManagement
