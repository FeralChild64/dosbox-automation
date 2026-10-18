// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//
#include "dos/drives.h"

#include "augra/log.h"
#include "dos/dos.h"
#include "dos/dos_names.h"
#include "dos/dos_system.h"
#include "dos/drive_search_slots.h"
#include "dos/programs/mount_policy.h"
#include "misc/cross.h"
#include "misc/support.h"
#include "utils/checks.h"
#include "utils/string_utils.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <list>
#include <memory>
#include <unordered_map>

CHECK_NARROWING();

using namespace ArchiveMount;

namespace {

constexpr auto LogComponent = "archive_drive";

// Bytes of one member in memory, shared by the drive's retained map and
// every open file on it.
using MemberBytes = std::shared_ptr<const std::vector<uint8_t>>;

FILE* OpenCacheFile(const std::filesystem::path& path)
{
#if defined(WIN32)
	return _wfopen(path.wstring().c_str(), L"rbN");
#else
	// A cache file is ours; "e" keeps it out of child processes.
	return std::fopen(path.c_str(), "rbe");
#endif
}

bool SeekFile(FILE* f, const int64_t position)
{
#if defined(WIN32)
	return _fseeki64(f, position, SEEK_SET) == 0;
#else
	return fseeko(f, static_cast<off_t>(position), SEEK_SET) == 0;
#endif
}

class ArchiveFile final : public DOS_File {
public:
	ArchiveFile(const char* dos_name, MemberBytes bytes, FILE* file,
	            const int64_t size, const uint16_t dos_date,
	            const uint16_t dos_time)
	        : bytes(std::move(bytes)),
	          file(file),
	          size(size)
	{
		SetName(dos_name);
		date = dos_date;
		time = dos_time;
		attr = FatAttributeFlags::ReadOnly;
	}
	~ArchiveFile() override
	{
		if (file) {
			std::fclose(file);
		}
	}
	ArchiveFile(const ArchiveFile&)            = delete;
	ArchiveFile& operator=(const ArchiveFile&) = delete;

	bool Read(uint8_t* data, uint16_t* count) override
	{
		if (position >= size) {
			*count = 0;
			return true;
		}
		const auto wanted = std::min<int64_t>(*count, size - position);
		if (bytes) {
			std::copy_n(bytes->data() + position, wanted, data);
		} else {
			if (!SeekFile(file, position)) {
				*count = 0;
				return false;
			}
			const auto got = std::fread(data,
			                            1,
			                            static_cast<size_t>(wanted),
			                            file);
			if (got != static_cast<size_t>(wanted)) {
				augra::log_warn(LogComponent,
				                "%s: short read from the cache file",
				                GetName());
				*count = static_cast<uint16_t>(got);
				position += static_cast<int64_t>(got);
				return got > 0;
			}
		}
		*count = static_cast<uint16_t>(wanted);
		position += wanted;
		return true;
	}
	bool Write(uint8_t* /*data*/, uint16_t* /*count*/) override
	{
		DOS_SetError(DOSERR_ACCESS_DENIED);
		return false;
	}
	// DOS may seek past the end (a read there returns nothing); a
	// position before the start is refused. CUR and END offsets are
	// signed, as DOS passes them.
	bool Seek(uint32_t* pos, const uint32_t type) override
	{
		int64_t target = 0;
		switch (type) {
		case DOS_SEEK_SET: target = *pos; break;
		case DOS_SEEK_CUR:
			target = position + static_cast<int32_t>(*pos);
			break;
		case DOS_SEEK_END:
			target = size + static_cast<int32_t>(*pos);
			break;
		default: return false;
		}
		if (target < 0 || target > UINT32_MAX) {
			return false;
		}
		position = target;
		*pos     = static_cast<uint32_t>(target);
		return true;
	}
	void Close() override {}
	uint16_t GetInformation() override
	{
		return 0x40; // as the ISO drive reports its files
	}
	bool IsOnReadOnlyMedium() const override
	{
		return true;
	}

private:
	MemberBytes bytes = {};
	FILE* file        = nullptr;
	int64_t size      = 0;
	int64_t position  = 0;
};

struct SearchCursor {
	int node         = -1;
	size_t next      = 0;
	bool is_root     = true;
	bool dot_done    = false;
	bool dotdot_done = false;
};

} // namespace

