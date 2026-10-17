// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//
#include "dos/archive/sidecar_record.h"

#include "augra/log.h"
#include "utils/checks.h"

#include <algorithm>

CHECK_NARROWING();

namespace ArchiveMount {

namespace {

constexpr auto LogComponent = "sidecar";

bool IsNameByte(const char c)
{
	// The FAT short-name set DOS produces: uppercase, digits, and the
	// punctuation generate_8x3 keeps; a space never reaches a DOS name here.
	if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
		return true;
	}
	return std::string_view("!#$%&'()-@^_`{}~").find(c) != std::string_view::npos;
}

bool IsComponent(const std::string_view component)
{
	if (component.empty() || component == "." || component == "..") {
		return false;
	}
	const auto dot  = component.find('.');
	const auto name = dot == std::string_view::npos ? component
	                                                : component.substr(0, dot);
	const auto ext  = dot == std::string_view::npos ? std::string_view{}
	                                                : component.substr(dot + 1);
	if (name.empty() || name.size() > 8 || ext.size() > 3) {
		return false;
	}
	if (ext.find('.') != std::string_view::npos) {
		return false;
	}
	return std::all_of(name.begin(), name.end(), IsNameByte) &&
	       std::all_of(ext.begin(), ext.end(), IsNameByte);
}

// "NAME." is the same file as "NAME" in DOS; strip the empty extension
// from each component so the record keys by the canonical form only.
std::string CanonicalDosPath(const std::string_view path)
{
	std::string out;
	out.reserve(path.size());
	size_t start = 0;
	while (start <= path.size()) {
		const auto end = path.find('\\', start);
		auto part      = path.substr(start,
		                             end == std::string_view::npos
		                                     ? std::string_view::npos
		                                     : end - start);
		if (part.size() > 1 && part.back() == '.') {
			part.remove_suffix(1);
		}
		out.append(part);
		if (end == std::string_view::npos) {
			break;
		}
		out.push_back('\\');
		start = end + 1;
	}
	return out;
}

} // namespace

bool IsSidecarDosPath(const std::string_view path)
{
	if (path.empty() || path.front() == '\\' || path.back() == '\\') {
		return false;
	}
	size_t start = 0;
	while (start <= path.size()) {
		const auto end       = path.find('\\', start);
		const auto component = path.substr(start,
		                                   end == std::string_view::npos
		                                           ? std::string_view::npos
		                                           : end - start);
		if (!IsComponent(component)) {
			return false;
		}
		if (end == std::string_view::npos) {
			break;
		}
		start = end + 1;
	}
	return true;
}

namespace {

ArchiveToml::Table ChangeTable(const SidecarChange& c)
{
	ArchiveToml::Table t = {};
	if (c.op == SidecarChange::Op::Delete) {
		t.Set("op", "delete");
		t.Set("path", c.path);
	} else {
		t.Set("op", "rename");
		t.Set("from", c.path);
		t.Set("to", c.to);
	}
	return t;
}

struct Size {
	size_t bytes = 0;
	int lines    = 0;
};

// Measured by the serializer itself: a record is its root pairs followed
// by one table per change, so the parts add up.
Size Measure(const ArchiveToml::Document& doc)
{
	const auto text = ArchiveToml::Serialize(doc);
	return {text.size(),
	        static_cast<int>(std::count(text.begin(), text.end(), '\n'))};
}

Size ChangeSize(const SidecarChange& c)
{
	ArchiveToml::Document doc = {};
	doc.arrays.emplace_back("changes", ChangeTable(c));
	return Measure(doc);
}

} // namespace

bool SidecarRecord::Fits(const SidecarChange& change) const
{
	const auto top = Measure(HeaderDocument());
	const auto add = ChangeSize(change);
	// One line more than the newlines, for a last line without one.
	return top.bytes + change_bytes + add.bytes <= ArchiveToml::MaxDocumentBytes &&
	       top.lines + change_lines + add.lines + 1 <=
	               ArchiveToml::MaxDocumentLines;
}

bool SidecarRecord::HasRoomForDelete(const std::string_view dos_path) const
{
	const auto key = CanonicalDosPath(dos_path);
	return IsHidden(key) ||
	       Fits({SidecarChange::Op::Delete, key, {}});
}

bool SidecarRecord::HasRoomForRename(const std::string_view from,
                                     const std::string_view to) const
{
	return Fits({SidecarChange::Op::Rename,
	             CanonicalDosPath(from),
	             CanonicalDosPath(to)});
}

bool SidecarRecord::AddDelete(const std::string_view dos_path)
{
	const auto key = CanonicalDosPath(dos_path);
	if (IsHidden(key)) {
		return true;
	}
	const SidecarChange change = {SidecarChange::Op::Delete, key, {}};
	if (!Fits(change)) {
		return false;
	}
	changes.push_back(change);
	const auto size = ChangeSize(change);
	change_bytes += size.bytes;
	change_lines += size.lines;
	hidden.emplace(key);
	return true;
}

