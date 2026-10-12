// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//
#ifndef DOSBOX_ARCHIVE_SOURCE_H
#define DOSBOX_ARCHIVE_SOURCE_H

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace ArchiveMount {

enum class Format { Zip, Rar, Rar5, SevenZip, Tar, Raw, Isz };

// Whether one member can be reached without inflating the ones before
// it (design D5): zip through the seekable reader, RAR v4 by header
// walking; 7z, RAR5 and every tar.* only in one sequential pass.
enum class Access { PerMember, Sequential };

// Travels over the helper protocol as a byte: append, never reorder.
enum class Error : uint8_t {
	None,
	OpenFailed,
	NotAnArchive,
	TooLarge, // declared total or one member over the cap
	TooManyEntries,
	Encrypted,
	MultiVolume,
	SolidUnsupported, // solid RAR v4: libarchive cannot read it at all
	EntryRejected,    // a name failed the rules; detail names the entry
	Corrupt,          // libarchive reported damage or a CRC mismatch
	Changed,          // the archive's size or mtime moved since the index
	Unsupported,  // a member uses a compression method nobody here reads
	NotFound,     // no entry with that ordinal
	HelperFailed, // the helper crashed, exited or broke the protocol
	Timeout,      // the helper passed the engine's clock and was killed
};

const char* ErrorText(Error error);

struct Limits {
	int64_t max_entries      = 65536;
	int64_t max_total_bytes  = 4ll * 1024 * 1024 * 1024;
	int64_t max_member_bytes = 2ll * 1024 * 1024 * 1024;
};

// The ordinal of a directory the archive implies but has no header for.
constexpr int ImpliedOrdinal = -1;

struct Entry {
	int ordinal           = 0;  // header index from 0, or ImpliedOrdinal
	std::string utf8_path = {}; // as normalized: separators '/', trimmed
	std::string dos_path  = {}; // uppercase 8.3 components, backslashes
	bool is_dir           = false;
	int64_t size          = 0;
	int64_t mtime         = 0;
	bool has_crc          = false;
	uint32_t crc32        = 0;
	// Nested design 2.2: where the member's bytes start when they can be
	// read in place as an archive (archive-named, stored, unencrypted, a
	// zip signature first, inside the file); -1 otherwise and for a directory.
	int64_t stored_offset = -1;
};

// The archive file's size and mtime when it was indexed (design 1.6).
struct StatPair {
	int64_t size  = 0;
	int64_t mtime = 0;
};

// A byte range of a file read as an archive in its own right (nested
// design N6). Offsets are absolute in the file the descriptor names.
struct ByteWindow {
	int64_t offset = 0;
	int64_t length = 0;
};

// The in-process reader and the helper client both implement this, so
// Phase 2's drive never knows which one serves it.
class ArchiveSource {
public:
	virtual ~ArchiveSource() = default;

	virtual Format GetFormat() const                  = 0;
	virtual Access GetAccess() const                  = 0;
	virtual const std::vector<Entry>& Entries() const = 0;
	virtual int64_t TotalBytes() const                = 0;
	virtual int64_t ArchiveLength() const             = 0;
	virtual const std::string& IdentityHash() const   = 0;
	virtual StatPair IndexedStat() const              = 0;

	// Contract: out holds exactly the declared size, or the error says why.
	virtual Error Materialize(int ordinal, std::vector<uint8_t>& out,
	                          std::string* detail) = 0;
	// Contract: target appears whole or not at all (temp name, then
	// rename) and carries the entry's mtime.
	virtual Error MaterializeToFile(int ordinal,
	                                const std::filesystem::path& target,
	                                std::string* detail) = 0;
	// Contract: one pass, each regular file at directory/<ordinal>; the
	// only way a sequential format is served.
	virtual Error MaterializeAll(const std::filesystem::path& directory,
	                             std::string* detail) = 0;
};

} // namespace ArchiveMount

#endif // DOSBOX_ARCHIVE_SOURCE_H
