// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//
#include "dos/archive/helper_protocol.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <unordered_set>
#include <utility>

#include "dos/archive/entry_names.h"
#include "dos/dos_names.h"

#include "utils/checks.h"

CHECK_NARROWING();

namespace ArchiveHelper {

namespace {

constexpr auto LastError  = static_cast<uint8_t>(ArchiveMount::Error::Timeout);
constexpr auto LastFormat = static_cast<uint8_t>(ArchiveMount::Format::Isz);
constexpr auto LastAccess = static_cast<uint8_t>(ArchiveMount::Access::Sequential);

// ordinal, two string counts, is_dir, size, mtime, has_crc, crc32
constexpr size_t MinEncodedEntry = 4 + 4 + 4 + 1 + 8 + 8 + 1 + 4 + 8;

constexpr uint32_t MaxOrdinal = std::numeric_limits<int32_t>::max();

bool known_type(const uint8_t t)
{
	switch (static_cast<Type>(t)) {
	case Type::Hello:
	case Type::Open:
	case Type::Materialize:
	case Type::MaterializeAll:
	case Type::Quit:
	case Type::HelloReply:
	case Type::IndexReply:
	case Type::StatusReply:
	case Type::TestCrashOn:
	case Type::TestHangOn:
	case Type::TestAllocate: return true;
	}
	return false;
}

bool is_dos_name_char(const char c)
{
	return c > ' ' && c < 0x7f && !(c >= 'a' && c <= 'z') && c != '.' &&
	       !is_special_character(c);
}

// One 8.3 component as the reader's name table produces it: uppercase
// ASCII, 1 to 8 name characters, an optional extension of 1 to 3.
bool is_dos_component(const std::string_view component)
{
	const auto dot  = component.find('.');
	const auto name = component.substr(0, dot);
	const auto ext  = dot == std::string_view::npos ? std::string_view()
	                                                : component.substr(dot + 1);
	if (name.empty() || name.size() > 8 || ext.size() > 3 ||
	    (dot != std::string_view::npos && ext.empty())) {
		return false;
	}
	for (const auto part : {name, ext}) {
		for (const char c : part) {
			if (!is_dos_name_char(c)) {
				return false;
			}
		}
	}
	return true;
}

bool is_dos_path(const std::string_view path)
{
	if (path.empty() || path.size() > ArchiveNames::DosPathLength - 2) {
		return false;
	}
	size_t start = 0;
	for (;;) {
		const auto sep = path.find('\\', start);
		if (!is_dos_component(path.substr(start, sep - start))) {
			return false;
		}
		if (sep == std::string_view::npos) {
			return true;
		}
		start = sep + 1;
	}
}

} // namespace

std::optional<std::string> CheckIndex(const IndexReply& index,
                                      const ArchiveMount::Limits& limits,
                                      const std::optional<int64_t> expected_archive_length)
{
	if (expected_archive_length &&
	    index.archive_length != *expected_archive_length) {
		return "an archive length that is not the file's or the window's";
	}
	// 78 characters of DOS path hold at most 39 components.
	constexpr int64_t MaxDepth = (ArchiveNames::DosPathLength - 1) / 2;

	auto ordinals   = std::unordered_set<int>();
	auto utf8_paths = std::unordered_set<std::string>();
	auto dos_paths  = std::unordered_set<std::string>();
	auto dirs       = std::unordered_set<std::string>();
	int64_t real    = 0;
	int64_t implied = 0;
	int64_t sum     = 0;
	for (const auto& e : index.entries) {
		if (e.size < 0 || e.size > limits.max_member_bytes ||
		    (e.is_dir && e.size != 0)) {
			return "a size outside the limits: " + e.utf8_path;
		}
		if (e.stored_offset < -1 || (e.is_dir && e.stored_offset != -1) ||
		    (e.stored_offset >= 0 &&
		     (e.stored_offset > index.archive_length ||
		      e.size > index.archive_length - e.stored_offset))) {
			return "a stored offset outside the archive: " + e.utf8_path;
		}
		if (e.ordinal == ArchiveMount::ImpliedOrdinal) {
			if (!e.is_dir) {
				return "an implied ordinal on a file: " + e.utf8_path;
			}
			++implied;
		} else if (e.ordinal < 0 || e.ordinal >= limits.max_entries ||
		           !ordinals.insert(e.ordinal).second) {
			return "an ordinal out of range or twice: " +
			       std::to_string(e.ordinal);
		} else {
			++real;
		}
		auto rejection   = ArchiveNames::Rejection::None;
		const auto split = ArchiveNames::SplitEntryPath(e.utf8_path,
		                                                &rejection);
		auto normal      = std::string();
		if (split) {
			for (const auto& component : split->components) {
				normal += (normal.empty() ? "" : "/") + component;
			}
		}
		if (!split || normal != e.utf8_path) {
			return "an entry path not in the reader's form: " + e.utf8_path;
		}
		if (!is_dos_path(e.dos_path) ||
		    split->components.size() !=
		            static_cast<size_t>(std::count(e.dos_path.begin(),
		                                           e.dos_path.end(),
		                                           '\\') +
		                                1)) {
			return "a DOS path the reader cannot produce: " + e.dos_path;
		}
		if (!utf8_paths.insert(e.utf8_path).second ||
		    !dos_paths.insert(e.dos_path).second) {
			return "a path twice: " + e.utf8_path;
		}
		if (e.is_dir) {
			dirs.insert(e.dos_path);
		}
		sum += e.size;
	}
	if (implied > MaxDepth * std::max<int64_t>(real, 1)) {
		return "more implied directories than the entries can need";
	}
	for (const auto& e : index.entries) {
		const auto sep = e.dos_path.rfind('\\');
		if (sep != std::string::npos &&
		    !dirs.contains(e.dos_path.substr(0, sep))) {
			return "an entry whose parent is not a directory: " +
			       e.dos_path;
		}
	}
	if (index.total_bytes != sum || sum > limits.max_total_bytes) {
		return "a total that is not the sum of the members or over the limit";
	}
	return std::nullopt;
}

void Writer::U8(const uint8_t v)
{
	bytes.push_back(v);
}

void Writer::U16(const uint16_t v)
{
	bytes.push_back(static_cast<uint8_t>(v));
	bytes.push_back(static_cast<uint8_t>(v >> 8));
}

void Writer::U32(const uint32_t v)
{
	for (int i = 0; i < 4; ++i) {
		bytes.push_back(static_cast<uint8_t>(v >> (8 * i)));
	}
}

void Writer::I64(const int64_t v)
{
	const auto u = static_cast<uint64_t>(v);
	for (int i = 0; i < 8; ++i) {
		bytes.push_back(static_cast<uint8_t>(u >> (8 * i)));
	}
}

void Writer::Str(const std::string_view s)
{
	U32(static_cast<uint32_t>(s.size()));
	bytes.insert(bytes.end(), s.begin(), s.end());
}

std::vector<uint8_t> Writer::Take()
{
	return std::move(bytes);
}

Reader::Reader(const std::span<const uint8_t> bytes_) : bytes(bytes_) {}

bool Reader::U8(uint8_t& v)
{
	if (Remaining() < 1) {
		return false;
	}
	v = bytes[pos++];
	return true;
}

bool Reader::U16(uint16_t& v)
{
	if (Remaining() < 2) {
		return false;
	}
	v = static_cast<uint16_t>(bytes[pos] | (bytes[pos + 1] << 8));
	pos += 2;
	return true;
}

bool Reader::U32(uint32_t& v)
{
	if (Remaining() < 4) {
		return false;
	}
	v = 0;
	for (size_t i = 4; i-- > 0;) {
		v = (v << 8) | bytes[pos + i];
	}
	pos += 4;
	return true;
}

bool Reader::I64(int64_t& v)
{
	if (Remaining() < 8) {
		return false;
	}
	uint64_t u = 0;
	for (size_t i = 8; i-- > 0;) {
		u = (u << 8) | bytes[pos + i];
	}
	pos += 8;
	v = static_cast<int64_t>(u);
	return true;
}

bool Reader::Str(std::string& s)
{
	uint32_t n = 0;
	if (!U32(n) || n > Remaining()) {
		return false;
	}
	s.assign(reinterpret_cast<const char*>(bytes.data() + pos), n);
	pos += n;
	return true;
}

bool Reader::AtEnd() const
{
	return pos == bytes.size();
}

size_t Reader::Position() const
{
	return pos;
}

size_t Reader::Remaining() const
{
	return bytes.size() - pos;
}

std::vector<uint8_t> EncodeFrame(const Type type, const std::span<const uint8_t> payload)
{
	auto w = Writer{};
	w.U32(Magic);
	w.U16(Version);
	w.U8(static_cast<uint8_t>(type));
	w.U8(0);
	w.U32(static_cast<uint32_t>(payload.size()));
	auto out = w.Take();
	out.insert(out.end(), payload.begin(), payload.end());
	return out;
}

bool DecodeHeader(const std::span<const uint8_t> header,
                  const uint32_t max_payload, Type& type, uint32_t& payload_length)
{
	if (header.size() < HeaderSize) {
		return false;
	}
	auto r           = Reader(header.first(HeaderSize));
	uint32_t magic   = 0;
	uint16_t version = 0;
	uint8_t t        = 0;
	uint8_t flags    = 0;
	uint32_t length  = 0;
	if (!r.U32(magic) || !r.U16(version) || !r.U8(t) || !r.U8(flags) ||
	    !r.U32(length)) {
		return false;
	}
	if (magic != Magic || version != Version || !known_type(t) ||
	    length > max_payload) {
		return false;
	}
	type           = static_cast<Type>(t);
	payload_length = length;
	return true;
}

std::vector<uint8_t> EncodeHelloReply(const HelloReply& v)
{
	auto w = Writer{};
	w.U16(v.version);
	w.U32(v.libarchive_version);
	w.Str(v.libarchive_string);
	return w.Take();
}

bool DecodeHelloReply(const std::span<const uint8_t> bytes, HelloReply& v)
{
	auto r = Reader(bytes);
	return r.U16(v.version) && r.U32(v.libarchive_version) &&
	       r.Str(v.libarchive_string) && r.AtEnd();
}

std::vector<uint8_t> EncodeOpenRequest(const OpenRequest& v)
{
	auto w = Writer{};
	w.Str(v.path);
	w.I64(v.limits.max_entries);
	w.I64(v.limits.max_total_bytes);
	w.I64(v.limits.max_member_bytes);
	w.U8(v.descriptor_follows ? 1 : 0);
	w.I64(static_cast<int64_t>(v.handle));
	w.I64(v.window_offset);
	w.I64(v.window_length);
	return w.Take();
}

bool DecodeOpenRequest(const std::span<const uint8_t> bytes, OpenRequest& v)
{
	auto r          = Reader(bytes);
	uint8_t follows = 0;
	int64_t handle  = 0;
	if (!(r.Str(v.path) && r.I64(v.limits.max_entries) &&
	      r.I64(v.limits.max_total_bytes) &&
	      r.I64(v.limits.max_member_bytes) && r.U8(follows) && r.I64(handle) &&
	      r.I64(v.window_offset) && r.I64(v.window_length) && r.AtEnd())) {
		return false;
	}
	if (follows > 1 || v.window_offset < 0 || v.window_length < 0) {
		return false;
	}
	v.descriptor_follows = follows == 1;
	v.handle             = static_cast<uint64_t>(handle);
	return true;
}

std::vector<uint8_t> EncodeIndexReply(const IndexReply& v)
{
	auto w = Writer{};
	w.U8(static_cast<uint8_t>(v.format));
	w.U8(static_cast<uint8_t>(v.access));
	w.I64(v.total_bytes);
	w.I64(v.archive_length);
	w.Str(v.identity);
	w.I64(v.stat.size);
	w.I64(v.stat.mtime);
	w.U32(static_cast<uint32_t>(v.entries.size()));
	for (const auto& e : v.entries) {
		w.U32(static_cast<uint32_t>(static_cast<int32_t>(e.ordinal)));
		w.Str(e.utf8_path);
		w.Str(e.dos_path);
		w.U8(e.is_dir ? 1 : 0);
		w.I64(e.size);
		w.I64(e.mtime);
		w.U8(e.has_crc ? 1 : 0);
		w.U32(e.crc32);
		w.I64(e.stored_offset);
	}
	return w.Take();
}

bool DecodeIndexReply(const std::span<const uint8_t> bytes, IndexReply& v)
{
	auto r         = Reader(bytes);
	uint8_t format = 0;
	uint8_t access = 0;
	uint32_t count = 0;
	v.entries.clear();
	if (!r.U8(format) || !r.U8(access) || !r.I64(v.total_bytes) ||
	    !r.I64(v.archive_length) || !r.Str(v.identity) ||
	    !r.I64(v.stat.size) || !r.I64(v.stat.mtime) || !r.U32(count)) {
		return false;
	}
	if (format > LastFormat || access > LastAccess ||
	    count > r.Remaining() / MinEncodedEntry) {
		return false;
	}
	v.format = static_cast<ArchiveMount::Format>(format);
	v.access = static_cast<ArchiveMount::Access>(access);
	v.entries.reserve(count);
	for (uint32_t i = 0; i < count; ++i) {
		auto e           = ArchiveMount::Entry{};
		uint32_t ordinal = 0;
		uint8_t is_dir   = 0;
		uint8_t has_crc  = 0;
		if (!r.U32(ordinal) ||
		    static_cast<int32_t>(ordinal) < ArchiveMount::ImpliedOrdinal ||
		    !r.Str(e.utf8_path) || !r.Str(e.dos_path) || !r.U8(is_dir) ||
		    !r.I64(e.size) || !r.I64(e.mtime) || !r.U8(has_crc) ||
		    !r.U32(e.crc32) || !r.I64(e.stored_offset)) {
			v.entries.clear();
			return false;
		}
		if (e.stored_offset < -1) {
			v.entries.clear();
			return false;
		}
		e.ordinal = static_cast<int32_t>(ordinal);
		e.is_dir  = is_dir != 0;
		e.has_crc = has_crc != 0;
		v.entries.push_back(std::move(e));
	}
	return r.AtEnd();
}

std::vector<uint8_t> EncodeMaterializeRequest(const MaterializeRequest& v)
{
	auto w = Writer{};
	w.U32(static_cast<uint32_t>(v.ordinal));
	w.Str(v.target);
	return w.Take();
}

bool DecodeMaterializeRequest(const std::span<const uint8_t> bytes,
                              MaterializeRequest& v)
{
	auto r           = Reader(bytes);
	uint32_t ordinal = 0;
	if (!r.U32(ordinal) || ordinal > MaxOrdinal || !r.Str(v.target) ||
	    !r.AtEnd()) {
		return false;
	}
	v.ordinal = static_cast<int>(ordinal);
	return true;
}

std::vector<uint8_t> EncodeMaterializeAllRequest(const MaterializeAllRequest& v)
{
	auto w = Writer{};
	w.Str(v.directory);
	return w.Take();
}

bool DecodeMaterializeAllRequest(const std::span<const uint8_t> bytes,
                                 MaterializeAllRequest& v)
{
	auto r = Reader(bytes);
	return r.Str(v.directory) && r.AtEnd();
}

std::vector<uint8_t> EncodeStatusReply(const StatusReply& v)
{
	auto w = Writer{};
	w.U8(static_cast<uint8_t>(v.error));
	w.Str(v.detail);
	return w.Take();
}

bool DecodeStatusReply(const std::span<const uint8_t> bytes, StatusReply& v)
{
	auto r    = Reader(bytes);
	uint8_t e = 0;
	if (!r.U8(e) || e > LastError || !r.Str(v.detail) || !r.AtEnd()) {
		return false;
	}
	v.error = static_cast<ArchiveMount::Error>(e);
	return true;
}

} // namespace ArchiveHelper
