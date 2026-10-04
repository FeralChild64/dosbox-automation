// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//
#ifndef DOSBOX_ARCHIVE_CUE_REWRITE_H
#define DOSBOX_ARCHIVE_CUE_REWRITE_H

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace ArchiveMount {

constexpr int64_t MaxCueSheetBytes = 64 * 1024;
// cdrom_image.cpp's MAX_LINE_LENGTH and MAX_FILENAME_LENGTH, less the NUL.
constexpr size_t MaxCueLineBytes = 511;
constexpr size_t MaxCueNameBytes = 255;

// The name to write for a FILE name as written in the cue, or nothing;
// the loader takes only files beside the sheet (ada-it53).
using CueFileResolver =
        std::function<std::optional<std::filesystem::path>(std::string_view name)>;

struct CueRewrite {
	bool ok             = false;
	std::string text    = {}; // the rewritten sheet when ok
	std::string refused = {}; // why not, naming the line or the name
};

// Design D17: every FILE line, the keyword matched without regard to case,
// is rewritten to the quoted name the resolver gives for its member.
CueRewrite RewriteCueSheet(std::string_view cue, const CueFileResolver& resolve);

} // namespace ArchiveMount

#endif // DOSBOX_ARCHIVE_CUE_REWRITE_H
