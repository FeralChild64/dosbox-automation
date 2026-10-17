// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//
#include "dos/archive/archive_reader.h"
#include "dos/archive/random_name.h"

#include "augra/log.h"

#include "dos/archive/archive_identity.h"
#include "dos/archive/descriptor.h"
#include "dos/archive/entry_names.h"
#include "dos/archive/exclusive_file.h"
#include "dos/archive/head_sniff.h"
#include "dos/archive/isz_image.h"
#include "dos/archive/legacy_zip.h"
#include "dos/archive/member_path.h"
#include "dos/archive/rar4_headers.h"
#include "dos/archive/zip_central_directory.h"
#include "utils/checks.h"

#include <archive.h>
#include <archive_entry.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <zlib.h>

#if defined(WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

CHECK_NARROWING();

namespace ArchiveMount {

namespace {

constexpr auto LogComponent     = "archive";
constexpr size_t ReadBlockBytes = 64 * 1024;

struct Descriptor {
	int fd = -1;
	~Descriptor()
	{
		CloseDescriptor(fd);
	}
};

int duplicate_at(const int fd, const int64_t offset)
{
	const auto dup_fd = DuplicateNoInherit(fd);
	if (dup_fd >= 0) {
#if defined(WIN32)
		_lseeki64(dup_fd, offset, SEEK_SET);
#else
		lseek(dup_fd, offset, SEEK_SET);
#endif
	}
	return dup_fd;
}

int duplicate_at_start(const int fd)
{
	return duplicate_at(fd, 0);
}

// libarchive over [offset, offset + length) of a descriptor (nested
// design 2.1): reads stop at the window's end, a seek outside it fails,
// and a file that ends inside the window is an error, not a short read.
struct WindowClient {
	int fd                      = -1;
	int64_t offset              = 0;
	int64_t length              = 0;
	int64_t pos                 = 0;
	std::vector<uint8_t> buffer = std::vector<uint8_t>(ReadBlockBytes);
};

// libarchive's fd client never closes the descriptor it is given, so
// the handle owns its duplicate and closes it after the free.
struct Handle {
	struct archive* a                    = nullptr;
	int fd                               = -1;
	std::unique_ptr<WindowClient> window = {};
	~Handle()
	{
		if (a) {
			archive_read_free(a);
		}
		CloseDescriptor(fd);
	}
};

la_ssize_t window_read(struct archive* a, void* data, const void** out)
{
	auto& w = *static_cast<WindowClient*>(data);
	*out    = w.buffer.data();
	const auto want = std::min<int64_t>(static_cast<int64_t>(w.buffer.size()),
	                                    w.length - w.pos);
	if (want <= 0) {
		return 0;
	}
#if defined(WIN32)
	if (_lseeki64(w.fd, w.offset + w.pos, SEEK_SET) < 0) {
		archive_set_error(a, errno, "seek failed inside the archive window");
		return ARCHIVE_FATAL;
	}
	const auto got = _read(w.fd, w.buffer.data(), static_cast<unsigned int>(want));
#else
	if (lseek(w.fd, w.offset + w.pos, SEEK_SET) < 0) {
		archive_set_error(a, errno, "seek failed inside the archive window");
		return ARCHIVE_FATAL;
	}
	const auto got = read(w.fd, w.buffer.data(), static_cast<size_t>(want));
#endif
	if (got <= 0) {
		archive_set_error(a,
		                  got < 0 ? errno : EIO,
		                  "the file ends inside the archive window");
		return ARCHIVE_FATAL;
	}
	w.pos += got;
	return got;
}

la_int64_t window_seek(struct archive* a, void* data, const la_int64_t where,
                       const int whence)
{
	auto& w        = *static_cast<WindowClient*>(data);
	int64_t target = where;
	if (whence == SEEK_CUR) {
		target = w.pos + where;
	} else if (whence == SEEK_END) {
		target = w.length + where;
	}
	if (target < 0 || target > w.length) {
		archive_set_error(a, EINVAL, "seek outside the archive window");
		return ARCHIVE_FATAL;
	}
	w.pos = target;
	return target;
}

la_int64_t window_skip(struct archive*, void* data, const la_int64_t request)
{
	auto& w         = *static_cast<WindowClient*>(data);
	const auto step = std::clamp<int64_t>(request, 0, w.length - w.pos);
	w.pos += step;
	return step;
}

enum class HandleStatus {
	Ok,
	DescriptorFailed,
	NotRecognized, // libarchive bids the format at open; no bidder took it
};

// The first bytes of the file, read once through a duplicate; every
// sniff below is a pure function of them.
struct Head {
	std::array<uint8_t, HeadBytes> bytes = {};
	size_t size                          = 0;
	std::span<const uint8_t> view() const
	{
		return {bytes.data(), size};
	}
};

Head read_head(const int fd, const int64_t offset)
{
	auto head         = Head{};
	const auto dup_fd = duplicate_at(fd, offset);
	if (dup_fd < 0) {
		return head;
	}
#if defined(WIN32)
	const auto got = _read(dup_fd,
	                       head.bytes.data(),
	                       static_cast<unsigned int>(head.bytes.size()));
#else
	const auto got = read(dup_fd, head.bytes.data(), head.bytes.size());
#endif
	CloseDescriptor(dup_fd);
	head.size = got > 0 ? static_cast<size_t>(got) : 0;
	return head;
}

// Only the D12 filters, and only where the library decodes them itself:
// every filter answers ARCHIVE_WARN when it would run an external program
// instead (filter_all adds lrzip and grzip, which always do).
struct FilterSet {
	std::vector<int (*)(struct archive*)> usable = {};
	ZipCentralDirectory::Decoders decoders       = {};
};

const FilterSet& probe_filters()
{
	static const auto set = [] {
		struct Probe {
			const char* name;
			int (*support)(struct archive*);
			bool ZipCentralDirectory::Decoders::* decoder;
		};
		using Decoders       = ZipCentralDirectory::Decoders;
		const Probe probes[] = {
		        { "gzip",  archive_read_support_filter_gzip,  &Decoders::gzip},
		        {"bzip2", archive_read_support_filter_bzip2, &Decoders::bzip2},
		        {   "xz",    archive_read_support_filter_xz,    &Decoders::xz},
		        { "lzma",  archive_read_support_filter_lzma,    &Decoders::xz},
		        { "zstd",  archive_read_support_filter_zstd,  &Decoders::zstd}
                };
		auto result = FilterSet{};
		for (const auto& probe : probes) {
			struct archive* trial = archive_read_new();
			const auto r          = probe.support(trial);
			archive_read_free(trial);
			if (r == ARCHIVE_OK) {
				result.usable.push_back(probe.support);
			} else {
				if (probe.decoder) {
					result.decoders.*probe.decoder = false;
				}
				augra::log_warn(LogComponent,
				                "libarchive has no built-in %s decoder; "
				                "archives and zip members using it are refused",
				                probe.name);
			}
		}
		return result;
	}();
	return set;
}

std::optional<ZipCentralDirectory::Decoders> decoders_for_tests = {};

const ZipCentralDirectory::Decoders& host_decoders()
{
	return decoders_for_tests ? *decoders_for_tests : probe_filters().decoders;
}

// A filter libarchive would run as an external program is never registered;
// Open refuses such a head as Unsupported before it gets here.
void register_one_filter(struct archive* a, const HeadKind kind)
{
	int (*support)(struct archive*) = nullptr;
	switch (kind) {
	case HeadKind::Gzip: support = archive_read_support_filter_gzip; break;
	case HeadKind::Bzip2:
		support = archive_read_support_filter_bzip2;
		break;
	case HeadKind::Xz: support = archive_read_support_filter_xz; break;
	case HeadKind::Lzma: support = archive_read_support_filter_lzma; break;
	case HeadKind::Zstd: support = archive_read_support_filter_zstd; break;
	default: return;
	}
	const auto& usable = probe_filters().usable;
	if (std::find(usable.begin(), usable.end(), support) != usable.end()) {
		support(a);
	}
}

// The first block of a compressed stream: libarchive's tar reader bids on
// a block of zeros, the start of every ISO and of a blank boot sector, so
// only a tar header by its checksum keeps a compressed image name a tar.
bool decompressed_head_is_tar(const int fd, const HeadKind kind)
{
	auto handle = Handle{};
	handle.a    = archive_read_new();
	archive_read_support_format_raw(handle.a);
	register_one_filter(handle.a, kind);
	handle.fd                = duplicate_at_start(fd);
	struct archive_entry* ae = nullptr;
	if (handle.fd < 0 ||
	    archive_read_open_fd(handle.a, handle.fd, ReadBlockBytes) != ARCHIVE_OK ||
	    archive_read_next_header(handle.a, &ae) != ARCHIVE_OK) {
		return false;
	}
	std::array<uint8_t, HeadBytes> block = {};
	size_t got                           = 0;
	while (got < block.size()) {
		const auto n = archive_read_data(handle.a,
		                                 block.data() + got,
		                                 block.size() - got);
		if (n <= 0) {
			break;
		}
		got += static_cast<size_t>(n);
	}
	return SniffHead(std::span<const uint8_t>(block.data(), got)).kind ==
	       HeadKind::Tar;
}

// One read handle over a dup of the kept descriptor (design 1.6); the
// dups share one offset, so only one handle reads at a time. The head
// picks the reader: a zip end record in a RAR's tail outbids libarchive.
HandleStatus open_handle(const int fd, const HeadKind kind, const bool cd_found,
                         const bool raw_allowed,
                         const std::optional<ByteWindow>& window,
                         Handle& handle, std::string* detail)
{
	handle.a = archive_read_new();
	switch (kind) {
	case HeadKind::Zip:
		archive_read_support_format_zip_seekable(handle.a);
		break;
	case HeadKind::Rar4: archive_read_support_format_rar(handle.a); break;
	case HeadKind::Rar5: archive_read_support_format_rar5(handle.a); break;
	case HeadKind::SevenZip:
		archive_read_support_format_7zip(handle.a);
		break;
	case HeadKind::Tar: archive_read_support_format_tar(handle.a); break;
	case HeadKind::Gzip:
	case HeadKind::Bzip2:
	case HeadKind::Xz:
	case HeadKind::Zstd:
	case HeadKind::Lzma:
		// A compressed stream holds a tar or nothing we mount (D12), or
		// one image when its name and its first block say so (plan 4b).
		if (raw_allowed) {
			archive_read_support_format_raw(handle.a);
		} else {
			archive_read_support_format_tar(handle.a);
		}
		break;
	// The ISZ reader is ours; libarchive reads no ISZ.
	case HeadKind::Isz: break;
	case HeadKind::Iso9660:
		archive_read_support_format_iso9660(handle.a);
		break;
	case HeadKind::Unknown:
		archive_read_support_format_zip_seekable(handle.a);
		if (!cd_found) {
			archive_read_support_format_rar(handle.a);
			archive_read_support_format_rar5(handle.a);
			archive_read_support_format_7zip(handle.a);
			archive_read_support_format_tar(handle.a);
		}
		break;
	}
	for (const auto filter : FiltersForHead(kind)) {
		register_one_filter(handle.a, filter);
	}
	// Where copyfile.h exists libarchive hides __MACOSX entries by
	// default; every host must walk the same set for the pairing to hold.
	archive_read_set_format_option(handle.a, "zip", "mac-ext", nullptr);
	// tar carries bytes; the house writes UTF-8 names into tars
	archive_read_set_format_option(handle.a, "tar", "hdrcharset", "UTF-8");
	if (window) {
		handle.fd = duplicate_at(fd, window->offset);
		if (handle.fd < 0) {
			if (detail) {
				*detail = "cannot duplicate the archive descriptor";
			}
			return HandleStatus::DescriptorFailed;
		}
		handle.window         = std::make_unique<WindowClient>();
		handle.window->fd     = handle.fd;
		handle.window->offset = window->offset;
		handle.window->length = window->length;
		archive_read_set_read_callback(handle.a, window_read);
		archive_read_set_seek_callback(handle.a, window_seek);
		archive_read_set_skip_callback(handle.a, window_skip);
		archive_read_set_callback_data(handle.a, handle.window.get());
		if (archive_read_open1(handle.a) != ARCHIVE_OK) {
			if (detail) {
				const auto* text = archive_error_string(handle.a);
				*detail = text ? text : "open failed";
			}
			return HandleStatus::NotRecognized;
		}
		return HandleStatus::Ok;
	}
	handle.fd = duplicate_at_start(fd);
	if (handle.fd < 0) {
		if (detail) {
			*detail = "cannot duplicate the archive descriptor";
		}
		return HandleStatus::DescriptorFailed;
	}
	if (archive_read_open_fd(handle.a, handle.fd, ReadBlockBytes) != ARCHIVE_OK) {
		if (detail) {
			const auto* text = archive_error_string(handle.a);
			*detail          = text ? text : "open failed";
		}
		return HandleStatus::NotRecognized;
	}
	return HandleStatus::Ok;
}

const char* zip_method_name(const uint16_t method)
{
	switch (method) {
	case 1: return "shrunk";
	case 2:
	case 3:
	case 4:
	case 5: return "reduced";
	case 6: return "imploded";
	case 7: return "tokenized";
	case 9: return "deflate64";
	case 10: return "PKWARE DCL imploded";
	case 99: return "AES encrypted";
	default: return "unknown";
	}
}

// Nested design 2.2: the range of a member stored as plain bytes, from
// the directory's sizes (check_local_headers judged the local header)
// and the local header's name and extra lengths; the zip head is Open's.
int64_t stored_offset_of(const ZipCentralDirectory::Record& rec,
                         const int64_t archive_length)
{
	const auto length = static_cast<uint64_t>(archive_length);
	if (rec.method != 0 || rec.encrypted ||
	    rec.compressed_size != rec.uncompressed_size || rec.data_offset > length ||
	    rec.uncompressed_size > length - rec.data_offset) {
		return -1;
	}
	return static_cast<int64_t>(rec.data_offset);
}

bool has_control_bytes(const std::string& raw)
{
	for (const auto c : raw) {
		const auto byte = static_cast<unsigned char>(c);
		if (byte < 0x20 || byte == 0x7f) {
			return true;
		}
	}
	return false;
}

std::optional<Format> format_of(struct archive* a)
{
	switch (archive_format(a) & ARCHIVE_FORMAT_BASE_MASK) {
	case ARCHIVE_FORMAT_ZIP: return Format::Zip;
	case ARCHIVE_FORMAT_RAR: return Format::Rar;
	case ARCHIVE_FORMAT_RAR_V5: return Format::Rar5;
	case ARCHIVE_FORMAT_RAW: return Format::Raw;
	case ARCHIVE_FORMAT_7ZIP: return Format::SevenZip;
	case ARCHIVE_FORMAT_TAR: return Format::Tar;
	case ARCHIVE_FORMAT_ISO9660: return Format::Iso9660;
	default: return {};
	}
}

Access access_of(const Format format)
{
	switch (format) {
	case Format::Zip:
	case Format::Rar:
	case Format::Raw:
	case Format::Isz:
	case Format::Iso9660: return Access::PerMember;
	case Format::Rar5:
	case Format::SevenZip:
	case Format::Tar: return Access::Sequential;
	}
	return Access::Sequential;
}

// RAR v4 main header (4.x technote, "Archive header"): 7-byte signature,
// block CRC (2), type (1, 0x73), flags (2); MHD_VOLUME bit 0, MHD_SOLID
// bit 3. libarchive reports solid only at data read, too late for a mount.
constexpr uint16_t Rar4FlagVolume   = 0x0001;
constexpr uint16_t Rar4FlagSolid    = 0x0008;
constexpr uint16_t Rar4FlagPassword = 0x0080; // block headers encrypted

// Both flag readers run only on a head the sniff already matched.
std::optional<uint16_t> rar4_main_flags(const Head& head)
{
	const auto bytes = head.view();
	if (bytes.size() < 12 || bytes[9] != 0x73) {
		return {};
	}
	return static_cast<uint16_t>(bytes[10] | (bytes[11] << 8));
}

// RAR 5.0 main header (rarlab technote): signature (8), CRC32, vints for
// header size, type (1 = main), header flags, an extra-area size if flag
// bit 0, then the archive flags (bit 0 = volume, which libarchive never says).
constexpr uint64_t Rar5ArchiveFlagVolume = 0x0001;

std::optional<uint64_t> rar5_archive_flags(const Head& head)
{
	const auto bytes = head.view();
	if (bytes.size() < 12) {
		return {};
	}
	size_t pos      = 12;
	const auto vint = [&](uint64_t& out) {
		out = 0;
		for (auto shift = 0; shift < 64; shift += 7) {
			if (pos >= bytes.size()) {
				return false;
			}
			const auto byte = bytes[pos++];
			out |= static_cast<uint64_t>(byte & 0x7f) << shift;
			if (!(byte & 0x80)) {
				return true;
			}
		}
		return false;
	};
	// Header flags bit 0 announces an extra area and bit 1 a data area;
	// each adds a size vint before the archive flags.
	uint64_t header_size = 0, header_type = 0, header_flags = 0,
	         skipped = 0, archive_flags = 0;
	if (!vint(header_size) || !vint(header_type) || !vint(header_flags) ||
	    header_type != 1) {
		return {};
	}
	if ((header_flags & 0x01) && !vint(skipped)) {
		return {};
	}
	if ((header_flags & 0x02) && !vint(skipped)) {
		return {};
	}
	if (!vint(archive_flags)) {
		return {};
	}
	return archive_flags;
}

// The file clock holds about 1882 to 2466 on libstdc++; a FILETIME of
// zero in a 7z (1601) or a base-256 tar mtime would overflow it.
constexpr int64_t MinStampSeconds = 0;
constexpr int64_t MaxStampSeconds = 4102444800; // 2100-01-01

// No clock_cast on AlmaLinux 8's libstdc++. The two clocks' epoch gap is
// whole seconds everywhere, so the nanoseconds between the two reads are
// rounded away; unrounded, clang's read order stamped a second early.
std::filesystem::file_time_type file_time_from_unix(const int64_t seconds)
{
	using file_duration = std::filesystem::file_time_type::duration;
	const auto sys_now  = std::chrono::system_clock::now();
	const auto file_now = std::filesystem::file_time_type::clock::now();
	const auto gap      = std::chrono::round<std::chrono::seconds>(
                file_now.time_since_epoch() -
                std::chrono::duration_cast<file_duration>(sys_now.time_since_epoch()));
	return std::filesystem::file_time_type(std::chrono::duration_cast<file_duration>(
	        gap + std::chrono::seconds(
	                      std::clamp(seconds, MinStampSeconds, MaxStampSeconds))));
}

// DOS date and time packed as the zip and the RAR4 header store them
// (time low, date high), the same bytes on every host; the other formats
// carry a Unix time.
int64_t identity_time_of(const Format format, const int64_t mtime,
                         const uint32_t stored_dos_time)
{
	switch (format) {
	case Format::Zip:
	case Format::Rar: return stored_dos_time;
	case Format::Rar5:
	case Format::SevenZip:
	case Format::Tar:
	case Format::Raw:
	case Format::Isz:
	case Format::Iso9660: return mtime;
	}
	return mtime;
}

const char* error_text_or(struct archive* a, const char* fallback)
{
	const auto* text = archive_error_string(a);
	return text ? text : fallback;
}

} // namespace

struct ArchiveReader::Impl {
	Descriptor file            = {};
	std::filesystem::path path = {};
	HeadKind kind              = HeadKind::Unknown;
	Limits limits              = {};
	Format format              = Format::Zip;
	Access access              = Access::Sequential;
	StatPair indexed_stat      = {};
	// Nested design 2.1: set when the archive is a range of the file; the
	// length below is the window's then, the file's otherwise.
	std::optional<ByteWindow> window                = {};
	int64_t archive_length                          = 0;
	std::vector<Entry> entries                      = {};
	std::unordered_map<std::string, size_t> by_path = {};
	std::unordered_map<int, size_t> by_ordinal      = {};
	int64_t total_bytes                             = 0;
	std::string identity                            = {};
	// Shrink and implode members by ordinal: libarchive walks them,
	// legacy_zip decodes them (ada-unr8).
	std::unordered_map<int, ZipCentralDirectory::Record> legacy = {};
	// Plan 4b: the ISZ reader's image, or the raw reader's permission.
	std::optional<Isz::Image> isz = {};
	bool raw_allowed              = false;
	// Every handle shares one file offset, so two calls at once would
	// interleave their reads; the guard makes a second caller fail loudly.
	std::atomic<bool> in_use = false;