struct ArchiveDrive::Impl {
	std::shared_ptr<HeldResources> held        = {}; // dies after source
	std::unique_ptr<ArchiveSource> source      = {};
	std::unique_ptr<ArchiveCache::Lease> lease = {}; // dies before source
	std::optional<ArchiveTree> tree            = {};
	Options options                            = {};
	std::string label                          = {};
	Allocation allocation                      = {};

	DriveSearchSlots<SearchCursor, MAX_OPENDIRS> searches = {};

	// Least recently opened at the front.
	struct Retained {
		MemberBytes bytes = {};
		std::list<int>::iterator order_it;
	};
	std::list<int> order                       = {};
	std::unordered_map<int, Retained> retained = {};
	int64_t retained_bytes                     = 0;

	ArchiveMountState state = ArchiveMountState::Ready;
	int64_t last_attempt    = 0; // unix seconds, 0 = never

	int64_t Now() const
	{
		if (options.reconnect && options.reconnect->now) {
			return options.reconnect->now();
		}
		return std::chrono::duration_cast<std::chrono::seconds>(
		               std::chrono::system_clock::now().time_since_epoch())
		        .count();
	}

	// The state a materialization error moves the drive to, if any.
	void Observe(const Error error)
	{
		switch (error) {
		case Error::Changed: state = ArchiveMountState::Changed; break;
		case Error::HelperFailed:
		case Error::Timeout:
			state = ArchiveMountState::Disconnected;
			break;
		case Error::OpenFailed: state = ArchiveMountState::Lost; break;
		default: return;
		}
		last_attempt = Now();
		augra::log_warn(LogComponent,
		                "%s: %s, the drive is %s",
		                label.c_str(),
		                ErrorText(error),
		                ArchiveDrive::StateName(state));
	}

	bool Reconnect(std::string* detail)
	{
		last_attempt = Now();
		if (!options.reconnect || !options.reconnect->reopen) {
			if (detail) {
				*detail = "no way to reopen the archive";
			}
			return false;
		}
		auto why   = std::string();
		auto fresh = options.reconnect->reopen(&why);
		if (!fresh.source) {
			state = ArchiveMountState::Lost;
			if (detail) {
				*detail = why;
			}
			return false;
		}
		if (fresh.source->IdentityHash() != source->IdentityHash()) {
			state = ArchiveMountState::Lost;
			if (detail) {
				*detail = "a different archive is at the path now";
			}
			return false;
		}
		if (lease && !lease->Rebind(*fresh.source)) {
			state = ArchiveMountState::Lost;
			if (detail) {
				*detail = "the cache lease could not follow";
			}
			return false;
		}
		source = std::move(fresh.source);
		held   = std::move(fresh.held); // after the old source is gone
		state  = ArchiveMountState::Ready;
		augra::log_info(LogComponent, "%s: reconnected", label.c_str());
		return true;
	}

	// At most one attempt per access once the backoff has passed.
	bool EnsureReady()
	{
		if (state == ArchiveMountState::Ready) {
			return true;
		}
		if (!options.reconnect) {
			return false;
		}
		if (Now() - last_attempt < options.reconnect->backoff_seconds) {
			return false;
		}
		return Reconnect(nullptr);
	}

	const TreeNode* Find(const char* dos_path) const
	{
		// DOS_MakeName keeps an empty extension ("SKIEGA."); the tree
		// is keyed by the plain 8.3 names (G8/G9).
		return tree->Find(
		        DosNames::WithoutEmptyExtensions(dos_path ? dos_path : ""));
	}

	bool Matches(const TreeNode& node, const char* pattern,
	             const FatAttributeFlags search_attr) const
	{
		// As the ISO drive: a directory, hidden or system entry is
		// listed only when the search asks for that class.
		FatAttributeFlags found = {};
		found.directory         = node.is_dir;
		FatAttributeFlags mask  = {};
		mask.directory          = true;
		mask.hidden             = true;
		mask.system             = true;
		if (~(search_attr._data) & found._data & mask._data) {
			return false;
		}
		return wild_file_cmp(node.name.c_str(), pattern);
	}

	static Listing Describe(const TreeNode& node)
	{
		Listing out        = {};
		out.name           = node.name;
		out.size           = static_cast<uint32_t>(node.size);
		out.date           = node.dos_date;
		out.time           = node.dos_time;
		out.attr           = FatAttributeFlags::ReadOnly;
		out.attr.directory = node.is_dir;
		return out;
	}