bool SidecarRecord::AddRename(const std::string_view from, const std::string_view to)
{
	const auto key_from = CanonicalDosPath(from);
	const auto key_to   = CanonicalDosPath(to);
	for (const auto& c : changes) {
		if (c.op == SidecarChange::Op::Rename && c.path == key_from &&
		    c.to == key_to) {
			return true;
		}
	}
	const SidecarChange change = {SidecarChange::Op::Rename, key_from, key_to};
	if (!Fits(change)) {
		return false;
	}
	changes.push_back(change);
	const auto size = ChangeSize(change);
	change_bytes += size.bytes;
	change_lines += size.lines;
	hidden.emplace(key_from);
	return true;
}

bool SidecarRecord::Unhide(const std::string_view dos_path)
{
	const auto key    = CanonicalDosPath(dos_path);
	const auto before = changes.size();
	std::erase_if(changes,
	              [&](const SidecarChange& c) { return c.path == key; });
	if (changes.size() == before) {
		return false;
	}
	Rebuild();
	return true;
}

bool SidecarRecord::IsHidden(const std::string_view dos_path) const
{
	if (dos_path.empty()) {
		return false;
	}
	auto probe = CanonicalDosPath(dos_path);
	for (;;) {
		if (hidden.contains(probe)) {
			return true;
		}
		const auto slash = probe.rfind('\\');
		if (slash == std::string::npos) {
			return false;
		}
		probe.resize(slash);
	}
}

void SidecarRecord::Rebuild()
{
	hidden.clear();
	change_bytes = 0;
	change_lines = 0;
	for (const auto& c : changes) {
		hidden.insert(c.path);
		const auto size = ChangeSize(c);
		change_bytes += size.bytes;
		change_lines += size.lines;
	}
}

ArchiveToml::Document SidecarRecord::HeaderDocument() const
{
	ArchiveToml::Document doc = {};
	doc.root.Set("version", int64_t{SidecarRecordVersion});
	doc.root.Set("archive_path", archive_path);
	doc.root.Set("identity_hash", identity_hash);
	doc.root.Set("entry_count", entry_count);
	doc.root.Set("total_bytes", total_bytes);
	doc.root.Set("created", created);
	return doc;
}

ArchiveToml::Document SidecarRecord::ToDocument() const
{
	auto doc = HeaderDocument();
	for (const auto& c : changes) {
		doc.arrays.emplace_back("changes", ChangeTable(c));
	}
	return doc;
}

std::optional<SidecarRecord> SidecarRecord::FromDocument(const ArchiveToml::Document& doc,
                                                         std::string* error)
{
	auto fail = [&](const char* what) {
		if (error) {
			*error = what;
		}
		return std::optional<SidecarRecord>{};
	};
	if (doc.root.Integer("version") != SidecarRecordVersion) {
		return fail("not a version 1 sidecar record");
	}
	const auto hash = doc.root.String("identity_hash");
	if (!hash || hash->empty()) {
		return fail("no identity hash");
	}
	SidecarRecord r = {};
	r.archive_path  = doc.root.String("archive_path").value_or("");
	r.identity_hash = *hash;
	r.entry_count   = doc.root.Integer("entry_count").value_or(0);
	r.total_bytes   = doc.root.Integer("total_bytes").value_or(0);
	r.created       = doc.root.Integer("created").value_or(0);
	for (const auto* t : doc.Array("changes")) {
		const auto op = t->String("op").value_or("");
		if (op == "delete") {
			const auto path = t->String("path").value_or("");
			if (!IsSidecarDosPath(path)) {
				++r.dropped_on_load;
				augra::log_warn(LogComponent,
				                "dropping a delete of '%s': not a DOS path",
				                path.c_str());
				continue;
			}
			r.AddDelete(path);
		} else if (op == "rename") {
			const auto from = t->String("from").value_or("");
			const auto to   = t->String("to").value_or("");
			if (!IsSidecarDosPath(from) || !IsSidecarDosPath(to)) {
				++r.dropped_on_load;
				augra::log_warn(LogComponent,
				                "dropping a rename '%s' -> '%s': not DOS paths",
				                from.c_str(),
				                to.c_str());
				continue;
			}
			r.AddRename(from, to);
		} else {
			++r.dropped_on_load;
			augra::log_warn(LogComponent,
			                "dropping a change with op '%s'",
			                op.c_str());
		}
	}
	return r;
}

bool SidecarRecord::Save(const std::filesystem::path& path, std::string* error) const
{
	return ArchiveToml::WriteAtomically(path, ToDocument(), error);
}

std::optional<SidecarRecord> SidecarRecord::Load(const std::filesystem::path& path,
                                                 std::string* error)
{
	const auto doc = ArchiveToml::ReadFile(path, error);
	if (!doc) {
		return {};
	}
	return FromDocument(*doc, error);
}

} // namespace ArchiveMount
