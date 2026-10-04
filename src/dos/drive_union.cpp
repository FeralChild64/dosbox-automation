// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//
#include "dos/drives.h"

#include "augra/log.h"
#include "dos/dos.h"
#include "dos/dos_system.h"
#include "dos/drive_search_slots.h"
#include "hardware/pic.h"
#include "misc/cross.h"
#include "misc/support.h"
#include "utils/checks.h"
#include "utils/string_utils.h"

#include <algorithm>
#include <cstring>
#include <unordered_map>
#include <vector>

CHECK_NARROWING();

using ArchiveMount::SidecarRecord;

namespace {

constexpr auto LogComponent   = "union";
constexpr double FlushDelayMs = 2000.0;

// A lower-only file: reads from the lower until the file has a sidecar
// copy, made by this handle's first write or by another handle's.
class UnionFile final : public DOS_File {
public:
	UnionFile(std::shared_ptr<UnionDrive> owner, const char* dos_name,
	          std::unique_ptr<DOS_File> lower_file, const uint8_t open_flags)
	        : owner(std::move(owner)),
	          lower(std::move(lower_file)),
	          open_flags(open_flags),
	          seen_generation(this->owner->SidecarGeneration())
	{
		SetName(dos_name);
		time           = lower->time;
		date           = lower->date;
		attr           = lower->attr;
		attr.read_only = false; // writable by copy-up
	}
	bool Read(uint8_t* data, uint16_t* size) override
	{
		return Active().Read(data, size);
	}
	bool Write(uint8_t* data, uint16_t* size) override
	{
		FollowSidecar();
		if (!upper && !SwitchToUpper()) {
			DOS_SetError(DOSERR_ACCESS_DENIED);
			return false;
		}
		return upper->Write(data, size);
	}
	bool Seek(uint32_t* pos, const uint32_t type) override
	{
		return Active().Seek(pos, type);
	}
	void Close() override
	{
		if (upper) {
			upper->Close();
		}
		lower->Close();
	}
	uint16_t GetInformation() override
	{
		return Active().GetInformation();
	}
	bool IsOnReadOnlyMedium() const override
	{
		return false;
	}

private:
	DOS_File& Active()
	{
		FollowSidecar();
		return upper ? *upper : *lower;
	}
	// Another handle made the sidecar copy: continue there at our position.
	void FollowSidecar()
	{
		if (upper || owner->SidecarGeneration() == seen_generation) {
			return;
		}
		seen_generation = owner->SidecarGeneration();
		if (!owner->InSidecar(GetName())) {
			return;
		}
		uint32_t position = 0;
		if (!lower->Seek(&position, DOS_SEEK_CUR)) {
			return;
		}
		auto opened   = owner->FileOpen(GetName(), open_flags);
		uint32_t seek = position;
		if (opened && opened->Seek(&seek, DOS_SEEK_SET)) {
			upper = std::move(opened);
		}
	}
	// The copy-up, then the upper file at the lower's position.
	bool SwitchToUpper()
	{
		uint32_t position = 0;
		if (!lower->Seek(&position, DOS_SEEK_CUR)) {
			return false;
		}
		if (!owner->CopyUp(GetName(), GetName())) {
			return false;
		}
		auto opened = owner->FileOpen(GetName(), open_flags);
		if (!opened) {
			return false;
		}
		uint32_t seek = position;
		if (!opened->Seek(&seek, DOS_SEEK_SET)) {
			return false;
		}
		upper = std::move(opened);
		return true;
	}