	// The next entry of the cursor's directory that the pattern and the
	// attribute mask accept; the dot entries first in a subdirectory
	// when directories are searched.
	bool NextMatch(SearchCursor& cursor, const char* pattern,
	               const FatAttributeFlags search_attr, Listing& out) const
	{
		const auto& dir = tree->Node(cursor.node);
		if (!cursor.is_root && search_attr.directory) {
			for (const auto* dot : {".", ".."}) {
				auto& done = dot[1] == '\0' ? cursor.dot_done
				                            : cursor.dotdot_done;
				if (done) {
					continue;
				}
				done = true;
				if (wild_file_cmp(dot, pattern)) {
					out      = {};
					out.name = dot;
					out.date = dir.dos_date;
					out.time = dir.dos_time;
					out.attr = FatAttributeFlags::Directory;
					return true;
				}
			}
		}
		while (cursor.next < dir.children.size()) {
			const auto& child = tree->Node(dir.children[cursor.next]);
			++cursor.next;
			if (Matches(child, pattern, search_attr)) {
				out = Describe(child);
				return true;
			}
		}
		return false;
	}

	// Label handling shared by FindFirst and List; true when a label
	// result was produced (or the search is for the label alone).
	bool LabelResult(const bool is_root, const char* pattern,
	                 const FatAttributeFlags search_attr,
	                 const bool fcb_findfirst, Listing& out, bool& produced) const
	{
		produced = false;
		if (search_attr == FatAttributeFlags::Volume) {
			produced = true;
		} else if (search_attr.volume && is_root && !fcb_findfirst &&
		           wild_file_cmp(label.c_str(), pattern)) {
			produced = true;
		}
		if (produced) {
			out      = {};
			out.name = label;
			out.attr = FatAttributeFlags::Volume;
		}
		return search_attr == FatAttributeFlags::Volume;
	}

	void Touch(const int ordinal)
	{
		auto found = retained.find(ordinal);
		if (found == retained.end()) {
			return;
		}
		order.erase(found->second.order_it);
		order.push_back(ordinal);
		found->second.order_it = std::prev(order.end());
	}

	// Drops the least recently opened members nobody holds until the
	// budget holds; a held member stays whatever the budget says.
	void Evict()
	{
		for (auto it = order.begin();
		     it != order.end() &&
		     retained_bytes > options.memory_retain_bytes;) {
			auto& entry = retained.at(*it);
			if (entry.bytes.use_count() > 1) {
				++it;
				continue;
			}
			retained_bytes -= static_cast<int64_t>(entry.bytes->size());
			retained.erase(*it);
			it = order.erase(it);
		}
	}

	MemberBytes BytesFor(const TreeNode& node)
	{
		if (const auto found = retained.find(node.ordinal);
		    found != retained.end()) {
			Touch(node.ordinal);
			return found->second.bytes;
		}
		auto out    = std::vector<uint8_t>();
		auto detail = std::string();
		const auto error = source->Materialize(node.ordinal, out, &detail);
		if (error != Error::None) {
			Observe(error);
			augra::log_warn(LogComponent,
			                "%s: cannot read member %d: %s: %s",
			                label.c_str(),
			                node.ordinal,
			                ErrorText(error),
			                detail.c_str());
			return {};
		}
		auto bytes = std::make_shared<const std::vector<uint8_t>>(
		        std::move(out));
		order.push_back(node.ordinal);
		retained[node.ordinal] = Retained{bytes, std::prev(order.end())};
		retained_bytes += static_cast<int64_t>(bytes->size());
		Evict();
		return bytes;
	}

	FILE* FileFor(const TreeNode& node)
	{
		if (!lease) {
			augra::log_warn(LogComponent,
			                "%s: member %d is over the memory limit and there is no cache",
			                label.c_str(),
			                node.ordinal);
			return nullptr;
		}
		std::filesystem::path path = {};
		auto detail                = std::string();
		const auto status = lease->MemberFile(node.ordinal, path, &detail);
		if (status != CacheStatus::Ok) {
			if (status == CacheStatus::SourceFailed) {
				Observe(lease->LastSourceError());
			}
			augra::log_warn(LogComponent,
			                "%s: member %d: %s: %s",
			                label.c_str(),
			                node.ordinal,
			                CacheStatusText(status),
			                detail.c_str());
			return nullptr;
		}
		FILE* f = OpenCacheFile(path);
		if (!f) {
			augra::log_warn(LogComponent,
			                "%s: cannot open %s",
			                label.c_str(),
			                path.string().c_str());
		}
		return f;
	}
};

