// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//
#ifndef DOSBOX_ARCHIVE_MEMBER_PATH_H
#define DOSBOX_ARCHIVE_MEMBER_PATH_H

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "dos/archive/archive_source.h"
#include "dos/archive/entry_names.h"
#include "dos/archive/head_sniff.h"

namespace ArchiveMount {

// The host path as UTF-8: the bytes as they are on POSIX, converted from
// the wide form on Windows.
std::string PathToUtf8(const std::filesystem::path& path);

// D12: a listed extension, without regard to case, confirmed by the head
// sniff; a .zip holding an ISO's bytes is not an archive.
bool IsArchiveFile(const std::filesystem::path& host_path);

struct MemberPath {
	std::filesystem::path archive = {}; // the host part, as typed
	std::string member            = {}; // the rest; empty when lone
	bool lone = false;                  // plan 4b: a one-member container
};

// Plan 4b D13.1: <stem>.<image extension>.<gz|bz2|xz|zst> on the file
// name, without regard to case; the stem must not be empty.
bool IsCompressedImageName(std::string_view name);

// A regular file whose name and head agree on a one-member container:
// .imz and a zip, .isz and the IsZ! signature, or a compressed image name
// and its filter's magic.
bool IsSingleImageContainer(const std::filesystem::path& host_path);
// The head a container's name promises: .imz a zip, .isz an ISZ, a
// compressed image name its filter; nothing for any other name.
std::optional<HeadKind> ContainerKindByName(const std::filesystem::path& host_path);

// The container as a member path with no member and the lone flag.
std::optional<MemberPath> LoneImageMember(const std::filesystem::path& host_path);

// Design D7: from the left, the first component that is a regular file
// and an archive ends the host part. Nothing when the whole path exists,
// a component before it does not resolve, or no prefix is an archive.
std::optional<MemberPath> SplitMemberPath(const std::string& raw);

// The typed member through the Phase 1 name rules, then matched on the
// UTF-8 path, the DOS path, and the UTF-8 path without regard to ASCII
// case when exactly one entry matches. Directories never match.
const Entry* FindMember(const std::vector<Entry>& entries, std::string_view typed,
                        ArchiveNames::Rejection* rejection);

} // namespace ArchiveMount

#endif // DOSBOX_ARCHIVE_MEMBER_PATH_H