	std::shared_ptr<UnionDrive> owner = {};
	std::unique_ptr<DOS_File> lower   = {};
	std::unique_ptr<DOS_File> upper   = {};
	uint8_t open_flags                = 0;
	uint64_t seen_generation          = 0;
};

// Live unions by id, for the delayed write's handler and quit_func.
std::unordered_map<uint32_t, UnionDrive*>& Registry()
{
	// Never destroyed: FlushAll runs from quit_func after static
	// destruction has begun, and drive_infos outlives this unit.
	static auto* registry = new std::unordered_map<uint32_t, UnionDrive*>();
	return *registry;
}
uint32_t next_union_id = 1;

void DelayedFlush(const uint32_t id)
{
	const auto found = Registry().find(id);
	if (found == Registry().end()) {
		return;
	}
	auto error = std::string();
	if (!found->second->Flush(&error)) {
		augra::log_warn(LogComponent,
		                "delayed record write failed: %s",
		                error.c_str());
	}
}

std::string JoinDos(const std::string& dir, const std::string& name)
{
	return dir.empty() ? name : dir + "\\" + name;
}

struct SearchCursor {
	std::vector<DOS_DTA::Result> results = {};
	size_t next                          = 0;
};

} // namespace

struct UnionDrive::Impl {
	std::shared_ptr<ArchiveDrive> lower                   = {};
	std::shared_ptr<localDrive> upper                     = {};
	std::filesystem::path sidecar_dir                     = {};
	SidecarRecord record                                  = {};
	bool dirty                                            = false;
	bool armed                                            = false;
	uint32_t id                                           = 0;
	char letter                                           = 'C';
	uint64_t sidecar_generation                           = 0;
	DriveSearchSlots<SearchCursor, MAX_OPENDIRS> searches = {};

	// Whether either layer has anything in dir, listed without a guest
	// DTA: the caller's may be mid-search (DELTREE) and the temp DTA holds
	// FCB find's state.
	bool HasEntries(const char* dir)
	{
		if (upper->TestDir(dir)) {
			std::error_code ec = {};
			const auto host    = upper->MapDosToHostFilename(dir);
			if (!std::filesystem::is_empty(host, ec) || ec) {
				return true;
			}
		}
		if (record.IsHidden(dir) || !lower->TestDir(dir)) {
			return false;
		}
		FatAttributeFlags everything               = {};
		everything.directory                       = true;
		everything.hidden                          = true;
		everything.system                          = true;
		std::vector<ArchiveDrive::Listing> entries = {};
		if (!lower->List(dir, "????????.???", everything, false, entries)) {
			return true;
		}
		return std::any_of(entries.begin(), entries.end(), [&](const auto& e) {
			const bool dot = e.attr.directory &&
			                 (e.name == "." || e.name == "..");
			return !dot && !record.IsHidden(JoinDos(dir, e.name));
		});
	}

	// A full record refuses the operation before anything has changed.
	bool RecordHasRoom(const bool room)
	{
		if (!room) {
			augra::log_warn(LogComponent,
			                "%c: the sidecar record is full, change refused",
			                letter);
			DOS_SetError(DOSERR_ACCESS_DENIED);
		}
		return room;
	}