ArchiveDrive::ArchiveDrive() : impl(std::make_unique<Impl>()) {}

ArchiveDrive::~ArchiveDrive()
{
	// The lease and the source go with impl, in declaration order; the
	// scratch directory is ours alone and goes last.
	const auto scratch = impl->options.owned_scratch_dir;
	impl.reset();
	if (!scratch.empty()) {
		std::error_code ec = {};
		std::filesystem::remove_all(scratch, ec);
	}
}

std::string ArchiveDrive::FoldLabel(const std::string_view label)
{
	auto out = std::string();
	for (const auto c : label) {
		if (out.size() == 11) {
			break;
		}
		const auto byte = static_cast<unsigned char>(c);
		if ((byte >= 'A' && byte <= 'Z') ||
		    (byte >= '0' && byte <= '9') || byte == '_' || byte == '-') {
			out.push_back(static_cast<char>(byte));
		} else if (byte >= 'a' && byte <= 'z') {
			out.push_back(static_cast<char>(byte - 'a' + 'A'));
		} else {
			out.push_back('_');
		}
	}
	return out.empty() ? "ARCHIVE" : out;
}

ArchiveDrive::Allocation ArchiveDrive::ComputeAllocation(const int64_t total_bytes)
{
	Allocation out   = {};
	int64_t clusters = 0;
	for (int sectors = 1; sectors <= 128; sectors *= 2) {
		const int64_t cluster_bytes = 512ll * sectors;
		clusters = (total_bytes + cluster_bytes - 1) / cluster_bytes;
		out.sectors_cluster = static_cast<uint8_t>(sectors);
		if (clusters <= 65535) {
			break;
		}
	}
	out.total_clusters = static_cast<uint16_t>(std::min<int64_t>(clusters, 65535));
	return out;
}

ArchiveDrive::CreateResult ArchiveDrive::Create(
        std::unique_ptr<ArchiveSource> source,
        std::unique_ptr<ArchiveCache::Lease> lease,
        const std::string_view root_dos_path, const std::string_view label,
        const std::string_view info_text, const Options& options)
{
	CreateResult result = {};
	if (!source) {
		result.error   = "no archive source";
		result.failure = CreateResult::Failure::TreeInvalid;
		return result;
	}
	auto error = std::string();
	auto tree = ArchiveTree::Build(source->Entries(), root_dos_path, &error);
	if (!tree) {
		result.error   = error;
		result.failure = error.starts_with("root path")
		                       ? CreateResult::Failure::RootNotFound
		                       : CreateResult::Failure::TreeInvalid;
		return result;
	}
	if (source->GetAccess() == Access::Sequential) {
		if (!lease) {
			result.error = "a sequential archive needs the archive cache";
			result.failure = CreateResult::Failure::CacheNeeded;
			return result;
		}
		auto detail = std::string();
		if (const auto status = lease->EnsureAll(&detail);
		    status != CacheStatus::Ok) {
			result.error = std::string(CacheStatusText(status)) +
			               ": " + detail;
			result.failure = CreateResult::Failure::CacheFailed;
			return result;
		}
	}
	if (MountPolicy::drive_construction_hook) {
		MountPolicy::drive_construction_hook(std::string(info_text).c_str());
	}
	auto drive          = std::shared_ptr<ArchiveDrive>(new ArchiveDrive());
	drive->impl->source = std::move(source);
	drive->impl->lease  = std::move(lease);
	drive->impl->tree   = std::move(*tree);
	drive->impl->options = options;
	drive->impl->held    = std::move(drive->impl->options.held);
	drive->impl->label   = FoldLabel(label);
	drive->impl->allocation = ComputeAllocation(drive->impl->tree->TotalBytes());
	drive->type = DosDriveType::Archive;
	safe_strcpy(drive->info, std::string(info_text).c_str());
	result.drive = std::move(drive);
	return result;
}

