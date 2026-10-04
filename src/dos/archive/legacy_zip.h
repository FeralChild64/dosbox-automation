// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//
#ifndef DOSBOX_ARCHIVE_LEGACY_ZIP_H
#define DOSBOX_ARCHIVE_LEGACY_ZIP_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

// PKZIP 1.x methods that libarchive does not decode (APPNOTE 4.4.5).
namespace LegacyZip {

// Fills buffer with up to capacity compressed bytes; 0 at the member's end.
using Source = std::function<size_t(uint8_t* buffer, size_t capacity)>;
// Takes decoded bytes in order; false stops the decoder.
using Sink = std::function<bool(const uint8_t* data, size_t size)>;

enum class Result { Ok, Corrupt, SinkFailed };

// The zip format's CRC-32 (APPNOTE 4.4.7), continued from crc over data.
uint32_t Crc32(uint32_t crc, const uint8_t* data, size_t size);

constexpr uint16_t MethodShrink  = 1;
constexpr uint16_t MethodImplode = 6;

// Method 1. Ends where the input ends; more or less than out_size is Corrupt.
Result Unshrink(const Source& in, uint64_t out_size, const Sink& out,
                std::string* detail);

// Method 6, general purpose bit 1 (8K window) and bit 2 (literal tree).
// Ends at out_size; bytes missing from the stream are Corrupt, bytes left
// over are accepted, as UnZip does.
Result Explode(const Source& in, uint64_t compressed_size, uint64_t out_size,
               bool window_8k, bool literal_tree, const Sink& out,
               std::string* detail);

} // namespace LegacyZip

#endif // DOSBOX_ARCHIVE_LEGACY_ZIP_H
