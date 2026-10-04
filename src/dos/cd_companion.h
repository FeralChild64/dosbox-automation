// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//
#ifndef DOSBOX_CD_COMPANION_H
#define DOSBOX_CD_COMPANION_H

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace CdCompanion {

struct Resolved {
	std::filesystem::path path = {};
	bool rewritten             = false; // not taken as written
};

// The file a cue FILE line or an mds names, found only directly beside
// the image (ada-it53): a name not there as written is looked up by its
// last component, case-insensitively. why says what refused it.
std::optional<Resolved> Resolve(const std::filesystem::path& image,
                                std::string_view name, std::string* why);

} // namespace CdCompanion

#endif // DOSBOX_CD_COMPANION_H