	// D16: an archive that is not Ready makes the whole drive not ready.
	bool LowerReady()
	{
		if (!lower->IsReady()) {
			DOS_SetError(DOSERR_DRIVE_NOT_READY);
			return false;
		}
		return true;
	}
	bool UpperHas(const char* name)
	{
		FatAttributeFlags attr = {};
		return upper->GetFileAttr(name, &attr);
	}
	bool LowerHas(const char* name)
	{
		if (record.IsHidden(name)) {
			return false;
		}
		FatAttributeFlags attr = {};
		return lower->GetFileAttr(name, &attr);
	}
	// 3.2: nothing in the sidecar that is a link is ever written to or
	// through; the parent chain is checked with lstat.
	bool SidecarPathIsPlain(const char* dos_name)
	{
		// localDrive matches names without case; check the host spelling
		// its cache resolves, component by component, without following.
		const auto base = std::filesystem::path(upper->GetBasedir());
		const auto host = std::filesystem::path(
		        upper->MapDosToHostFilename(dos_name));
		std::error_code ec = {};
		const auto rel     = host.lexically_relative(base);
		auto probe         = base;
		for (const auto& component : rel) {
			if (component.empty() || component == ".") {
				continue;
			}
			probe /= component;
			const auto status = std::filesystem::symlink_status(probe, ec);
			bool link = !ec && std::filesystem::is_symlink(status);
#if defined(WIN32)
			// MSVC reports a junction as its own type, not as a
			// symlink.
			link = link ||
			       (!ec && status.type() ==
			                       std::filesystem::file_type::junction);
#endif
			if (link) {
				augra::log_warn(LogComponent,
				                "sidecar: '%s' is a link, refused",
				                probe.string().c_str());
				return false;
			}
		}
		return true;
	}
	static std::string ParentOf(const char* dos_name)
	{
		const std::string path = dos_name;
		const auto slash       = path.rfind('\\');
		return slash == std::string::npos ? std::string()
		                                  : path.substr(0, slash);
	}
	// DOS creates nothing below a missing directory, in either layer.
	bool ParentVisible(const char* dos_name)
	{
		const auto parent = ParentOf(dos_name);
		return parent.empty() || upper->TestDir(parent.c_str()) ||
		       (!record.IsHidden(parent.c_str()) &&
		        lower->TestDir(parent.c_str()));
	}
	// The sidecar directories dos_name needs, mirrored from visible lower
	// ones; the ones it had to create go into made, shallowest first.
	bool EnsureParents(const char* dos_name, std::vector<std::string>& made)
	{
		const auto parent = ParentOf(dos_name);
		if (parent.empty() || upper->TestDir(parent.c_str())) {
			return true;
		}
		if (record.IsHidden(parent.c_str()) ||
		    !lower->TestDir(parent.c_str())) {
			return false;
		}
		if (!EnsureParents(parent.c_str(), made) ||
		    !upper->MakeDir(parent.c_str())) {
			return false;
		}
		made.push_back(parent);
		return true;
	}
	// Undoes EnsureParents for a step that failed after it; a directory
	// that is no longer empty stays.
	void TakeBack(const std::vector<std::string>& made)
	{
		for (auto it = made.rbegin(); it != made.rend(); ++it) {
			upper->RemoveDir(it->c_str());
		}
	}
	void MarkDirty()
	{
		dirty = true;
		if (!armed) {
			armed = true;
			PIC_AddEvent(DelayedFlush, FlushDelayMs, id);
		}
	}
	// A listing of one layer's directory through the DTA the caller gave.
	void Collect(DOS_Drive& layer, const char* dir, DOS_DTA& dta,
	             const bool fcb, std::vector<DOS_DTA::Result>& out)
	{
		if (!layer.FindFirst(dir, dta, fcb)) {
			return;
		}
		for (;;) {
			DOS_DTA::Result r = {};
			dta.GetResult(r);
			out.push_back(r);
			if (!layer.FindNext(dta)) {
				break;
			}
		}
	}
};

UnionDrive::UnionDrive() : impl(std::make_unique<Impl>()) {}

UnionDrive::~UnionDrive()
{
	auto error = std::string();
	if (!Flush(&error)) {
		augra::log_warn(LogComponent,
		                "record write at unmount failed: %s",
		                error.c_str());
	}
	Registry().erase(impl->id);
}

std::shared_ptr<UnionDrive> UnionDrive::Create(std::shared_ptr<ArchiveDrive> lower,
                                               const std::filesystem::path& sidecar_dir,
                                               const char drive_letter,
                                               const SidecarRecord& identity,
                                               std::string* error)
{
	std::error_code ec = {};
	if (!lower || !std::filesystem::is_directory(sidecar_dir, ec)) {
		if (error) {
			*error = "no lower drive or no sidecar directory";
		}
		return nullptr;
	}
	auto drive = std::shared_ptr<UnionDrive>(new UnionDrive());
	auto base  = sidecar_dir.string();
	if (base.back() != CROSS_FILESPLIT) {
		base += CROSS_FILESPLIT;
	}
	drive->impl->upper = std::make_shared<localDrive>(
	        base.c_str(), 512, 32, 32765, 16000, MediaId::HardDisk, false, false);
	drive->impl->lower       = std::move(lower);
	drive->impl->sidecar_dir = sidecar_dir;
	drive->impl->letter      = drive_letter;
	const auto toml          = sidecar_dir / ArchiveMount::SidecarTomlName;
	if (std::filesystem::exists(toml, ec)) {
		auto loaded = SidecarRecord::Load(toml, error);
		if (!loaded) {
			return nullptr;
		}
		drive->impl->record = std::move(*loaded);
	} else {
		if (identity.identity_hash.empty()) {
			if (error) {
				*error = "no identity for a new sidecar record";
			}
			return nullptr;
		}
		drive->impl->record = identity;
		drive->impl->record.changes.clear();
		drive->impl->dirty = true;
	}
	drive->impl->id             = next_union_id++;
	Registry()[drive->impl->id] = drive.get();
	drive->type                 = drive->impl->lower->GetType();
	safe_strcpy(drive->info, drive->impl->lower->GetInfo());
	return drive;
}

