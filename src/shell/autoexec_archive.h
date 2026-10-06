// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//
#ifndef DOSBOX_AUTOEXEC_ARCHIVE_H
#define DOSBOX_AUTOEXEC_ARCHIVE_H

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Design D8 and plan 5 decision 1: what an archive or a single-image
// container on the command line becomes in AUTOEXEC.BAT.
namespace AutoexecArchive {

enum class Argument { Archive, DiskContainer, CdContainer };

// By name and head bytes, as MOUNT judges them; nothing for any other path.
std::optional<Argument> Classify(const std::filesystem::path& path);

// Mounts the archive as C: and leaves the program to MOUNT -run, which
// queues the EXIT only behind a program. A run name that is no program
// name is left out; nothing when the path cannot be quoted.
std::optional<std::vector<std::string>> ArchiveLines(const std::string& path,
                                                     std::string_view run);

} // namespace AutoexecArchive

#endif