std::unique_ptr<DOS_File> ArchiveDrive::FileOpen(const char* name, const uint8_t flags)
{
	if (!impl->EnsureReady()) {
		DOS_SetError(DOSERR_DRIVE_NOT_READY);
		return nullptr;
	}
	if ((flags & 0x0f) == OPEN_WRITE) {
		DOS_SetError(DOSERR_ACCESS_DENIED);
		return nullptr;
	}
	const auto* node = impl->Find(name);
	if (!node || node->is_dir || node->entry_index < 0) {
		DOS_SetError(DOSERR_FILE_NOT_FOUND);
		return nullptr;
	}
	MemberBytes bytes = {};
	FILE* file        = nullptr;
	if (node->size <= impl->options.memory_limit_bytes) {
		bytes = impl->BytesFor(*node);
		if (!bytes) {
			// The access that saw the archive change reports it as
			// DOS does.
			DOS_SetError(impl->state == ArchiveMountState::Ready
			                     ? DOSERR_ACCESS_DENIED
			                     : DOSERR_DRIVE_NOT_READY);
			return nullptr;
		}
	} else {
		file = impl->FileFor(*node);
		if (!file) {
			// The access that saw the archive change reports it as
			// DOS does.
			DOS_SetError(impl->state == ArchiveMountState::Ready
			                     ? DOSERR_ACCESS_DENIED
			                     : DOSERR_DRIVE_NOT_READY);
			return nullptr;
		}
	}
	auto opened   = std::make_unique<ArchiveFile>(name,
                                                    std::move(bytes),
                                                    file,
                                                    node->size,
                                                    node->dos_date,
                                                    node->dos_time);
	opened->flags = flags;
	return opened;
}

std::unique_ptr<DOS_File> ArchiveDrive::FileCreate(const char* /*name*/,
                                                   FatAttributeFlags /*attributes*/)
{
	DOS_SetError(DOSERR_ACCESS_DENIED);
	return nullptr;
}

bool ArchiveDrive::FileUnlink(const char* /*name*/)
{
	DOS_SetError(DOSERR_ACCESS_DENIED);
	return false;
}

bool ArchiveDrive::RemoveDir(const char* /*dir*/)
{
	DOS_SetError(DOSERR_ACCESS_DENIED);
	return false;
}

bool ArchiveDrive::MakeDir(const char* /*dir*/)
{
	DOS_SetError(DOSERR_ACCESS_DENIED);
	return false;
}

bool ArchiveDrive::TestDir(const char* dir)
{
	if (!impl->EnsureReady()) {
		DOS_SetError(DOSERR_DRIVE_NOT_READY);
		return false;
	}
	const auto* node = impl->Find(dir);
	return node && node->is_dir;
}

bool ArchiveDrive::FindFirst(const char* dir, DOS_DTA& dta, const bool fcb_findfirst)
{
	if (!impl->EnsureReady()) {
		DOS_SetError(DOSERR_DRIVE_NOT_READY);
		return false;
	}
	const auto* node = impl->Find(dir);
	if (!node || !node->is_dir) {
		DOS_SetError(DOSERR_PATH_NOT_FOUND);
		return false;
	}
	const auto search = impl->searches.Acquire();
	auto& cursor      = search.cursor;
	cursor.node       = static_cast<int>(node - &impl->tree->Node(0));
	cursor.is_root    = node == &impl->tree->Root();
	dta.SetDirID(search.id);

	FatAttributeFlags search_attr = {};
	char pattern[DOS_NAMELENGTH_ASCII];
	dta.GetSearchParams(search_attr, pattern);
	Listing label    = {};
	bool produced    = false;
	const auto alone = impl->LabelResult(
	        cursor.is_root, pattern, search_attr, fcb_findfirst, label, produced);
	if (produced) {
		dta.SetResult(label.name.c_str(), 0, 0, 0, FatAttributeFlags::Volume);
		return true;
	}
	if (alone) {
		DOS_SetError(DOSERR_NO_MORE_FILES);
		return false;
	}
	return FindNext(dta);
}

bool ArchiveDrive::FindNext(DOS_DTA& dta)
{
	if (!impl->EnsureReady()) {
		DOS_SetError(DOSERR_DRIVE_NOT_READY);
		return false;
	}
	const auto id = dta.GetDirID();
	auto* cursor  = impl->searches.Find(id);
	if (!cursor) {
		DOS_SetError(DOSERR_NO_MORE_FILES);
		return false;
	}
	FatAttributeFlags search_attr = {};
	char pattern[DOS_NAMELENGTH_ASCII];
	dta.GetSearchParams(search_attr, pattern);
	Listing found = {};
	if (impl->NextMatch(*cursor, pattern, search_attr, found)) {
		dta.SetResult(found.name.c_str(),
		              found.size,
		              found.date,
		              found.time,
		              found.attr);
		return true;
	}
	impl->searches.Release(id);
	DOS_SetError(DOSERR_NO_MORE_FILES);
	return false;
}