	struct Use {
		std::atomic<bool>& flag;
		explicit Use(std::atomic<bool>& f) : flag(f)
		{
			const auto was = flag.exchange(true);
			assert(!was && "ArchiveReader used from two threads at once");
			(void)was;
		}
		~Use()
		{
			flag = false;
		}
	};

	// The listing's time column per entry: zip and RAR4 store DOS date and
	// time, which libarchive converts through mktime in the host's zone;
	// the identity uses the DOS fields themselves, the same on every host.
	std::vector<int64_t> identity_times = {};

	void add(Entry entry, const int64_t identity_time)
	{
		by_path[entry.utf8_path] = entries.size();
		if (entry.ordinal >= 0) {
			by_ordinal[entry.ordinal] = entries.size();
		}
		identity_times.push_back(identity_time);
		entries.push_back(std::move(entry));
	}

	const Entry* at_path(const std::string& utf8_path) const
	{
		const auto it = by_path.find(utf8_path);
		return it == by_path.end() ? nullptr : &entries[it->second];
	}

	Error check_unchanged(std::string* detail) const
	{
		auto now = StatPair{};
		if (!StatDescriptor(file.fd, now) || now.size != indexed_stat.size ||
		    now.mtime != indexed_stat.mtime) {
			if (detail) {
				*detail = "archive size or mtime changed since it was indexed";
			}
			return Error::Changed;
		}
		return Error::None;
	}

