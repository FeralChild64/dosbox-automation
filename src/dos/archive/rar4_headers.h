// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//
#ifndef DOSBOX_ARCHIVE_RAR4_HEADERS_H
#define DOSBOX_ARCHIVE_RAR4_HEADERS_H

#include <cstdint>
#include <span>
#include <vector>

// The RAR 4.x block walk (technote "Archive block format"): the stored
// FTIME and FILE_CRC per file header, which libarchive converts or hides.
// Reads headers only and seeks over the data.
namespace Rar4Headers {

struct FileHeader {
	uint32_t dos_time      = 0; // date in the high word, time in the low
	uint32_t crc32         = 0;
	bool is_dir            = false;
	uint64_t packed_size   = 0;
	uint64_t unpacked_size = 0;
};

enum class Error {
	None,
	NotRar4,        // no marker block
	Truncated,      // a header or its data runs past the end
	Malformed,      // a head size below seven, or below the fixed part
	TooManyEntries, // more file headers than the cap
};

struct Result {
	std::vector<FileHeader> files = {};
	Error error                   = Error::None;
};

// Pure; the tests feed it hand-built bytes.
Result Parse(std::span<const uint8_t> bytes, uint64_t max_entries);

// Over a duplicate of fd, which shares the caller's offset and moves it.
Result Read(int fd, uint64_t max_entries);

} // namespace Rar4Headers

#endif // DOSBOX_ARCHIVE_RAR4_HEADERS_H