bool UnionDrive::Flush(std::string* error)
{
	impl->armed = false;
	if (!impl->dirty) {
		return true;
	}
	if (!impl->record.Save(impl->sidecar_dir / ArchiveMount::SidecarTomlName,
	                       error)) {
		return false;
	}
	impl->dirty = false;
	return true;
}

void UnionDrive::FlushAll()
{
	for (auto& [id, drive] : Registry()) {
		auto error = std::string();
		if (!drive->Flush(&error)) {
			augra::log_warn(LogComponent,
			                "record write at exit failed: %s",
			                error.c_str());
		}
	}
}

int UnionDrive::LiveUnions()
{
	return static_cast<int>(Registry().size());
}

const SidecarRecord& UnionDrive::Record() const
{
	return impl->record;
}

const std::filesystem::path& UnionDrive::SidecarDirectory() const
{
	return impl->sidecar_dir;
}

std::shared_ptr<DOS_Drive> UnionDrive::Lower() const
{
	return impl->lower;
}

bool UnionDrive::CopyUp(const char* lower_name, const char* upper_name,
                        std::vector<std::string>* parents_made)
{
	++impl->sidecar_generation;
	if (strcmp(lower_name, upper_name) == 0 && impl->UpperHas(upper_name)) {
		return true; // a second handle's first write, already copied
	}
	std::vector<std::string> made = {};
	if (!impl->SidecarPathIsPlain(upper_name) ||
	    !impl->EnsureParents(upper_name, made)) {
		impl->TakeBack(made);
		return false;
	}
	auto source = impl->lower->FileOpen(lower_name, OPEN_READ);
	if (!source) {
		impl->TakeBack(made);
		return false;
	}
	FatAttributeFlags attributes = source->attr;
	attributes.read_only         = false;
	auto target = impl->upper->FileCreate(upper_name, attributes);
	if (!target) {
		source->Close();
		impl->TakeBack(made);
		return false;
	}
	// Read and Write count in a uint16_t, so the buffer stays below 64 KiB.
	std::vector<uint8_t> buffer(32768);
	bool ok = true;
	for (;;) {
		uint16_t n = static_cast<uint16_t>(buffer.size());
		if (!source->Read(buffer.data(), &n)) {
			ok = false;
			break;
		}
		if (n == 0) {
			break;
		}
		uint16_t written = n;
		if (!target->Write(buffer.data(), &written) || written != n) {
			ok = false;
			break;
		}
	}
	// The member's date on the copy (house timestamp rule).
	target->time                = source->time;
	target->date                = source->date;
	target->flush_time_on_close = FlushTimeOnClose::ManuallySet;
	target->Close();
	source->Close();
	if (!ok) {
		impl->upper->FileUnlink(upper_name);
		impl->TakeBack(made);
		return false;
	}
	if (parents_made) {
		*parents_made = std::move(made);
	}
	return true;
}