	// Reads exactly the declared size; more or less is corruption.
	Error read_member(struct archive* a, const Entry& entry,
	                  const std::function<bool(const uint8_t*, size_t)>& sink,
	                  std::string* detail) const
	{
		std::vector<uint8_t> block(ReadBlockBytes);
		int64_t total = 0;
		while (true) {
			const auto got = archive_read_data(a,
			                                   block.data(),
			                                   block.size());
			if (got < 0) {
				if (detail) {
					*detail = error_text_or(a, "read failed");
				}
				return Error::Corrupt;
			}
			if (got == 0) {
				break;
			}
			total += got;
			if (total > entry.size) {
				if (detail) {
					*detail = "member longer than its declared size";
				}
				return Error::Corrupt;
			}
			if (!sink(block.data(), static_cast<size_t>(got))) {
				if (detail) {
					*detail = "cannot write the member";
				}
				return Error::OpenFailed;
			}
		}
		if (total != entry.size) {
			if (detail) {
				*detail = "member shorter than its declared size";
			}
			return Error::Corrupt;
		}
		return Error::None;
	}

	Error read_legacy(const ZipCentralDirectory::Record& rec, const Entry& entry,
	                  const std::function<bool(const uint8_t*, size_t)>& sink,
	                  std::string* detail) const
	{
		auto say = [&](const std::string& why) {
			if (detail) {
				*detail = why;
			}
			return Error::Corrupt;
		};
		const auto base = window ? static_cast<uint64_t>(window->offset) : 0;
		const auto end = static_cast<uint64_t>(archive_length);
		if (rec.local_header_offset > end ||
		    30 > end - rec.local_header_offset) {
			return say("local header past the end of the archive");
		}
		std::vector<uint8_t> local = {};
		if (ZipCentralDirectory::ReadRange(
		            file.fd, base + rec.local_header_offset, 30, local) !=
		    ZipCentralDirectory::Error::None) {
			return say("cannot read the local header");
		}
		const auto start = rec.local_header_offset + 30 +
		                   static_cast<uint64_t>(local[26] |
		                                         (local[27] << 8)) +
		                   static_cast<uint64_t>(local[28] |
		                                         (local[29] << 8));
		if (start > end || rec.compressed_size > end - start) {
			return say("member data runs past the end of the archive");
		}
		std::vector<uint8_t> data = {};
		switch (ZipCentralDirectory::ReadRange(
		        file.fd, base + start, rec.compressed_size, data)) {
		case ZipCentralDirectory::Error::None: break;
		case ZipCentralDirectory::Error::Truncated:
			return say("member data runs past the end of the archive");
		default:
			return say("cannot read the member data (an I/O error, or over " +
			           std::to_string(ZipCentralDirectory::MaxRangeBytes) +
			           " bytes)");
		}
		size_t pos                     = 0;
		const LegacyZip::Source source = [&](uint8_t* buffer,
		                                     const size_t capacity) {
			const auto n = std::min(capacity, data.size() - pos);
			if (n > 0) {
				std::memcpy(buffer, data.data() + pos, n);
			}
			pos += n;
			return n;
		};
		uint32_t crc              = 0;
		const LegacyZip::Sink out = [&](const uint8_t* bytes,
		                                const size_t size) {
			crc = LegacyZip::Crc32(crc, bytes, size);
			return sink(bytes, size);
		};
		const auto size   = static_cast<uint64_t>(entry.size);
		const auto result = rec.method == LegacyZip::MethodShrink
		                          ? LegacyZip::Unshrink(source, size, out, detail)
		                          : LegacyZip::Explode(source,
		                                               rec.compressed_size,
		                                               size,
		                                               (rec.flags & 2) != 0,
		                                               (rec.flags & 4) != 0,
		                                               out,
		                                               detail);
		if (result == LegacyZip::Result::SinkFailed) {
			if (detail) {
				*detail = "cannot write the member";
			}
			return Error::OpenFailed;
		}
		if (result != LegacyZip::Result::Ok) {
			return Error::Corrupt;
		}
		if (crc != rec.crc32) {
			return say("CRC mismatch");
		}
		return Error::None;
	}

