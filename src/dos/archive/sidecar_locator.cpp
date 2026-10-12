// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//
#include "dos/archive/sidecar_locator.h"

#include "dos/archive/archive_identity.h"
#include "dos/archive/member_path.h"
#include "dos/archive/sidecar_record.h"
#include "utils/checks.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

CHECK_NARROWING();

namespace ArchiveMount {

namespace fs = std::filesystem;

std::string SidecarKey(const fs::path& archive_path,
                       const std::string_view identity_hash)
{
	return ArchiveIdentity::ShortKey(PathToUtf8(archive_path.stem()),
	                                 identity_hash);
}

std::string SidecarKeyForStem(const std::string_view stem,
                              const std::string_view identity_hash)
{
	return ArchiveIdentity::ShortKey(stem, identity_hash);
}

namespace {

struct Candidate {
	fs::path dir         = {};
	SidecarRecord record = {};
};

// Every real directory under parent with a readable record, sorted by name.
std::vector<Candidate> Scan(const fs::path& parent)
{
	std::vector<Candidate> out = {};
	std::error_code ec         = {};
	// increment(ec), not range-for: the iterator's operator++ throws.
	auto it = fs::directory_iterator(parent, ec);
	for (; !ec && it != fs::directory_iterator(); it.increment(ec)) {
		const auto item       = it->path();
		std::error_code probe = {};
		if (!fs::is_directory(fs::symlink_status(item, probe)) || probe) {
			continue;
		}
		const auto toml = item / SidecarTomlName;
		if (!fs::is_regular_file(fs::symlink_status(toml, probe)) || probe) {
			continue;
		}
		auto error  = std::string();
		auto record = SidecarRecord::Load(toml, &error);
		if (!record) {
			continue;
		}
		out.push_back({item, std::move(*record)});
	}
	std::sort(out.begin(), out.end(), [](const Candidate& a, const Candidate& b) {
		return a.dir < b.dir;
	});
	return out;
}

struct Matches {
	const Candidate* by_path            = nullptr;
	const Candidate* by_hash            = nullptr;
	const Candidate* by_path_other_hash = nullptr;
};

Matches Classify(const std::vector<Candidate>& candidates,
                 const std::string& path_text, const std::string_view identity_hash)
{
	Matches m = {};
	for (const auto& c : candidates) {
		const bool same_path = c.record.archive_path == path_text;
		const bool same_hash = c.record.identity_hash == identity_hash;
		if (same_path && same_hash && !m.by_path) {
			m.by_path = &c;
		} else if (same_hash && !m.by_hash) {
			m.by_hash = &c;
		} else if (same_path && !same_hash && !m.by_path_other_hash) {
			m.by_path_other_hash = &c;
		}
	}
	return m;
}

} // namespace

SidecarLocation LocateSidecar(const fs::path& saves_dir,
                              const fs::path& archive_canonical,
                              const std::string_view identity_hash,
                              const std::string_view key_stem)
{
	SidecarLocation loc    = {};
	const auto path_text   = PathToUtf8(archive_canonical);
	const auto under_saves = Scan(saves_dir);
	const auto saves = Classify(under_saves, path_text, identity_hash);
	if (saves.by_path) {
		loc.kind      = SidecarLocation::Kind::ByPath;
		loc.directory = saves.by_path->dir;
		if (saves.by_hash) {
			loc.also_matching = saves.by_hash->dir;
		}
		return loc;
	}
	if (saves.by_hash) {
		loc.kind      = SidecarLocation::Kind::ByHash;
		loc.directory = saves.by_hash->dir;
		return loc;
	}
	// Step 4 runs the same lookups, D2a included: a path match whose hash
	// differs is an old release and never shadows the new one.
	const auto nearby = Scan(archive_canonical.parent_path());
	const auto beside = Classify(nearby, path_text, identity_hash);
	if (beside.by_path) {
		loc.beside_archive = beside.by_path->dir;
	} else if (beside.by_hash) {
		loc.beside_archive = beside.by_hash->dir;
	}
	loc.kind      = SidecarLocation::Kind::Create;
	loc.directory = saves_dir /
	                (key_stem.empty()
	                         ? SidecarKey(archive_canonical, identity_hash)
	                         : SidecarKeyForStem(key_stem, identity_hash));
	if (saves.by_path_other_hash) {
		loc.replaced_old = saves.by_path_other_hash->dir;
	} else if (beside.by_path_other_hash) {
		loc.replaced_old = beside.by_path_other_hash->dir;
	}
	return loc;
}

} // namespace ArchiveMount