std::unique_ptr<DOS_File> UnionDrive::FileOpen(const char* name, const uint8_t flags)
{
	if (!impl->LowerReady()) {
		return nullptr;
	}
	const bool wants_write = (flags & 0x0f) != OPEN_READ;
	if (impl->UpperHas(name)) {
		if (wants_write && !impl->SidecarPathIsPlain(name)) {
			DOS_SetError(DOSERR_ACCESS_DENIED);
			return nullptr;
		}
		return impl->upper->FileOpen(name, flags);
	}
	if (!impl->LowerHas(name)) {
		DOS_SetError(DOSERR_FILE_NOT_FOUND);
		return nullptr;
	}
	auto lower_file = impl->lower->FileOpen(name, OPEN_READ);
	if (!lower_file) {
		return nullptr;
	}
	auto file   = std::make_unique<UnionFile>(shared_from_this(),
                                                name,
                                                std::move(lower_file),
                                                flags);
	file->flags = flags;
	return file;
}

std::unique_ptr<DOS_File> UnionDrive::FileCreate(const char* name,
                                                 FatAttributeFlags attributes)
{
	if (!impl->LowerReady()) {
		return nullptr;
	}
	++impl->sidecar_generation;
	if (!impl->ParentVisible(name)) {
		DOS_SetError(DOSERR_PATH_NOT_FOUND);
		return nullptr;
	}
	// A file never shadows a lower directory the guest can still see, and
	// one over a removed directory must not bring that directory back.
	const bool over_lower_dir = impl->lower->TestDir(name);
	if (over_lower_dir && !impl->record.IsHidden(name)) {
		DOS_SetError(DOSERR_ACCESS_DENIED);
		return nullptr;
	}
	std::vector<std::string> made = {};
	if (!impl->SidecarPathIsPlain(name) || !impl->EnsureParents(name, made)) {
		impl->TakeBack(made);
		DOS_SetError(DOSERR_ACCESS_DENIED);
		return nullptr;
	}
	auto created = impl->upper->FileCreate(name, attributes);
	if (!created) {
		const auto error = dos.errorcode;
		impl->TakeBack(made);
		DOS_SetError(error);
		return nullptr;
	}
	if (!over_lower_dir && impl->record.Unhide(name)) {
		impl->MarkDirty();
	}
	return created;
}

bool UnionDrive::FileUnlink(const char* name)
{
	if (!impl->LowerReady()) {
		return false;
	}
	if (impl->UpperHas(name)) {
		// The upper copy shadows a lower file: the lower stays hidden.
		FatAttributeFlags attr = {};
		const bool shadows = impl->lower->GetFileAttr(name, &attr) &&
		                     !attr.directory;
		if (shadows &&
		    !impl->RecordHasRoom(impl->record.HasRoomForDelete(name))) {
			return false;
		}
		if (!impl->upper->FileUnlink(name)) {
			return false;
		}
		if (shadows) {
			impl->record.AddDelete(name);
			impl->MarkDirty();
		}
		return true;
	}
	if (!impl->LowerHas(name)) {
		DOS_SetError(DOSERR_FILE_NOT_FOUND);
		return false;
	}
	FatAttributeFlags attr = {};
	impl->lower->GetFileAttr(name, &attr);
	if (attr.directory) {
		DOS_SetError(DOSERR_ACCESS_DENIED);
		return false;
	}
	if (!impl->RecordHasRoom(impl->record.AddDelete(name))) {
		return false;
	}
	impl->MarkDirty();
	return true;
}

bool UnionDrive::MakeDir(const char* dir)
{
	if (!impl->LowerReady()) {
		return false;
	}
	if (TestDir(dir) || FileExists(dir)) {
		DOS_SetError(DOSERR_ACCESS_DENIED);
		return false;
	}
	if (!impl->ParentVisible(dir)) {
		DOS_SetError(DOSERR_PATH_NOT_FOUND);
		return false;
	}
	std::vector<std::string> made = {};
	if (!impl->SidecarPathIsPlain(dir) || !impl->EnsureParents(dir, made)) {
		impl->TakeBack(made);
		DOS_SetError(DOSERR_ACCESS_DENIED);
		return false;
	}
	if (!impl->upper->MakeDir(dir)) {
		const auto error = dos.errorcode;
		impl->TakeBack(made);
		DOS_SetError(error);
		return false;
	}
	if (impl->record.Unhide(dir)) {
		impl->MarkDirty();
	}
	return true;
}