bool ArchiveDrive::List(const char* dir, const char* pattern,
                        const FatAttributeFlags search_attr,
                        const bool fcb_findfirst, std::vector<Listing>& out)
{
	if (!impl->EnsureReady()) {
		DOS_SetError(DOSERR_DRIVE_NOT_READY);
		return false;
	}
	const auto* node = impl->Find(dir);
	if (!node || !node->is_dir) {
		DOS_SetError(DOSERR_PATH_NOT_FOUND);
		return false;
	}
	SearchCursor cursor = {};
	cursor.node         = static_cast<int>(node - &impl->tree->Node(0));
	cursor.is_root      = node == &impl->tree->Root();
	Listing label       = {};
	bool produced       = false;
	const auto alone    = impl->LabelResult(
                cursor.is_root, pattern, search_attr, fcb_findfirst, label, produced);
	if (produced) {
		out.push_back(label);
	}
	if (alone) {
		return true;
	}
	Listing found = {};
	while (impl->NextMatch(cursor, pattern, search_attr, found)) {
		out.push_back(found);
	}
	return true;
}

bool ArchiveDrive::GetFileAttr(const char* name, FatAttributeFlags* attr)
{
	if (!impl->EnsureReady()) {
		DOS_SetError(DOSERR_DRIVE_NOT_READY);
		return false;
	}
	*attr            = {};
	const auto* node = impl->Find(name);
	if (!node) {
		return false;
	}
	attr->read_only = true;
	attr->directory = node->is_dir;
	return true;
}

bool ArchiveDrive::SetFileAttr(const char* /*name*/, const FatAttributeFlags /*attr*/)
{
	DOS_SetError(DOSERR_ACCESS_DENIED);
	return false;
}

bool ArchiveDrive::Rename(const char* /*oldname*/, const char* /*newname*/)
{
	DOS_SetError(DOSERR_ACCESS_DENIED);
	return false;
}

bool ArchiveDrive::AllocationInfo(uint16_t* bytes_sector, uint8_t* sectors_cluster,
                                  uint16_t* total_clusters, uint16_t* free_clusters)
{
	*bytes_sector    = impl->allocation.bytes_sector;
	*sectors_cluster = impl->allocation.sectors_cluster;
	*total_clusters  = impl->allocation.total_clusters;
	*free_clusters   = 0;
	return true;
}

bool ArchiveDrive::FileExists(const char* name)
{
	if (!impl->EnsureReady()) {
		DOS_SetError(DOSERR_DRIVE_NOT_READY);
		return false;
	}
	const auto* node = impl->Find(name);
	return node && !node->is_dir && node->entry_index >= 0;
}

uint8_t ArchiveDrive::GetMediaByte()
{
	return 0xF8;
}

bool ArchiveDrive::IsRemote()
{
	return false;
}

bool ArchiveDrive::IsRemovable()
{
	return false;
}

Bits ArchiveDrive::UnMount()
{
	return DriveHasOpenFiles(this) ? UnmountRefusedOpenFiles : 0;
}

const char* ArchiveDrive::GetLabel()
{
	return impl->label.c_str();
}

bool ArchiveDrive::IsReady()
{
	return impl->EnsureReady();
}

ArchiveMountState ArchiveDrive::State() const
{
	return impl->state;
}

const char* ArchiveDrive::StateName(const ArchiveMountState state)
{
	switch (state) {
	case ArchiveMountState::Ready: return "ready";
	case ArchiveMountState::Changed: return "changed";
	case ArchiveMountState::Lost: return "lost";
	case ArchiveMountState::Disconnected: return "offline";
	}
	return "unknown";
}

bool ArchiveDrive::Reconnect(std::string* detail)
{
	if (impl->state == ArchiveMountState::Ready) {
		return true;
	}
	return impl->Reconnect(detail);
}

const ArchiveSource& ArchiveDrive::Source() const
{
	return *impl->source;
}

const ArchiveTree& ArchiveDrive::Tree() const
{
	return *impl->tree;
}

int64_t ArchiveDrive::RetainedMemoryBytes() const
{
	return impl->retained_bytes;
}