	const Entry* find(const int ordinal) const
	{
		const auto it = by_ordinal.find(ordinal);
		return it == by_ordinal.end() ? nullptr : &entries[it->second];
	}

	// Temp name beside the target, renamed only after the declared size
	// arrived and libarchive's CRC check passed, so a crash never leaves a
	// short member under the real name (design 2.1).
	using ByteSink = std::function<bool(const uint8_t*, size_t)>;

	Error write_member(const std::function<Error(const ByteSink&)>& fill,
	                   const Entry& entry, const std::filesystem::path& target,
	                   std::string* detail) const
	{
		std::error_code ec = {};
		std::filesystem::create_directories(target.parent_path(), ec);
		auto temp = target;
		temp += "." + RandomHex(4) + ".part";
		FILE* f = OpenExclusive(temp);
		if (!f) {
			if (detail) {
				*detail = "cannot create " + PathToUtf8(temp);
			}
			return Error::OpenFailed;
		}
		const auto read = fill([&](const uint8_t* data, const size_t size) {
			return fwrite(data, 1, size, f) == size;
		});
		const auto closed = fclose(f) == 0;
		if (read != Error::None || !closed) {
			if (read == Error::None && detail) {
				*detail = "cannot finish writing " + PathToUtf8(temp);
			}
			std::filesystem::remove(temp, ec);
			return read != Error::None ? read : Error::OpenFailed;
		}
		if (entry.mtime < MinStampSeconds || entry.mtime > MaxStampSeconds) {
			augra::log_warn(LogComponent,
			                "%s: mtime %lld of '%s' is outside the file clock, clamped",
			                PathToUtf8(path).c_str(),
			                static_cast<long long>(entry.mtime),
			                PathToUtf8(target).c_str());
		}
		std::filesystem::last_write_time(temp,
		                                 file_time_from_unix(entry.mtime),
		                                 ec);
		if (ec) {
			augra::log_warn(LogComponent,
			                "%s: cannot set the mtime of '%s': %s",
			                PathToUtf8(path).c_str(),
			                PathToUtf8(target).c_str(),
			                ec.message().c_str());
			ec.clear();
		}
		std::filesystem::rename(temp, target, ec);
		if (ec) {
			if (detail) {
				*detail = "cannot rename into " + PathToUtf8(target);
			}
			std::filesystem::remove(temp, ec);
			return Error::OpenFailed;
		}
		return Error::None;
	}

