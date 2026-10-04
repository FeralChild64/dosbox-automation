// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//
#ifndef DOSBOX_ARCHIVE_HELPER_PROTOCOL_H
#define DOSBOX_ARCHIVE_HELPER_PROTOCOL_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "dos/archive/archive_source.h"

// The wire between the engine and dosbox-automation-archive-helper (design
// D15): a 12-byte header, then the payload, little-endian throughout; a
// string is a u32 byte count and the bytes. One reply per request.
namespace ArchiveHelper {

constexpr uint32_t Magic    = 0x48414244u; // "DBAH" read as little-endian
constexpr uint16_t Version  = 2;
constexpr size_t HeaderSize = 12;

// Requests carry a path and limits; the index reply for 65536 entries
// with long names runs to megabytes.
constexpr uint32_t MaxRequestPayload = 64u * 1024;
constexpr uint32_t MaxReplyPayload   = 64u * 1024 * 1024;

// The helper exits after this many requests and the client respawns it,
// so decoder state cannot pile up over a long session.
constexpr int RequestsPerInstance = 256;

enum class Type : uint8_t {
	Hello          = 1,
	Open           = 2,
	Materialize    = 3,
	MaterializeAll = 4,
	Quit           = 5,
	HelloReply     = 0x41,
	IndexReply     = 0x42,
	StatusReply    = 0x43,
	// Honoured only by a helper started with --test-hooks, which the
	// engine never passes. Payload: u32 ordinal, or u32 MiB to allocate.
	TestCrashOn  = 0x80,
	TestHangOn   = 0x81,
	TestAllocate = 0x82,
};

class Writer {
public:
	void U8(uint8_t v);
	void U16(uint16_t v);
	void U32(uint32_t v);
	void I64(int64_t v);
	void Str(std::string_view s);
	std::vector<uint8_t> Take();

private:
	std::vector<uint8_t> bytes = {};
};

class Reader {
public:
	explicit Reader(std::span<const uint8_t> bytes);
	bool U8(uint8_t& v);
	bool U16(uint16_t& v);
	bool U32(uint32_t& v);
	bool I64(int64_t& v);
	bool Str(std::string& s);
	bool AtEnd() const;
	size_t Position() const;
	size_t Remaining() const;

private:
	std::span<const uint8_t> bytes = {};
	size_t pos                     = 0;
};

std::vector<uint8_t> EncodeFrame(Type type, std::span<const uint8_t> payload);

// False on a short header, a wrong magic or version, an unknown type, or a
// length over max_payload; the caller then ends the conversation.
bool DecodeHeader(std::span<const uint8_t> header, uint32_t max_payload,
                  Type& type, uint32_t& payload_length);

struct HelloReply {
	uint16_t version              = 0;
	uint32_t libarchive_version   = 0;
	std::string libarchive_string = {};
};

struct OpenRequest {
	std::string path = {}; // UTF-8; a label only when a descriptor comes
	ArchiveMount::Limits limits = {};
	// POSIX: the archive descriptor arrives as SCM_RIGHTS with this frame.
	// Windows: handle is the inheritable duplicate's value in the child
	// (the same value as in the engine), 0 when none.
	bool descriptor_follows = false;
	uint64_t handle         = 0;
};

struct IndexReply {
	ArchiveMount::Format format = ArchiveMount::Format::Zip;
	ArchiveMount::Access access = ArchiveMount::Access::PerMember;
	int64_t total_bytes         = 0;
	int64_t archive_length      = 0;
	std::string identity        = {};
	ArchiveMount::StatPair stat = {};
	std::vector<ArchiveMount::Entry> entries = {};
};

struct MaterializeRequest {
	int ordinal        = 0;
	std::string target = {}; // UTF-8 path the helper writes
};

struct MaterializeAllRequest {
	std::string directory = {};
};

struct StatusReply {
	ArchiveMount::Error error = ArchiveMount::Error::None;
	std::string detail        = {};
};

// Nothing the helper reports is trusted: the reason an index breaks the
// limits or the reader's own shape (sizes, ordinals, paths), or nothing.
std::optional<std::string> CheckIndex(const IndexReply& index,
                                      const ArchiveMount::Limits& limits);

std::vector<uint8_t> EncodeHelloReply(const HelloReply& v);
bool DecodeHelloReply(std::span<const uint8_t> bytes, HelloReply& v);
std::vector<uint8_t> EncodeOpenRequest(const OpenRequest& v);
bool DecodeOpenRequest(std::span<const uint8_t> bytes, OpenRequest& v);
std::vector<uint8_t> EncodeIndexReply(const IndexReply& v);
bool DecodeIndexReply(std::span<const uint8_t> bytes, IndexReply& v);
std::vector<uint8_t> EncodeMaterializeRequest(const MaterializeRequest& v);
bool DecodeMaterializeRequest(std::span<const uint8_t> bytes, MaterializeRequest& v);
std::vector<uint8_t> EncodeMaterializeAllRequest(const MaterializeAllRequest& v);
bool DecodeMaterializeAllRequest(std::span<const uint8_t> bytes,
                                 MaterializeAllRequest& v);
std::vector<uint8_t> EncodeStatusReply(const StatusReply& v);
bool DecodeStatusReply(std::span<const uint8_t> bytes, StatusReply& v);

} // namespace ArchiveHelper

#endif // DOSBOX_ARCHIVE_HELPER_PROTOCOL_H
