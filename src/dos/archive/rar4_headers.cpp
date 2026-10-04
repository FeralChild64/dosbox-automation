// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//
#include "dos/archive/rar4_headers.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <functional>

#if defined(WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

#include "dos/archive/descriptor.h"
#include "utils/checks.h"

CHECK_NARROWING();

namespace Rar4Headers {

namespace {

constexpr uint8_t Marker[]       = {'R', 'a', 'r', '!', 0x1a, 0x07, 0x00};
constexpr size_t BlockHead       = 7;
constexpr size_t FileFixed       = 32; // through ATTR
constexpr size_t FileFixedLarge  = 40; // plus the two high words
constexpr uint8_t FileHead       = 0x74;
constexpr uint8_t ServiceHead    = 0x7a;
constexpr uint8_t EndHead        = 0x7b;
constexpr uint16_t FlagLarge     = 0x0100;
constexpr uint16_t FlagDirectory = 0x00e0;
constexpr uint16_t FlagAddSize   = 0x8000;

uint16_t le16(const uint8_t* p)
{
	return static_cast<uint16_t>(p[0] | (p[1] << 8));
}
uint32_t le32(const uint8_t* p)
{
	return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
	       (static_cast<uint32_t>(p[2]) << 16) |
	       (static_cast<uint32_t>(p[3]) << 24);
}

// The walk over any byte source: fetch(offset, out, n) fills n bytes at
// offset or returns false. length is the source's size.
Result walk(const std::function<bool(uint64_t, uint8_t*, size_t)>& fetch,
            const uint64_t length, const uint64_t max_entries)
{
	auto result                              = Result{};
	std::array<uint8_t, FileFixedLarge> head = {};
	if (length < sizeof(Marker) || !fetch(0, head.data(), sizeof(Marker)) ||
	    std::memcmp(head.data(), Marker, sizeof(Marker)) != 0) {
		result.error = Error::NotRar4;
		return result;
	}
	uint64_t pos = sizeof(Marker);
	while (pos < length) {
		if (length - pos < BlockHead) {
			// Trailing bytes shorter than a block head: the end, as
			// libarchive takes a clean EOF.
			break;
		}
		if (!fetch(pos, head.data(), BlockHead)) {
			result.error = Error::Truncated;
			return result;
		}
		const auto type      = head[2];
		const auto flags     = le16(head.data() + 3);
		const auto head_size = le16(head.data() + 5);
		if (head_size < BlockHead) {
			result.error = Error::Malformed;
			return result;
		}
		if (type == EndHead) {
			break;
		}
		uint64_t data_size = 0;
		if (type == FileHead || type == ServiceHead) {
			const auto fixed = (flags & FlagLarge) ? FileFixedLarge
			                                       : FileFixed;
			if (head_size < fixed) {
				result.error = Error::Malformed;
				return result;
			}
			if (!fetch(pos, head.data(), fixed)) {
				result.error = Error::Truncated;
				return result;
			}
			data_size = le32(head.data() + 7);
			auto unp = static_cast<uint64_t>(le32(head.data() + 11));
			if (flags & FlagLarge) {
				data_size |= static_cast<uint64_t>(
				                     le32(head.data() + 32))
				          << 32;
				unp |= static_cast<uint64_t>(le32(head.data() + 36))
				    << 32;
			}
			if (type == FileHead) {
				if (result.files.size() >= max_entries) {
					result.error = Error::TooManyEntries;
					return result;
				}
				auto file     = FileHeader{};
				file.crc32    = le32(head.data() + 16);
				file.dos_time = le32(head.data() + 20);
				file.is_dir = (flags & FlagDirectory) == FlagDirectory;
				file.packed_size   = data_size;
				file.unpacked_size = unp;
				result.files.push_back(file);
			}
		} else if (flags & FlagAddSize) {
			if (head_size < BlockHead + 4) {
				result.error = Error::Malformed;
				return result;
			}
			if (!fetch(pos, head.data(), BlockHead + 4)) {
				result.error = Error::Truncated;
				return result;
			}
			data_size = le32(head.data() + 7);
		}
		// A size past the whole file is nonsense, not truncation;
		// checking it first keeps the sum below from wrapping (LARGE
		// headers).
		if (data_size > length) {
			result.error = Error::Malformed;
			return result;
		}
		const auto next = pos + head_size + data_size;
		if (next > length) {
			result.error = Error::Truncated;
			return result;
		}
		pos = next;
	}
	return result;
}

} // namespace

Result Parse(const std::span<const uint8_t> bytes, const uint64_t max_entries)
{
	return walk(
	        [&](const uint64_t offset, uint8_t* out, const size_t n) {
		        if (offset > bytes.size() || bytes.size() - offset < n) {
			        return false;
		        }
		        std::memcpy(out, bytes.data() + offset, n);
		        return true;
	        },
	        bytes.size(),
	        max_entries);
}

Result Read(const int fd, const uint64_t max_entries)
{
	auto result       = Result{};
	const auto dup_fd = fd >= 0 ? ArchiveMount::DuplicateNoInherit(fd) : -1;
	if (dup_fd < 0) {
		result.error = Error::Truncated;
		return result;
	}
	auto stat = ArchiveMount::StatPair{};
	if (!ArchiveMount::StatDescriptor(dup_fd, stat) || stat.size < 0) {
		ArchiveMount::CloseDescriptor(dup_fd);
		result.error = Error::Truncated;
		return result;
	}
	const auto fetch = [&](const uint64_t offset, uint8_t* out, size_t n) {
#if defined(WIN32)
		if (_lseeki64(dup_fd, static_cast<int64_t>(offset), SEEK_SET) < 0) {
			return false;
		}
		while (n > 0) {
			const auto got = _read(dup_fd,
			                       out,
			                       static_cast<unsigned int>(n));
			if (got <= 0) {
				return false;
			}
			out += got;
			n -= static_cast<size_t>(got);
		}
#else
		if (lseek(dup_fd, static_cast<off_t>(offset), SEEK_SET) < 0) {
			return false;
		}
		while (n > 0) {
			const auto got = read(dup_fd, out, n);
			if (got <= 0) {
				return false;
			}
			out += got;
			n -= static_cast<size_t>(got);
		}
#endif
		return true;
	};
	result = walk(fetch, static_cast<uint64_t>(stat.size), max_entries);
	ArchiveMount::CloseDescriptor(dup_fd);
	return result;
}

} // namespace Rar4Headers
