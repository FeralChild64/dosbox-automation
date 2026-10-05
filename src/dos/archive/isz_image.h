// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//
#ifndef DOSBOX_ARCHIVE_ISZ_IMAGE_H
#define DOSBOX_ARCHIVE_ISZ_IMAGE_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "dos/archive/archive_source.h"

// UltraISO's ISZ ("ISO zipped"), EZB Systems specification v1.00 plus the
// on-disk details the open readers agree on (plan 4b, D13.4). Only the
// unencrypted, single-file case; encrypted and segmented are refused.
namespace Isz {

enum class ChunkType : uint8_t { Zeros = 0, Stored = 1, Zlib = 2, Bzip2 = 3 };

struct Chunk {
	uint64_t offset = 0; // in the .isz, cumulative from data_offs
	uint32_t length = 0; // stored bytes; Zeros store none
	ChunkType type  = ChunkType::Zeros;
};

struct Image {
	uint64_t total_bytes      = 0; // total_sectors * 2048
	uint32_t block_size       = 0;
	uint64_t data_offs        = 0;
	std::vector<Chunk> chunks = {}; // empty: raw contiguous (ptr_offs 0)
};

struct ParseResult {
	ArchiveMount::Error error = ArchiveMount::Error::None;
	std::string detail        = {};
	Image image               = {};
};

constexpr size_t HeaderBytes    = 48;
constexpr uint32_t SectorBytes  = 2048;
constexpr uint32_t MaxBlockSize = 16u << 20;
constexpr uint32_t MaxChunks    = 4u << 20;
constexpr uint8_t TableKey[4]   = {0xB6, 0x8C, 0xA5, 0xDE};

// Reads the header and the chunk table through fd (the offset is left
// alone) and runs every check of D13.4 before any chunk is read.
ParseResult Parse(int fd, int64_t file_size, int64_t max_image_bytes);

using Sink = std::function<bool(const uint8_t*, size_t)>;

// The whole image, chunk by chunk, into sink; a bzip2 chunk's first three
// bytes are restored to "BZh" before decoding. Corrupt names the chunk;
// OpenFailed when the sink refused, as for any other member.
ArchiveMount::Error ReadAll(int fd, const Image& image, const Sink& sink,
                            std::string* detail);

} // namespace Isz

#endif // DOSBOX_ARCHIVE_ISZ_IMAGE_H
