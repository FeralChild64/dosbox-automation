// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//
#ifndef DOSBOX_SIDECAR_LOCATOR_H
#define DOSBOX_SIDECAR_LOCATOR_H

#include <filesystem>
#include <string>
#include <string_view>

namespace ArchiveMount {

// "<sanitized stem>-<hash8>", the same key the cache uses (D2, D14).
std::string SidecarKey(const std::filesystem::path& archive_path,
                       std::string_view identity_hash);
// The same from a stem the caller names: the host's for a nested or an
// entered archive (nested design N5).
std::string SidecarKeyForStem(std::string_view stem, std::string_view identity_hash);

struct SidecarLocation {
	enum class Kind { ByPath, ByHash, Create };
	Kind kind                       = Kind::Create;
	std::filesystem::path directory = {}; // the sidecar to use or create
	std::filesystem::path also_matching = {};  // a hash match beside a path
	                                           // match
	std::filesystem::path replaced_old = {};   // D2a: the path match with
	                                           // another hash
	std::filesystem::path beside_archive = {}; // a candidate the caller may
	                                           // adopt
};

// Design D2: under saves a record naming the archive's path wins, then one
// carrying its hash; a sidecar beside the archive is reported for the caller;
// else create as key_stem (empty: the archive's stem). Links never count.
SidecarLocation LocateSidecar(const std::filesystem::path& saves_dir,
                              const std::filesystem::path& archive_canonical,
                              std::string_view identity_hash,
                              std::string_view key_stem = {});

} // namespace ArchiveMount

#endif // DOSBOX_SIDECAR_LOCATOR_H