bool UnionDrive::RemoveDir(const char* dir)
{
	if (!impl->LowerReady()) {
		return false;
	}
	if (!TestDir(dir)) {
		DOS_SetError(DOSERR_PATH_NOT_FOUND);
		return false;
	}
	if (impl->HasEntries(dir)) {
		DOS_SetError(DOSERR_ACCESS_DENIED);
		return false;
	}
	FatAttributeFlags attr = {};
	const bool lower_dir = impl->lower->GetFileAttr(dir, &attr) && attr.directory;
	if (lower_dir && !impl->RecordHasRoom(impl->record.HasRoomForDelete(dir))) {
		return false;
	}
	if (impl->upper->TestDir(dir) && !impl->upper->RemoveDir(dir)) {
		return false;
	}
	if (lower_dir) {
		impl->record.AddDelete(dir);
		impl->MarkDirty();
	}
	return true;
}

bool UnionDrive::TestDir(const char* dir)
{
	if (!impl->LowerReady()) {
		return false;
	}
	if (dir[0] == '\0') {
		return true;
	}
	if (impl->upper->TestDir(dir)) {
		return true;
	}
	return !impl->record.IsHidden(dir) && impl->lower->TestDir(dir);
}

bool UnionDrive::FindFirst(const char* dir, DOS_DTA& dta, const bool fcb_findfirst)
{
	if (!impl->LowerReady()) {
		return false;
	}
	FatAttributeFlags search_attr = {};
	char pattern[DOS_NAMELENGTH_ASCII];
	dta.GetSearchParams(search_attr, pattern);
	if (search_attr == FatAttributeFlags::Volume) {
		// The label comes from the lower; the DTA keeps our cursor id,
		// so FindNext ends this search instead of reading another
		// one's.
		if (!impl->lower->FindFirst(dir, dta, fcb_findfirst)) {
			return false;
		}
		DOS_DTA::Result label = {};
		dta.GetResult(label);
		const auto search = impl->searches.Acquire();
		search.cursor.results.push_back(label);
		search.cursor.next = 1;
		dta.SetDirID(search.id);
		return true;
	}
	if (!TestDir(dir)) {
		DOS_SetError(DOSERR_PATH_NOT_FOUND);
		return false;
	}
	std::vector<DOS_DTA::Result> merged = {};
	if (impl->upper->TestDir(dir)) {
		impl->Collect(*impl->upper, dir, dta, fcb_findfirst, merged);
		if (dir[0] == '\0') {
			// The local drive passes the host's dot entries through
			// at its base; a DOS root has none, and the lower emits
			// none.
			std::erase_if(merged, [](const DOS_DTA::Result& r) {
				return r.IsDummyDirectory();
			});
		}
	}
	if (!impl->record.IsHidden(dir) && impl->lower->TestDir(dir)) {
		std::vector<DOS_DTA::Result> below = {};
		impl->Collect(*impl->lower, dir, dta, fcb_findfirst, below);
		for (auto& r : below) {
			const auto path = JoinDos(dir, r.name);
			const bool shadowed =
			        std::any_of(merged.begin(),
			                    merged.end(),
			                    [&](const DOS_DTA::Result& m) {
				                    return m.name == r.name;
			                    });
			if (shadowed || (!r.IsDummyDirectory() &&
			                 impl->record.IsHidden(path))) {
				continue;
			}
			// The lower marks its members read-only; through the
			// union they are writable by copy-up, and DEL refuses
			// read-only files.
			r.attr.read_only = false;
			merged.push_back(r);
		}
	}
	const auto search     = impl->searches.Acquire();
	search.cursor.results = std::move(merged);
	dta.SetDirID(search.id);
	return FindNext(dta);
}

