// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//
#ifndef DOSBOX_ARCHIVE_CP437_H
#define DOSBOX_ARCHIVE_CP437_H

#include <string>
#include <string_view>

namespace ArchiveMount {

// CP437 to UTF-8 the way dos_437_to_fs_utf8 does it, without the engine's
// resource tables, so the helper process needs no resource files.
std::string Cp437ToUtf8(std::string_view raw);

} // namespace ArchiveMount

#endif // DOSBOX_ARCHIVE_CP437_H