	Error seek_to(Handle& handle, const int ordinal, std::string* detail) const
	{
		if (open_handle(file.fd,
		                kind,
		                format == Format::Zip,
		                raw_allowed,
		                window,
		                handle,
		                detail) != HandleStatus::Ok) {
			return Error::OpenFailed;
		}
		struct archive_entry* ae = nullptr;
		for (auto index = 0;; ++index) {
			const auto r = archive_read_next_header(handle.a, &ae);
			if (r == ARCHIVE_EOF) {
				if (detail) {
					*detail = "ordinal past the end";
				}
				return Error::NotFound;
			}
			if (r < ARCHIVE_OK && r != ARCHIVE_WARN) {
				if (detail) {
					*detail = error_text_or(handle.a,
					                        "header failed");
				}
				return Error::Corrupt;
			}
			if (index == ordinal) {
				return Error::None;
			}
			if (archive_read_data_skip(handle.a) < ARCHIVE_OK) {
				if (detail) {
					*detail = error_text_or(handle.a,
					                        "skip failed");
				}
				return Error::Corrupt;
			}
		}
	}
};

bool FilterDecodable(const HeadKind kind, const ZipCentralDirectory::Decoders& decoders)
{
	switch (kind) {
	case HeadKind::Gzip: return decoders.gzip;
	case HeadKind::Bzip2: return decoders.bzip2;
	case HeadKind::Xz:
	case HeadKind::Lzma: return decoders.xz;
	case HeadKind::Zstd: return decoders.zstd;
	default: return true;
	}
}

std::vector<HeadKind> FiltersForHead(const HeadKind head)
{
	switch (head) {
	case HeadKind::Gzip:
	case HeadKind::Bzip2:
	case HeadKind::Xz:
	case HeadKind::Lzma:
	case HeadKind::Zstd: return {head};
	case HeadKind::Isz:
	case HeadKind::Iso9660: return {};
	default:
		return {HeadKind::Gzip, HeadKind::Bzip2, HeadKind::Xz, HeadKind::Zstd};
	}
}

void SetDecodersForTests(const std::optional<ZipCentralDirectory::Decoders> decoders)
{
	decoders_for_tests = decoders;
}

const char* ErrorText(const Error error)
{
	switch (error) {
	case Error::None: return "";
	case Error::OpenFailed: return "cannot open the archive";
	case Error::NotAnArchive: return "not a recognized archive";
	case Error::TooLarge: return "declared size over the limit";
	case Error::TooManyEntries: return "too many entries";
	case Error::Encrypted: return "password-protected archive";
	case Error::MultiVolume: return "multi-volume archive";
	case Error::SolidUnsupported:
		return "solid RAR v4 archives cannot be read";
	case Error::EntryRejected: return "entry name refused";
	case Error::Corrupt: return "archive damaged";
	case Error::Changed: return "archive changed since it was indexed";
	case Error::Unsupported: return "compression method not supported";
	case Error::NotFound: return "no such entry";
	case Error::HelperFailed:
		return "the archive helper failed or spoke out of protocol";
	case Error::Timeout: return "the archive helper did not answer in time";
	}
	return "";
}

ArchiveReader::ArchiveReader()  = default;
ArchiveReader::~ArchiveReader() = default;

OpenResult ArchiveReader::Open(const std::filesystem::path& path, const Limits& limits)
{
	const auto fd = OpenReadOnlyShared(path);
	if (fd < 0) {
		auto result   = OpenResult{};
		result.error  = Error::OpenFailed;
		result.detail = "open failed";
		augra::log_warn(LogComponent,
		                "%s: %s (%s)",
		                PathToUtf8(path).c_str(),
		                ErrorText(result.error),
		                result.detail.c_str());
		return result;
	}
	auto result = Open(fd, path, limits);
	CloseDescriptor(fd);
	return result;
}

OpenResult ArchiveReader::Open(const int fd, const std::filesystem::path& name,
                               const Limits& limits,
                               const std::optional<ByteWindow> window)
{
	auto result     = OpenResult{};
	auto reader     = std::unique_ptr<ArchiveReader>(new ArchiveReader());
	reader->impl    = std::make_unique<Impl>();
	auto& impl      = *reader->impl;
	auto& path      = impl.path;
	impl.path       = name;
	impl.limits     = limits;
	const auto fail = [&](const Error error, const std::string& detail) {
		result.error  = error;
		result.detail = detail;
		augra::log_warn(LogComponent,
		                "%s: %s (%s)",
		                PathToUtf8(path).c_str(),
		                ErrorText(error),
		                detail.c_str());
		return std::move(result);
	};

	impl.file.fd = fd >= 0 ? DuplicateNoInherit(fd) : -1;
	if (impl.file.fd < 0) {
		return fail(Error::OpenFailed,
		            "cannot duplicate the archive descriptor");
	}
	if (!StatDescriptor(impl.file.fd, impl.indexed_stat)) {
		return fail(Error::OpenFailed, "fstat failed");
	}
	impl.window         = window;
	impl.archive_length = impl.indexed_stat.size;
	if (window) {
		constexpr int64_t EndRecordBytes = 22;
		if (window->offset < 0 || window->length < EndRecordBytes ||
		    window->offset > impl.indexed_stat.size ||
		    window->length > impl.indexed_stat.size - window->offset) {
			return fail(Error::OpenFailed,
			            "archive window outside the file: offset " +
			                    std::to_string(window->offset) +
			                    ", length " +
			                    std::to_string(window->length) + ", file " +
			                    std::to_string(impl.indexed_stat.size));
		}
		impl.archive_length = window->length;
	}
	const auto finish = [&] {
		auto listing = std::vector<ArchiveIdentity::ListingEntry>{};
		for (size_t i = 0; i < impl.entries.size(); ++i) {
			const auto& e = impl.entries[i];
			if (!e.is_dir) {
				listing.push_back({e.utf8_path,
				                   e.size,
				                   impl.identity_times[i],
				                   e.has_crc,
				                   e.crc32});
			}
		}
		impl.identity = ArchiveIdentity::Hash(listing, impl.archive_length);
		result.reader = std::move(reader);
		return std::move(result);
	};

	const auto head = read_head(impl.file.fd, window ? window->offset : 0);
	impl.kind       = SniffHead(head.view()).kind;
	// An ISO's head is blank (ECMA-119 8.4); never through a window (N6).
	if (!window && impl.kind == HeadKind::Unknown && ProbeIso9660(impl.file.fd)) {
		impl.kind = HeadKind::Iso9660;
	}
	if (window && impl.kind != HeadKind::Zip) {
		if (impl.kind == HeadKind::Unknown) {
			return fail(Error::NotAnArchive,
			            "no zip signature at the start of the archive window");
		}
		return fail(Error::Unsupported,
		            std::string("only a zip is read through an archive window, not ") +
		                    HeadKindName(impl.kind));
	}
	if (!FilterDecodable(impl.kind, host_decoders())) {
		return fail(Error::Unsupported,
		            std::string("no built-in ") + HeadKindName(impl.kind) +
		                    " decoder on this host");
	}
	const auto filter_head = impl.kind == HeadKind::Gzip ||
	                         impl.kind == HeadKind::Bzip2 ||
	                         impl.kind == HeadKind::Xz ||
	                         impl.kind == HeadKind::Zstd;
	// D13.1: the name decides here as in the engine, which never decodes.
	const bool image_name = filter_head &&
	                        IsCompressedImageName(PathToUtf8(name.filename()));
	if (image_name && decompressed_head_is_tar(impl.file.fd, impl.kind)) {
		return fail(Error::NotAnArchive,
		            "a tar under a compressed image name; a tar archive "
		            "needs a .tar name");
	}
	impl.raw_allowed = image_name;

	// Plan 4b: an ISZ is one ISO from our own reader; libarchive never
	// sees the file. D13.3 names it after the container.
	if (impl.kind == HeadKind::Isz) {
		auto parsed = Isz::Parse(impl.file.fd,
		                         impl.indexed_stat.size,
		                         limits.max_member_bytes);
		if (parsed.error != Error::None) {
			return fail(parsed.error, parsed.detail);
		}
		const auto utf8_name = PathToUtf8(name.stem()) + ".iso";
		auto rejection       = ArchiveNames::Rejection::None;
		const auto split = ArchiveNames::SplitEntryPath(utf8_name, &rejection);
		if (!split || split->is_dir || split->components.size() != 1) {
			return fail(Error::EntryRejected,
			            "'" + utf8_name + "': " +
			                    ArchiveNames::RejectionText(rejection));
		}
		auto names     = ArchiveNames::DosNameTable{};
		const auto dos = names.Assign("", split->components[0]);
		if (!dos) {
			return fail(Error::EntryRejected,
			            "'" + utf8_name + "': no free 8.3 name");
		}
		const auto size = static_cast<int64_t>(parsed.image.total_bytes);
		if (size > limits.max_total_bytes) {
			return fail(Error::TooLarge,
			            "declared total over " +
			                    std::to_string(limits.max_total_bytes) +
			                    " bytes");
		}
		auto content_crc = uint32_t{0};
		auto crc_detail  = std::string();
		if (const auto error = Isz::ContentCrc(
		            impl.file.fd, parsed.image, &content_crc, &crc_detail);
		    error != Error::None) {
			return fail(error, crc_detail);
		}
		auto entry       = Entry{};
		entry.ordinal    = 0;
		entry.utf8_path  = split->components[0];
		entry.dos_path   = *dos;
		entry.size       = size;
		entry.mtime      = impl.indexed_stat.mtime;
		entry.has_crc    = true;
		entry.crc32      = content_crc;
		impl.format      = Format::Isz;
		impl.access      = access_of(Format::Isz);
		impl.total_bytes = size;
		impl.add(entry, identity_time_of(Format::Isz, entry.mtime, 0));
		impl.isz = std::move(parsed.image);
		return finish();
	}
	if (impl.kind == HeadKind::Rar4) {
		if (const auto rar_flags = rar4_main_flags(head)) {
			if (*rar_flags & Rar4FlagVolume) {
				return fail(Error::MultiVolume, "RAR volume flag set");
			}
			if (*rar_flags & Rar4FlagSolid) {
				return fail(Error::SolidUnsupported,
				            "RAR solid flag set");
			}
			// Ciphertext headers would fail the block walk below as
			// damage; the main header names the real reason.
			if (*rar_flags & Rar4FlagPassword) {
				return fail(Error::Encrypted,
				            "RAR header encryption flag set");
			}
		}
	}
	if (impl.kind == HeadKind::Rar5) {
		if (const auto rar5_flags = rar5_archive_flags(head)) {
			if (*rar5_flags & Rar5ArchiveFlagVolume) {
				return fail(Error::MultiVolume,
				            "RAR5 volume flag set");
			}
		}
	}
	// The identity takes the stored FTIME and FILE_CRC from the block
	// walk, paired with libarchive's entries by position (ada-3qeb).
	auto rar4 = Rar4Headers::Result{};
	if (impl.kind == HeadKind::Rar4) {
		rar4 = Rar4Headers::Read(impl.file.fd,
		                         static_cast<uint64_t>(limits.max_entries));
		switch (rar4.error) {
		case Rar4Headers::Error::None: break;
		case Rar4Headers::Error::TooManyEntries:
			return fail(Error::TooManyEntries,
			            "more RAR file headers than the cap");
		case Rar4Headers::Error::NotRar4:
		case Rar4Headers::Error::Truncated:
		case Rar4Headers::Error::Malformed:
			return fail(Error::Corrupt, "RAR block headers do not add up");
		}
	}

	// The zip central directory is read before the libarchive walk opens,
	// so the two never move the shared offset under each other. For any
	// other format the probe answers NotAZip and is ignored.
	const auto cd       = window
	                            ? ZipCentralDirectory::Read(
                                        impl.file.fd,
                                        static_cast<uint64_t>(window->offset),
                                        static_cast<uint64_t>(window->length),
                                        static_cast<uint64_t>(limits.max_entries))
	                            : ZipCentralDirectory::Read(impl.file.fd,
                                                          static_cast<uint64_t>(
                                                                  limits.max_entries));
	const auto cd_found = cd.error != ZipCentralDirectory::Error::NotAZip;
	// A zip by its head, or a zip behind a stub: an unknown head with an
	// end record in the tail (ada-qbxb). Either way the directory judges.
	const auto zip_or_stub = impl.kind == HeadKind::Zip ||
	                         (impl.kind == HeadKind::Unknown && cd_found);

	// The code page decoder turns control bytes into '?' and libarchive's
	// zip reader refuses such names only on some platforms, so the raw
	// names are checked here for one verdict on every host.
	if (impl.kind == HeadKind::Zip && !cd_found) {
		return fail(Error::Corrupt,
		            "zip local header without an end record: truncated, or "
		            "one volume of a spanned set");
	}
	const auto control_byte_verdict = [&]() -> std::optional<std::string> {
		for (size_t i = 0; i < cd.records.size(); ++i) {
			if (has_control_bytes(cd.records[i].raw_name)) {
				return "entry " + std::to_string(i) + ": " +
				       ArchiveNames::RejectionText(
				               ArchiveNames::Rejection::ControlCharacter);
			}
		}
		return {};
	};
	if (zip_or_stub && cd.error == ZipCentralDirectory::Error::None) {
		if (const auto verdict = control_byte_verdict()) {
			return fail(Error::EntryRejected, *verdict);
		}
	}
	// The directory's own verdict, for a zip: at the first header or,
	// when libarchive walks nothing, after the walk.
	const auto directory_verdict =
	        [&]() -> std::optional<std::pair<Error, std::string>> {
		switch (cd.error) {
		case ZipCentralDirectory::Error::None: return {};
		case ZipCentralDirectory::Error::TooManyEntries:
			return std::pair{Error::TooManyEntries,
			                 std::string(ZipCentralDirectory::ErrorText(
			                         cd.error))};
		case ZipCentralDirectory::Error::MultiDisk:
			return std::pair{Error::MultiVolume,
			                 std::string(ZipCentralDirectory::ErrorText(
			                         cd.error))};
		case ZipCentralDirectory::Error::IoFailed:
			return std::pair{Error::OpenFailed,
			                 std::string(ZipCentralDirectory::ErrorText(
			                         cd.error))};
		default:
			return std::pair{Error::Corrupt,
			                 std::string(ZipCentralDirectory::ErrorText(
			                         cd.error))};
		}
	};

	// With a zip head only the zip reader is registered, so the
	// directory's verdict (spanned, over the caps, past the end record)
	// goes out before libarchive bids on it or slurps the directory.
	if (zip_or_stub) {
		if (const auto verdict = directory_verdict()) {
			return fail(verdict->first, verdict->second);
		}
	}

	// zip names and CRCs come from the central directory, paired by
	// position with libarchive's walk, which goes by ascending local header
	// offset (its red-black tree); the probe refused any repeated offset.
	auto zip_records = std::vector<ZipCentralDirectory::Record>{};
	if (cd.error == ZipCentralDirectory::Error::None) {
		zip_records = cd.records;
		std::stable_sort(zip_records.begin(),
		                 zip_records.end(),
		                 [](const auto& a, const auto& b) {
			                 return a.local_header_offset <
			                        b.local_header_offset;
		                 });
	}
	// Nested design 2.2: which members can be read in place, decided here
	// and never in the engine (D15). Only archive-named members get the
	// head read, which bounds the extra reads on a large stored zip.
	auto stored_offsets = std::vector<int64_t>(zip_records.size(), -1);
	for (size_t i = 0; i < zip_records.size(); ++i) {
		const auto& rec = zip_records[i];
		const auto candidate = stored_offset_of(rec, impl.archive_length);
		if (candidate < 0) {
			continue;
		}
		const auto decoded   = ZipCentralDirectory::DecodeName(rec);
		const auto slash     = decoded.find_last_of("/\\");
		const auto file_name = slash == std::string::npos
		                             ? decoded
		                             : decoded.substr(slash + 1);
		if (!IsArchiveName(file_name)) {
			continue;
		}
		const auto base = window ? static_cast<uint64_t>(window->offset) : 0;
		const auto want = std::min<uint64_t>(rec.uncompressed_size, HeadBytes);
		std::vector<uint8_t> member_head = {};
		if (ZipCentralDirectory::ReadRange(impl.file.fd,
		                                   base + static_cast<uint64_t>(candidate),
		                                   want,
		                                   member_head) !=
		    ZipCentralDirectory::Error::None) {
			continue;
		}
		if (SniffHead(member_head).kind == HeadKind::Zip) {
			stored_offsets[i] = candidate;
		}
	}
	auto handle = Handle{};
	auto detail = std::string();
	switch (open_handle(impl.file.fd,
	                    impl.kind,
	                    cd_found,
	                    impl.raw_allowed,
	                    impl.window,
	                    handle,
	                    &detail)) {
	case HandleStatus::Ok: break;
	case HandleStatus::DescriptorFailed:
		return fail(Error::OpenFailed, detail);
	case HandleStatus::NotRecognized:
		return fail(Error::NotAnArchive, detail);
	}
	auto names        = ArchiveNames::DosNameTable{};
	auto seen_paths   = std::unordered_set<std::string>{};
	auto format_known = false;
	auto walked       = 0;

	struct archive_entry* ae = nullptr;
	for (auto ordinal = 0;; ++ordinal) {
		const auto r = archive_read_next_header(handle.a, &ae);
		if (r == ARCHIVE_EOF) {
			walked = ordinal;
			break;
		}
		if (r < ARCHIVE_OK && r != ARCHIVE_WARN) {
			const auto message = std::string(error_text_or(handle.a, ""));
			if (ordinal == 0 && archive_format(handle.a) == 0) {
				return fail(Error::NotAnArchive, message);
			}
			// Both RAR readers raise the encrypted flag before they
			// fail; their messages differ in case, so the flag
			// decides.
			if (archive_read_has_encrypted_entries(handle.a) > 0) {
				return fail(Error::Encrypted, message);
			}
			if (impl.kind == HeadKind::Rar4 &&
			    message.find("solid") != std::string::npos) {
				return fail(Error::SolidUnsupported, message);
			}
			return fail(Error::Corrupt, message);
		}
		if (ordinal == 0) {
			const auto format = format_of(handle.a);
			if (!format) {
				return fail(Error::NotAnArchive, "unsupported format");
			}
			impl.format  = *format;
			impl.access  = access_of(*format);
			format_known = true;
			if (impl.kind == HeadKind::Unknown &&
			    (impl.format == Format::Rar ||
			     impl.format == Format::Rar5)) {
				// A self-extractor: the main header flags were
				// never read, and D12 keeps executables out.
				return fail(Error::NotAnArchive,
				            "RAR data behind a self-extractor stub");
			}
			if (impl.format == Format::Zip) {
				if (const auto verdict = directory_verdict()) {
					return fail(verdict->first, verdict->second);
				}
				if (const auto verdict = control_byte_verdict()) {
					return fail(Error::EntryRejected, *verdict);
				}
			}
		}
		if (ordinal >= limits.max_entries) {
			return fail(Error::TooManyEntries,
			            "more than " + std::to_string(limits.max_entries) +
			                    " entries");
		}
		if (archive_entry_is_encrypted(ae) ||
		    archive_read_has_encrypted_entries(handle.a) > 0) {
			return fail(Error::Encrypted, "encrypted entry");
		}

		const auto filetype = archive_entry_filetype(ae);
		if (filetype != AE_IFREG && filetype != AE_IFDIR) {
			const auto* name = archive_entry_pathname_utf8(ae);
			augra::log_warn(LogComponent,
			                "%s: dropping entry %d '%s': not a file or directory",
			                PathToUtf8(path).c_str(),
			                ordinal,
			                name ? name : "");
			archive_read_data_skip(handle.a);
			continue;
		}

		auto utf8_name       = std::string();
		auto has_crc         = false;
		auto crc             = uint32_t{0};
		auto stored_dos_time = uint32_t{0};
		if (impl.format == Format::Zip) {
			if (static_cast<size_t>(ordinal) >= zip_records.size()) {
				return fail(Error::Corrupt,
				            "more entries than the central directory lists");
			}
			const auto& rec = zip_records[static_cast<size_t>(ordinal)];
			if (rec.encrypted) {
				return fail(Error::Encrypted, "encrypted entry");
			}
			utf8_name       = ZipCentralDirectory::DecodeName(rec);
			stored_dos_time = rec.dos_datetime;
			if (!ZipCentralDirectory::MethodReadable(rec.method,
			                                         host_decoders())) {
				return fail(Error::Unsupported,
				            "'" + utf8_name + "': ZIP method " +
				                    std::to_string(rec.method) + " (" +
				                    zip_method_name(rec.method) +
				                    ")");
			}
			has_crc = true;
			crc     = rec.crc32;
			if (rec.method == LegacyZip::MethodShrink ||
			    rec.method == LegacyZip::MethodImplode) {
				impl.legacy[ordinal] = rec;
			}
		} else if (impl.format == Format::Rar) {
			if (static_cast<size_t>(ordinal) >= rar4.files.size()) {
				return fail(Error::Corrupt,
				            "more entries than the RAR headers list");
			}
			const auto* name = archive_entry_pathname_utf8(ae);
			if (!name) {
				name = archive_entry_pathname(ae);
			}
			utf8_name = name ? name : "";
			has_crc   = true;
			crc = rar4.files[static_cast<size_t>(ordinal)].crc32;
			stored_dos_time =
			        rar4.files[static_cast<size_t>(ordinal)].dos_time;
		} else if (impl.format == Format::Raw) {
			if (ordinal > 0) {
				return fail(Error::Corrupt,
				            "a raw stream with a second entry");
			}
			// D13.3: disk1.img.xz holds disk1.img.
			utf8_name = PathToUtf8(name.stem());
		} else {
			const auto* name = archive_entry_pathname_utf8(ae);
			if (!name) {
				name = archive_entry_pathname(ae);
			}
			utf8_name = name ? name : "";
		}

		auto rejection = ArchiveNames::Rejection::None;
		const auto split = ArchiveNames::SplitEntryPath(utf8_name, &rejection);
		if (!split) {
			return fail(Error::EntryRejected,
			            "'" + utf8_name + "': " +
			                    ArchiveNames::RejectionText(rejection));
		}

		// A regular file spelled as the root or as a directory would
		// lose its bytes to the directory it names.
		if (split->is_dir && filetype == AE_IFREG) {
			return fail(Error::EntryRejected,
			            "'" + utf8_name + "': a file named as a directory");
		}
		if (split->components.empty()) {
			archive_read_data_skip(handle.a);
			continue;
		}
		auto entry    = Entry{};
		entry.ordinal = ordinal;
		entry.is_dir  = filetype == AE_IFDIR || split->is_dir;
		entry.size    = entry.is_dir ? 0
		                             : (archive_entry_size_is_set(ae)
		                                        ? archive_entry_size(ae)
		                                        : -1);
		entry.mtime   = archive_entry_mtime(ae);
		entry.has_crc = has_crc && !entry.is_dir;
		entry.crc32   = crc;
		if (impl.format == Format::Zip && !entry.is_dir) {
			entry.size = static_cast<int64_t>(
			        zip_records[static_cast<size_t>(ordinal)].uncompressed_size);
			entry.stored_offset = stored_offsets[static_cast<size_t>(ordinal)];
		}
		if (impl.format == Format::Raw) {
			// D13.2: a raw entry declares no size; this one bounded
			// pass counts it, and the member's time is the
			// container's (D13.3).
			std::vector<uint8_t> block(ReadBlockBytes);
			int64_t counted = 0;
			// The archive cap bounds this pass too (ada-ciru item
			// 5); the CRC of the bytes read here is the identity's
			// content field (item 6).
			const auto cap   = std::min(limits.max_member_bytes,
                                                  limits.max_total_bytes);
			auto content_crc = crc32(0L, Z_NULL, 0);
			while (true) {
				const auto got = archive_read_data(handle.a,
				                                   block.data(),
				                                   block.size());
				if (got < 0) {
					return fail(Error::Corrupt,
					            error_text_or(handle.a,
					                          "read failed"));
				}
				if (got == 0) {
					break;
				}
				counted += got;
				if (counted > cap) {
					return fail(Error::TooLarge,
					            "'" + utf8_name + "': over " +
					                    std::to_string(cap) +
					                    " bytes");
				}
				content_crc = crc32(content_crc,
				                    block.data(),
				                    static_cast<uInt>(got));
			}
			entry.size    = counted;
			entry.mtime   = impl.indexed_stat.mtime;
			entry.has_crc = true;
			entry.crc32   = static_cast<uint32_t>(content_crc);
		}
		if (entry.size < 0) {
			return fail(Error::TooLarge,
			            "'" + utf8_name + "': size not declared");
		}
		if (entry.size > limits.max_member_bytes) {
			return fail(Error::TooLarge,
			            "'" + utf8_name +
			                    "': " + std::to_string(entry.size) +
			                    " bytes");
		}

		// Intermediate directories appear on first sight, from an entry
		// or the first file below; a name goes only to a path not seen
		// before, so a duplicate never burns a numbered name.
		auto normalized = std::string();
		auto parent_dos = std::string();
		for (size_t i = 0; i + 1 < split->components.size(); ++i) {
			normalized += (i ? "/" : "") + split->components[i];
			if (const auto* existing = impl.at_path(normalized)) {
				if (!existing->is_dir) {
					return fail(Error::EntryRejected,
					            "'" + utf8_name + "': '" +
					                    normalized +
					                    "' is a file");
				}
				parent_dos = existing->dos_path;
				continue;
			}
			const auto dir_dos = names.Assign(parent_dos,
			                                  split->components[i]);
			if (!dir_dos) {
				return fail(Error::EntryRejected,
				            "'" + utf8_name + "': DOS path too long");
			}
			auto dir_entry      = Entry{};
			dir_entry.ordinal   = ImpliedOrdinal;
			dir_entry.utf8_path = normalized;
			dir_entry.dos_path  = *dir_dos;
			dir_entry.is_dir    = true;
			dir_entry.mtime     = entry.mtime;
			impl.add(dir_entry, 0);
			seen_paths.insert(normalized);
			parent_dos = *dir_dos;
		}
		normalized += (split->components.size() > 1 ? "/" : "") +
		              split->components.back();
		if (seen_paths.contains(normalized)) {
			// 7z and RAR list a directory after its files, so a dir
			// after a dir is expected; a file and a directory under
			// one path is a tree DOS cannot hold.
			const auto* existing = impl.at_path(normalized);
			if (existing && existing->is_dir != entry.is_dir) {
				return fail(Error::EntryRejected,
				            "'" + utf8_name + "': " +
				                    (entry.is_dir ? "a file"
				                                  : "a directory") +
				                    " of that name is already listed");
			}
			if (!entry.is_dir) {
				// ada-0o6v: two files under
				// one path are refused when their bytes differ;
				// formats without a CRC can only tell sizes apart.
				const auto differs = existing &&
				                     (existing->size != entry.size ||
				                      (existing->has_crc && has_crc &&
				                       existing->crc32 != crc));
				if (differs) {
					return fail(Error::EntryRejected,
					            "'" + utf8_name +
					                    "': listed twice with different content");
				}
				augra::log_warn(LogComponent,
				                "%s: duplicate entry '%s' kept once",
				                PathToUtf8(path).c_str(),
				                normalized.c_str());
			}
			archive_read_data_skip(handle.a);
			continue;
		}
		const auto dos_path = names.Assign(parent_dos,
		                                   split->components.back());
		if (!dos_path) {
			return fail(Error::EntryRejected,
			            "'" + utf8_name +
			                    "': no free 8.3 name or DOS path too long");
		}
		seen_paths.insert(normalized);
		entry.utf8_path = normalized;
		entry.dos_path  = *dos_path;
		impl.total_bytes += entry.size;
		if (impl.total_bytes > limits.max_total_bytes) {
			return fail(Error::TooLarge,
			            "declared total over " +
			                    std::to_string(limits.max_total_bytes) +
			                    " bytes");
		}
		impl.add(entry,
		         identity_time_of(impl.format, entry.mtime, stored_dos_time));
		if (archive_read_data_skip(handle.a) < ARCHIVE_OK) {
			return fail(Error::Corrupt,
			            error_text_or(handle.a, "skip failed"));
		}
	}
	if (!format_known) {
		if (zip_or_stub) {
			if (const auto verdict = directory_verdict()) {
				return fail(verdict->first, verdict->second);
			}
		}
		const auto format = format_of(handle.a);
		if (!format) {
			return fail(Error::NotAnArchive, "no entries and no format");
		}
		impl.format = *format;
		impl.access = access_of(*format);
	}
	if (impl.format == Format::Zip) {
		if (const auto verdict = directory_verdict()) {
			return fail(verdict->first, verdict->second);
		}
	}
	if (impl.format == Format::Zip &&
	    static_cast<size_t>(walked) != zip_records.size()) {
		return fail(Error::Corrupt,
		            "central directory lists " +
		                    std::to_string(zip_records.size()) +
		                    " entries, libarchive walked " +
		                    std::to_string(walked));
	}
	if (impl.format == Format::Rar &&
	    static_cast<size_t>(walked) != rar4.files.size()) {
		return fail(Error::Corrupt,
		            "RAR headers list " + std::to_string(rar4.files.size()) +
		                    " entries, libarchive walked " +
		                    std::to_string(walked));
	}

	return finish();
}

Format ArchiveReader::GetFormat() const
{
	return impl->format;
}
Access ArchiveReader::GetAccess() const
{
	return impl->access;
}
const std::vector<Entry>& ArchiveReader::Entries() const
{
	return impl->entries;
}
int64_t ArchiveReader::TotalBytes() const
{
	return impl->total_bytes;
}
int64_t ArchiveReader::ArchiveLength() const
{
	return impl->archive_length;
}
StatPair ArchiveReader::IndexedStat() const
{
	return impl->indexed_stat;
}
const std::string& ArchiveReader::IdentityHash() const
{
	return impl->identity;
}

Error ArchiveReader::Materialize(const int ordinal, std::vector<uint8_t>& out,
                                 std::string* detail)
{
	out.clear();
	const auto use = Impl::Use(impl->in_use);
	if (const auto changed = impl->check_unchanged(detail);
	    changed != Error::None) {
		return changed;
	}
	const auto* entry = impl->find(ordinal);
	if (!entry || entry->is_dir) {
		if (detail) {
			*detail = "no such member";
		}
		return Error::NotFound;
	}
	if (impl->isz) {
		const auto read = Isz::ReadAll(
		        impl->file.fd,
		        *impl->isz,
		        [&](const uint8_t* data, const size_t size) {
			        out.insert(out.end(), data, data + size);
			        return true;
		        },
		        detail);
		if (read != Error::None) {
			out.clear();
		}
		return read;
	}
	if (const auto legacy = impl->legacy.find(ordinal);
	    legacy != impl->legacy.end()) {
		const auto read = impl->read_legacy(
		        legacy->second,
		        *entry,
		        [&](const uint8_t* data, const size_t size) {
			        out.insert(out.end(), data, data + size);
			        return true;
		        },
		        detail);
		if (read != Error::None) {
			out.clear();
		}
		return read;
	}
	auto handle = Handle{};
	if (const auto seek = impl->seek_to(handle, ordinal, detail);
	    seek != Error::None) {
		return seek;
	}
	return impl->read_member(
	        handle.a,
	        *entry,
	        [&](const uint8_t* data, const size_t size) {
		        out.insert(out.end(), data, data + size);
		        return true;
	        },
	        detail);
}

Error ArchiveReader::MaterializeToFile(const int ordinal,
                                       const std::filesystem::path& target,
                                       std::string* detail)
{
	const auto use = Impl::Use(impl->in_use);
	if (const auto changed = impl->check_unchanged(detail);
	    changed != Error::None) {
		return changed;
	}
	const auto* entry = impl->find(ordinal);
	if (!entry || entry->is_dir) {
		if (detail) {
			*detail = "no such member";
		}
		return Error::NotFound;
	}
	if (impl->isz) {
		return impl->write_member(
		        [&](const Impl::ByteSink& sink) {
			        return Isz::ReadAll(impl->file.fd, *impl->isz, sink, detail);
		        },
		        *entry,
		        target,
		        detail);
	}
	if (const auto legacy = impl->legacy.find(ordinal);
	    legacy != impl->legacy.end()) {
		return impl->write_member(
		        [&](const Impl::ByteSink& sink) {
			        return impl->read_legacy(legacy->second,
			                                 *entry,
			                                 sink,
			                                 detail);
		        },
		        *entry,
		        target,
		        detail);
	}
	auto handle = Handle{};
	if (const auto seek = impl->seek_to(handle, ordinal, detail);
	    seek != Error::None) {
		return seek;
	}
	return impl->write_member(
	        [&](const Impl::ByteSink& sink) {
		        return impl->read_member(handle.a, *entry, sink, detail);
	        },
	        *entry,
	        target,
	        detail);
}

Error ArchiveReader::MaterializeAll(const std::filesystem::path& directory,
                                    std::string* detail)
{
	const auto use = Impl::Use(impl->in_use);
	if (const auto changed = impl->check_unchanged(detail);
	    changed != Error::None) {
		return changed;
	}
	std::error_code ec = {};
	std::filesystem::create_directories(directory, ec);
	if (ec) {
		if (detail) {
			*detail = "cannot create " + PathToUtf8(directory);
		}
		return Error::OpenFailed;
	}
	if (impl->isz) {
		const auto* entry = impl->find(0);
		if (!entry) {
			return Error::NotFound;
		}
		return impl->write_member(
		        [&](const Impl::ByteSink& sink) {
			        return Isz::ReadAll(impl->file.fd, *impl->isz, sink, detail);
		        },
		        *entry,
		        directory / "0",
		        detail);
	}
	auto handle = Handle{};
	if (open_handle(impl->file.fd,
	                impl->kind,
	                impl->format == Format::Zip,
	                impl->raw_allowed,
	                impl->window,
	                handle,
	                detail) != HandleStatus::Ok) {
		return Error::OpenFailed;
	}
	struct archive_entry* ae = nullptr;
	for (auto ordinal = 0;; ++ordinal) {
		const auto r = archive_read_next_header(handle.a, &ae);
		if (r == ARCHIVE_EOF) {
			return Error::None;
		}
		if (r < ARCHIVE_OK && r != ARCHIVE_WARN) {
			if (detail) {
				*detail = error_text_or(handle.a, "header failed");
			}
			return Error::Corrupt;
		}
		const auto* entry = impl->find(ordinal);
		if (!entry || entry->is_dir) {
			archive_read_data_skip(handle.a);
			continue;
		}
		const auto written = impl->write_member(
		        [&](const Impl::ByteSink& sink) {
			        return impl->read_member(handle.a, *entry, sink, detail);
		        },
		        *entry,
		        directory / std::to_string(ordinal),
		        detail);
		if (written != Error::None) {
			return written;
		}
	}
}

} // namespace ArchiveMount
