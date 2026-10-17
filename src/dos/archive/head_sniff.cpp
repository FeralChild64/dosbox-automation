// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//
#include "dos/archive/head_sniff.h"

#include <algorithm>
#include <array>
#include <cstdio>

#if defined(WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

#include "dos/archive/descriptor.h"

namespace ArchiveMount {

namespace {

bool starts_with(const std::span<const uint8_t> data,
                 const std::span<const uint8_t> magic)
{
	return data.size() >= magic.size() &&
	       std::equal(magic.begin(), magic.end(), data.begin());
}

constexpr uint8_t ZipMagic[]   = {'P', 'K', 0x03, 0x04};
constexpr uint8_t Rar4Magic[]  = {'R', 'a', 'r', '!', 0x1a, 0x07, 0x00};
constexpr uint8_t Rar5Magic[]  = {'R', 'a', 'r', '!', 0x1a, 0x07, 0x01, 0x00};
constexpr uint8_t SevenMagic[] = {'7', 'z', 0xbc, 0xaf, 0x27, 0x1c};
constexpr uint8_t GzipMagic[]  = {0x1f, 0x8b};
constexpr uint8_t Bzip2Magic[] = {'B', 'Z', 'h'};
constexpr uint8_t XzMagic[]    = {0xfd, '7', 'z', 'X', 'Z', 0x00};
constexpr uint8_t ZstdMagic[]  = {0x28, 0xb5, 0x2f, 0xfd};
constexpr uint8_t IszMagic[]   = {'I', 's', 'Z', '!'};

// A v7 tar has no magic: the header checksum (octal ASCII at 148, the
// field itself counted as spaces) is the only mark, as libarchive's bid
// tests it. ustar and GNU tars carry "ustar" at 257 and pass it too.
bool is_tar_header(const std::span<const uint8_t> head)
{
	if (head.size() < HeadBytes) {
		return false;
	}
	unsigned stored = 0;
	auto digits     = 0;
	for (size_t i = 148; i < 156; ++i) {
		const auto c = head[i];
		if (c == ' ' || c == 0) {
			if (digits > 0) {
				break;
			}
			continue;
		}
		if (c < '0' || c > '7') {
			return false;
		}
		stored = stored * 8 + static_cast<unsigned>(c - '0');
		++digits;
	}
	if (digits == 0) {
		return false;
	}
	unsigned sum = 0;
	for (size_t i = 0; i < HeadBytes; ++i) {
		sum += (i >= 148 && i < 156) ? ' ' : head[i];
	}
	return sum == stored;
}

uint32_t le32(const std::span<const uint8_t> b, const size_t at)
{
	return static_cast<uint32_t>(b[at]) |
	       (static_cast<uint32_t>(b[at + 1]) << 8) |
	       (static_cast<uint32_t>(b[at + 2]) << 16) |
	       (static_cast<uint32_t>(b[at + 3]) << 24);
}

// LZMA_Alone has no magic. This is libarchive 3.8.7's lzma_bidder_bid
// (archive_read_support_filter_xz.c) rule for rule; any score is a bid.
bool is_lzma_alone(const std::span<const uint8_t> head)
{
	if (head.size() < 14 || head[0] > (4 * 5 + 4) * 9 + 8) {
		return false;
	}
	auto bits = 0;
	if (head[0] == 0x5d || head[0] == 0x5e) {
		bits += 8;
	}
	if (le32(head, 5) == 0xffffffffu && le32(head, 9) == 0xffffffffu) {
		bits += 64;
	}
	const auto dict   = le32(head, 1);
	const auto listed = dict >= 0x1000 && dict <= 0x08000000 &&
	                    (dict & (dict - 1)) == 0;
	const auto reduced = dict >= 0x00300000 && dict <= 0x03F00000 &&
	                     (dict & ((1u << 20) - 1)) == 0 && bits == 8 + 64;
	return listed || reduced;
}

} // namespace

HeadSniff SniffHead(const std::span<const uint8_t> head)
{
	auto sniff        = HeadSniff{};
	const auto assign = [&](const HeadKind kind, const Access access) {
		sniff.kind   = kind;
		sniff.access = access;
		return sniff;
	};
	// The RAR4 magic is a prefix of the RAR5 one, so RAR5 goes first.
	if (starts_with(head, Rar5Magic)) {
		return assign(HeadKind::Rar5, Access::Sequential);
	}
	if (starts_with(head, Rar4Magic)) {
		return assign(HeadKind::Rar4, Access::PerMember);
	}
	if (starts_with(head, SevenMagic)) {
		return assign(HeadKind::SevenZip, Access::Sequential);
	}
	if (starts_with(head, ZipMagic)) {
		return assign(HeadKind::Zip, Access::PerMember);
	}
	if (starts_with(head, GzipMagic)) {
		return assign(HeadKind::Gzip, Access::Sequential);
	}
	if (starts_with(head, Bzip2Magic)) {
		return assign(HeadKind::Bzip2, Access::Sequential);
	}
	if (starts_with(head, XzMagic)) {
		return assign(HeadKind::Xz, Access::Sequential);
	}
	if (starts_with(head, ZstdMagic)) {
		return assign(HeadKind::Zstd, Access::Sequential);
	}
	if (starts_with(head, IszMagic)) {
		return assign(HeadKind::Isz, Access::PerMember);
	}
	if (is_tar_header(head)) {
		return assign(HeadKind::Tar, Access::Sequential);
	}
	if (is_lzma_alone(head)) {
		return assign(HeadKind::Lzma, Access::Sequential);
	}
	return sniff;
}

HeadSniff SniffFile(const int fd)
{
	if (fd < 0) {
		return {};
	}
	const auto dup_fd = DuplicateNoInherit(fd);
	if (dup_fd < 0) {
		return {};
	}
	std::array<uint8_t, HeadBytes> bytes = {};
#if defined(WIN32)
	_lseeki64(dup_fd, 0, SEEK_SET);
	const auto got = _read(dup_fd,
	                       bytes.data(),
	                       static_cast<unsigned int>(bytes.size()));
#else
	lseek(dup_fd, 0, SEEK_SET);
	const auto got = read(dup_fd, bytes.data(), bytes.size());
#endif
	CloseDescriptor(dup_fd);
	if (got <= 0) {
		return {};
	}
	auto sniff = SniffHead(
	        std::span<const uint8_t>(bytes.data(), static_cast<size_t>(got)));
	if (sniff.kind == HeadKind::Unknown && ProbeIso9660(fd)) {
		sniff.kind   = HeadKind::Iso9660;
		sniff.access = Access::PerMember;
	}
	return sniff;
}

bool ProbeIso9660(const int fd)
{
	if (fd < 0) {
		return false;
	}
	const auto dup_fd = DuplicateNoInherit(fd);
	if (dup_fd < 0) {
		return false;
	}
	constexpr int64_t PvdOffset = 16 * 2048;
	constexpr char PvdHead[]    = {1, 'C', 'D', '0', '0', '1', 1};
	std::array<char, sizeof(PvdHead)> bytes = {};
#if defined(WIN32)
	_lseeki64(dup_fd, PvdOffset, SEEK_SET);
	const auto got = _read(dup_fd,
	                       bytes.data(),
	                       static_cast<unsigned int>(bytes.size()));
#else
	lseek(dup_fd, PvdOffset, SEEK_SET);
	const auto got = read(dup_fd, bytes.data(), bytes.size());
#endif
	CloseDescriptor(dup_fd);
	return got > 0 && static_cast<size_t>(got) == bytes.size() &&
	       std::equal(bytes.begin(), bytes.end(), PvdHead);
}

const char* HeadKindName(const HeadKind kind)
{
	switch (kind) {
	case HeadKind::Unknown: return "unknown";
	case HeadKind::Zip: return "zip";
	case HeadKind::Rar4: return "RAR";
	case HeadKind::Rar5: return "RAR5";
	case HeadKind::SevenZip: return "7z";
	case HeadKind::Tar: return "tar";
	case HeadKind::Gzip: return "gzip";
	case HeadKind::Bzip2: return "bzip2";
	case HeadKind::Xz: return "xz";
	case HeadKind::Zstd: return "zstd";
	case HeadKind::Isz: return "isz";
	case HeadKind::Lzma: return "lzma";
	case HeadKind::Iso9660: return "ISO 9660";
	}
	return "unknown";
}

} // namespace ArchiveMount