bool UnionDrive::FindNext(DOS_DTA& dta)
{
	if (!impl->LowerReady()) {
		return false;
	}
	const auto id = dta.GetDirID();
	auto* cursor  = impl->searches.Find(id);
	if (!cursor) {
		DOS_SetError(DOSERR_NO_MORE_FILES);
		return false;
	}
	if (cursor->next >= cursor->results.size()) {
		impl->searches.Release(id);
		DOS_SetError(DOSERR_NO_MORE_FILES);
		return false;
	}
	const auto& r = cursor->results[cursor->next++];
	dta.SetResult(r.name.c_str(), r.size, r.date, r.time, r.attr);
	return true;
}

bool UnionDrive::GetFileAttr(const char* name, FatAttributeFlags* attr)
{
	if (!impl->LowerReady()) {
		return false;
	}
	if (impl->upper->GetFileAttr(name, attr)) {
		return true;
	}
	if (impl->record.IsHidden(name)) {
		*attr = {};
		return false;
	}
	if (!impl->lower->GetFileAttr(name, attr)) {
		return false;
	}
	attr->read_only = false; // writable by copy-up
	return true;
}

bool UnionDrive::SetFileAttr(const char* name, const FatAttributeFlags attr)
{
	if (!impl->LowerReady()) {
		return false;
	}
	const bool copied_up          = !impl->UpperHas(name);
	bool is_directory             = false;
	std::vector<std::string> made = {};
	if (copied_up) {
		if (!impl->LowerHas(name)) {
			DOS_SetError(DOSERR_FILE_NOT_FOUND);
			return false;
		}
		FatAttributeFlags current = {};
		impl->lower->GetFileAttr(name, &current);
		is_directory = current.directory;
		if (is_directory) {
			if (!impl->SidecarPathIsPlain(name) ||
			    !impl->EnsureParents(name, made) ||
			    !impl->upper->MakeDir(name)) {
				impl->TakeBack(made);
				DOS_SetError(DOSERR_ACCESS_DENIED);
				return false;
			}
		} else if (!CopyUp(name, name, &made)) {
			DOS_SetError(DOSERR_ACCESS_DENIED);
			return false;
		}
	}
	if (impl->upper->SetFileAttr(name, attr)) {
		return true;
	}
	// A host without an attribute store (FreeBSD) refuses here; the
	// failed call leaves no copy behind.
	if (copied_up) {
		const auto error = dos.errorcode;
		if (is_directory) {
			impl->upper->RemoveDir(name);
		} else {
			impl->upper->FileUnlink(name);
		}
		impl->TakeBack(made);
		DOS_SetError(error);
	}
	return false;
}

