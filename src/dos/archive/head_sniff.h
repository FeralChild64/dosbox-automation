// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//
#ifndef DOSBOX_ARCHIVE_HEAD_SNIFF_H
#define DOSBOX_ARCHIVE_HEAD_SNIFF_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "dos/archive/archive_source.h"

namespace ArchiveMount {

// What the first bytes say. A compressed stream is presumed to hold a
// tar (design D12 lists no other use of the four filters); the walk
// says NotAnArchive when it does not.
enum class HeadKind {
	Unknown,
	Zip,
	Rar4,
	Rar5,
	SevenZip,
	Tar,
	Gzip,
	Bzip2,
	Xz,
	Zstd,
	Isz,
	Lzma,
	Iso9660,
};

struct HeadSniff {
	HeadKind kind                = HeadKind::Unknown;
	std::optional<Access> access = {}; // set for every kind but Unknown
};

// Enough for a tar header; the sniff is a pure function of these bytes.
constexpr size_t HeadBytes = 512;

HeadSniff SniffHead(std::span<const uint8_t> head);

// Reads HeadBytes at offset 0 through a duplicate; the duplicate shares
// the offset, so the caller's position moves. Unknown on any failure.
HeadSniff SniffFile(int fd);

// ECMA-119 8.4: a primary volume descriptor at sector 16 (type 1,
// "CD001", version 1); the system area before it is blank, so the head
// cannot see an ISO. Reads through a duplicate; false on any failure.
bool ProbeIso9660(int fd);

const char* HeadKindName(HeadKind kind);

} // namespace ArchiveMount

#endif // DOSBOX_ARCHIVE_HEAD_SNIFF_H
