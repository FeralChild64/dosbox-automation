// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//
#include "dos/archive/isz_image.h"

#include "dos/archive/zip_central_directory.h"
#include "utils/checks.h"

#include <bzlib.h>
#include <zlib.h>

#include <algorithm>
#include <cstring>
#include <limits>

CHECK_NARROWING();

namespace Isz {

namespace {

using ArchiveMount::Error;

uint64_t le(const std::vector<uint8_t>& b, const size_t at, const size_t width)
{
	uint64_t value = 0;
	for (size_t i = 0; i < width; ++i) {
		value |= static_cast<uint64_t>(b[at + i]) << (8 * i);
	}
	return value;
}

ParseResult refuse(const Error error, std::string detail)
{
	return {error, std::move(detail), {}};
}

// Reads through a duplicate; Truncated when the range leaves the file.
Error read_range(const int fd, const uint64_t offset, const uint64_t size,
                 std::vector<uint8_t>& out)
{
	switch (ZipCentralDirectory::ReadRange(fd, offset, size, out)) {
	case ZipCentralDirectory::Error::None: return Error::None;
	case ZipCentralDirectory::Error::Truncated: return Error::Corrupt;
	default: return Error::OpenFailed;
	}
}

} // namespace

ParseResult Parse(const int fd, const int64_t file_size, const int64_t max_image_bytes)
{
	if (fd < 0 || file_size < 0) {
		return refuse(Error::OpenFailed, "no file to read");
	}
	const auto size        = static_cast<uint64_t>(file_size);
	std::vector<uint8_t> h = {};
	if (read_range(fd, 0, std::min<uint64_t>(size, HeaderBytes), h) !=
	    Error::None) {
		return refuse(Error::OpenFailed, "cannot read the header");
	}
	if (h.size() < 4 || std::memcmp(h.data(), "IsZ!", 4) != 0) {
		return refuse(Error::NotAnArchive, "no IsZ! signature");
	}
	if (h.size() < HeaderBytes) {
		return refuse(Error::Corrupt, "header truncated");
	}
	// EZB ISZ spec v1.00, section 3.1: the packed header.
	const auto header_size   = le(h, 4, 1);
	const auto sect_size     = le(h, 10, 2);
	const auto total_sectors = le(h, 12, 4);
	const auto has_password  = le(h, 16, 1);
	const auto segment_size  = le(h, 17, 8);
	const auto nblocks       = le(h, 25, 4);
	const auto block_size    = le(h, 29, 4);
	const auto ptr_len       = le(h, 33, 1);
	const auto ptr_offs      = le(h, 35, 4);
	const auto seg_offs      = le(h, 39, 4);
	const auto data_offs     = le(h, 43, 4);

	if (header_size < HeaderBytes) {
		return refuse(Error::Corrupt,
		              "header size " + std::to_string(header_size) +
		                      " below 48");
	}
	if (sect_size != SectorBytes) {
		return refuse(Error::Corrupt,
		              "sector size " + std::to_string(sect_size) +
		                      ", not 2048");
	}
	if (has_password != 0) {
		return refuse(Error::Encrypted,
		              "encrypted (has_password " +
		                      std::to_string(has_password) + ")");
	}
	if (seg_offs != 0 || segment_size != 0) {
		return refuse(Error::MultiVolume, "segmented: one part of a set");
	}
	if (block_size == 0 || block_size % SectorBytes != 0 ||
	    block_size > MaxBlockSize) {
		return refuse(Error::Corrupt,
		              "block size " + std::to_string(block_size));
	}
	if (total_sectors == 0) {
		return refuse(Error::Corrupt, "total sectors 0");
	}
	auto result          = ParseResult{};
	auto& image          = result.image;
	image.total_bytes    = total_sectors * SectorBytes;
	image.block_size     = static_cast<uint32_t>(block_size);
	image.data_offs      = data_offs;
	const auto max_image = static_cast<uint64_t>(
	        std::max<int64_t>(max_image_bytes, 0));
	if (image.total_bytes > max_image) {
		return refuse(Error::TooLarge,
		              "image of " + std::to_string(image.total_bytes) +
		                      " bytes over the member cap");
	}
	const auto nchunks = (image.total_bytes + block_size - 1) / block_size;
	if (nchunks > nblocks || nchunks > MaxChunks) {
		return refuse(Error::Corrupt,
		              "chunk count " + std::to_string(nchunks) +
		                      " (nblocks " + std::to_string(nblocks) + ")");
	}
	if (ptr_offs == 0) {
		if (data_offs > size || image.total_bytes > size - data_offs) {
			return refuse(Error::Corrupt, "data end past the file size");
		}
		return result;
	}
	if (ptr_len < 1 || ptr_len > 8) {
		return refuse(Error::Corrupt,
		              "pointer length " + std::to_string(ptr_len));
	}
	const auto table_bytes = nchunks * ptr_len;
	if (ptr_offs > size || table_bytes > size - ptr_offs) {
		return refuse(Error::Corrupt, "chunk table runs past the file");
	}
	std::vector<uint8_t> table = {};
	if (read_range(fd, ptr_offs, table_bytes, table) != Error::None) {
		return refuse(Error::Corrupt, "cannot read the chunk table");
	}
	for (size_t i = 0; i < table.size(); ++i) {
		table[i] ^= TableKey[i % 4];
	}
	// The pointer: ptr_len bytes, the type in the top two bits, the stored
	// length below; chunk data lies back to back, Zeros store nothing.
	const auto bits     = static_cast<unsigned>(ptr_len * 8);
	const auto len_mask = (uint64_t{1} << (bits - 2)) - 1;
	auto offset         = static_cast<uint64_t>(data_offs);
	image.chunks.reserve(static_cast<size_t>(nchunks));
	for (uint64_t i = 0; i < nchunks; ++i) {
		const auto value  = le(table,
                                      static_cast<size_t>(i * ptr_len),
                                      static_cast<size_t>(ptr_len));
		const auto type   = static_cast<ChunkType>(value >> (bits - 2));
		const auto length = value & len_mask;
		const auto name   = "chunk " + std::to_string(i);
		if (length > block_size) {
			return refuse(Error::Corrupt,
			              name + ": length over the block size");
		}
		if (length == 0 &&
		    (type == ChunkType::Zlib || type == ChunkType::Bzip2)) {
			return refuse(Error::Corrupt,
			              name + ": empty compressed chunk");
		}
		auto chunk   = Chunk{};
		chunk.offset = offset;
		chunk.type   = type;
		if (type != ChunkType::Zeros) {
			chunk.length = static_cast<uint32_t>(length);
			if (length > std::numeric_limits<uint64_t>::max() - offset) {
				return refuse(Error::Corrupt,
				              name + ": offset overflow");
			}
			offset += length;
		}
		image.chunks.push_back(chunk);
	}
	// UltraISO writes a 68-byte trailer after the last chunk (plan 4b, 3a).
	if (offset > size) {
		return refuse(Error::Corrupt,
		              "data end " + std::to_string(offset) +
		                      " past the file size " + std::to_string(size));
	}
	return result;
}

Error ReadAll(const int fd, const Image& image, const Sink& sink, std::string* detail)
{
	auto fail = [&](const Error error, const std::string& why) {
		if (detail) {
			*detail = why;
		}
		return error;
	};
	auto emit = [&](const uint8_t* bytes, const size_t count) {
		return sink(bytes, count);
	};
	std::vector<uint8_t> stored = {};
	if (image.chunks.empty()) {
		constexpr uint64_t Piece = 1u << 20;
		for (uint64_t done = 0; done < image.total_bytes;) {
			const auto count = std::min(Piece, image.total_bytes - done);
			const auto read = read_range(fd,
			                             image.data_offs + done,
			                             count,
			                             stored);
			if (read != Error::None) {
				return fail(read,
				            "cannot read the image at " +
				                    std::to_string(done));
			}
			if (!emit(stored.data(), stored.size())) {
				return fail(Error::OpenFailed,
				            "cannot write the member");
			}
			done += count;
		}
		return Error::None;
	}
	// Parse builds the table to cover the image exactly; any other Image
	// would size a chunk's share past the block buffer.
	const auto needed = image.block_size == 0
	                          ? uint64_t{0}
	                          : image.total_bytes / image.block_size +
	                                    (image.total_bytes % image.block_size != 0);
	if (image.block_size == 0 || image.block_size > MaxBlockSize ||
	    image.chunks.size() != needed) {
		return fail(Error::Corrupt,
		            "the chunk table does not cover the image");
	}
	std::vector<uint8_t> block(image.block_size);
	const auto last = image.chunks.size() - 1;
	for (size_t i = 0; i < image.chunks.size(); ++i) {
		const auto& chunk    = image.chunks[i];
		const auto name      = "chunk " + std::to_string(i);
		const auto expected  = i == last ? image.total_bytes -
                                                          uint64_t{last} * image.block_size
		                                 : uint64_t{image.block_size};
		const auto out_bytes = static_cast<size_t>(expected);
		if (chunk.type != ChunkType::Zeros) {
			const auto read = read_range(fd, chunk.offset, chunk.length, stored);
			if (read != Error::None) {
				return fail(read, name + ": cannot read its data");
			}
		}
		switch (chunk.type) {
		case ChunkType::Zeros:
			std::fill_n(block.begin(), out_bytes, 0);
			break;
		case ChunkType::Stored:
			if (stored.size() != out_bytes) {
				return fail(Error::Corrupt,
				            name + ": stored data does not fill its share");
			}
			std::copy(stored.begin(), stored.end(), block.begin());
			break;
		case ChunkType::Zlib: {
			z_stream z = {};
			if (inflateInit(&z) != Z_OK) {
				return fail(Error::OpenFailed,
				            name + ": zlib init failed");
			}
			z.next_in        = stored.data();
			z.avail_in       = static_cast<uInt>(stored.size());
			z.next_out       = block.data();
			z.avail_out      = static_cast<uInt>(out_bytes);
			const auto r     = inflate(&z, Z_FINISH);
			const auto total = z.total_out;
			inflateEnd(&z);
			if (r != Z_STREAM_END || total != out_bytes) {
				return fail(Error::Corrupt,
				            name + ": zlib data does not decode to its share");
			}
			break;
		}
		case ChunkType::Bzip2: {
			if (stored.size() < 4) {
				return fail(Error::Corrupt,
				            name + ": bzip2 chunk too short");
			}
			// UltraISO overwrites the stream's "BZh" prefix (plan
			// 4b, D13.4).
			stored[0]    = 'B';
			stored[1]    = 'Z';
			stored[2]    = 'h';
			bz_stream bz = {};
			if (BZ2_bzDecompressInit(&bz, 0, 0) != BZ_OK) {
				return fail(Error::OpenFailed,
				            name + ": bzip2 init failed");
			}
			bz.next_in   = reinterpret_cast<char*>(stored.data());
			bz.avail_in  = static_cast<unsigned>(stored.size());
			bz.next_out  = reinterpret_cast<char*>(block.data());
			bz.avail_out = static_cast<unsigned>(out_bytes);
			const auto r = BZ2_bzDecompress(&bz);
			const auto left = bz.avail_out;
			BZ2_bzDecompressEnd(&bz);
			if (r != BZ_STREAM_END || left != 0) {
				return fail(Error::Corrupt,
				            name + ": bzip2 data does not decode to its share");
			}
			break;
		}
		}
		if (!emit(block.data(), out_bytes)) {
			return fail(Error::OpenFailed, "cannot write the member");
		}
	}
	return Error::None;
}

} // namespace Isz