bool UnionDrive::Rename(const char* oldname, const char* newname)
{
	if (!impl->LowerReady()) {
		return false;
	}
	++impl->sidecar_generation;
	if (!impl->ParentVisible(newname)) {
		DOS_SetError(DOSERR_PATH_NOT_FOUND);
		return false;
	}
	const bool over_lower_dir = impl->lower->TestDir(newname);
	if (over_lower_dir && !impl->record.IsHidden(newname)) {
		DOS_SetError(DOSERR_ACCESS_DENIED);
		return false;
	}
	if (impl->UpperHas(oldname)) {
		FatAttributeFlags lower_attr = {};
		if (impl->LowerHas(oldname) &&
		    impl->lower->GetFileAttr(oldname, &lower_attr) &&
		    lower_attr.directory) {
			DOS_SetError(DOSERR_ACCESS_DENIED); // D3: the lower half
			                                    // would stay behind
			return false;
		}
		// The shadowed lower file stays hidden.
		FatAttributeFlags attr = {};
		const bool shadows = impl->lower->GetFileAttr(oldname, &attr) &&
		                     !attr.directory;
		if (shadows &&
		    !impl->RecordHasRoom(impl->record.HasRoomForDelete(oldname))) {
			return false;
		}
		std::vector<std::string> made = {};
		if (!impl->SidecarPathIsPlain(oldname) ||
		    !impl->SidecarPathIsPlain(newname) ||
		    !impl->EnsureParents(newname, made)) {
			impl->TakeBack(made);
			DOS_SetError(DOSERR_ACCESS_DENIED);
			return false;
		}
		if (!impl->upper->Rename(oldname, newname)) {
			const auto error = dos.errorcode;
			impl->TakeBack(made);
			DOS_SetError(error);
			return false;
		}
		bool changed = false;
		if (shadows) {
			impl->record.AddDelete(oldname);
			changed = true;
		}
		if (!over_lower_dir && impl->record.Unhide(newname)) {
			changed = true;
		}
		if (changed) {
			impl->MarkDirty();
		}
		return true;
	}
	if (!impl->LowerHas(oldname)) {
		DOS_SetError(DOSERR_FILE_NOT_FOUND);
		return false;
	}
	FatAttributeFlags attr = {};
	impl->lower->GetFileAttr(oldname, &attr);
	if (attr.directory) {
		DOS_SetError(DOSERR_ACCESS_DENIED); // D3: no directory rename
		                                    // across the layers
		return false;
	}
	if (!impl->RecordHasRoom(impl->record.HasRoomForRename(oldname, newname))) {
		return false;
	}
	if (!CopyUp(oldname, newname)) {
		DOS_SetError(DOSERR_ACCESS_DENIED);
		return false;
	}
	impl->record.AddRename(oldname, newname);
	!over_lower_dir && impl->record.Unhide(newname);
	impl->MarkDirty();
	return true;
}

bool UnionDrive::AllocationInfo(uint16_t* bytes_sector, uint8_t* sectors_cluster,
                                uint16_t* total_clusters, uint16_t* free_clusters)
{
	return impl->upper->AllocationInfo(bytes_sector,
	                                   sectors_cluster,
	                                   total_clusters,
	                                   free_clusters);
}

bool UnionDrive::FileExists(const char* name)
{
	if (!impl->LowerReady()) {
		return false;
	}
	if (impl->upper->FileExists(name)) {
		return true;
	}
	return !impl->record.IsHidden(name) && impl->lower->FileExists(name);
}

uint8_t UnionDrive::GetMediaByte()
{
	return impl->upper->GetMediaByte();
}

void UnionDrive::EmptyCache()
{
	impl->upper->EmptyCache();
	impl->lower->EmptyCache();
}

bool UnionDrive::IsRemote()
{
	return false;
}

bool UnionDrive::IsRemovable()
{
	return false;
}

Bits UnionDrive::UnMount()
{
	if (DriveHasOpenFiles(this)) {
		return UnmountRefusedOpenFiles;
	}
	auto error = std::string();
	if (!Flush(&error)) {
		augra::log_warn(LogComponent,
		                "record write at unmount failed: %s",
		                error.c_str());
	}
	return impl->lower->UnMount();
}

uint64_t UnionDrive::SidecarGeneration() const
{
	return impl->sidecar_generation;
}

bool UnionDrive::InSidecar(const char* name)
{
	return impl->UpperHas(name);
}

bool UnionDrive::IsReady()
{
	return impl->lower->IsReady();
}

const char* UnionDrive::GetLabel()
{
	return impl->lower->GetLabel();
}

bool DriveHasOpenFiles(const DOS_Drive* drive)
{
	// Device handles carry no drive in the table.
	return std::any_of(Files.begin(), Files.end(), [drive](const auto& file) {
		return file && file->GetDrive() < Drives.size() &&
		       Drives[file->GetDrive()].get() == drive;
	});
}

std::shared_ptr<ArchiveDrive> ArchiveDriveOf(const std::shared_ptr<DOS_Drive>& drive)
{
	if (const auto unioned = std::dynamic_pointer_cast<UnionDrive>(drive)) {
		return std::dynamic_pointer_cast<ArchiveDrive>(unioned->Lower());
	}
	return std::dynamic_pointer_cast<ArchiveDrive>(drive);
}
