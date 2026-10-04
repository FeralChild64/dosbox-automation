// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//
#ifndef DOSBOX_SIDECAR_RECORD_H
#define DOSBOX_SIDECAR_RECORD_H

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "dos/archive/archive_toml.h"

namespace ArchiveMount {

constexpr const char* SidecarTomlName = ".source-archive.toml";
constexpr int SidecarRecordVersion    = 1;

// Uppercase 8.3 components joined by backslashes, no leading or trailing
// one, no dot components: the only form a DOS name takes in the toml.
bool IsSidecarDosPath(std::string_view path);

struct SidecarChange {
	enum class Op { Delete, Rename };
	Op op            = Op::Delete;
	std::string path = {}; // the deleted path, or a rename's source
	std::string to   = {}; // a rename's target
};

// The sidecar's .source-archive.toml (design D2, D3): what the archive
// was, and which lower entries the game deleted or moved.
struct SidecarRecord {
	std::string archive_path           = {};
	std::string identity_hash          = {};
	int64_t entry_count                = 0;
	int64_t total_bytes                = 0;
	int64_t created                    = 0;
	std::vector<SidecarChange> changes = {};
	int dropped_on_load                = 0;

	// False, with nothing changed, when the record would grow past what
	// Load accepts. Deletes below a deleted directory stay: recreating the
	// directory must bring it back empty.
	bool AddDelete(std::string_view dos_path);
	bool AddRename(std::string_view from, std::string_view to);
	bool HasRoomForDelete(std::string_view dos_path) const;
	bool HasRoomForRename(std::string_view from, std::string_view to) const;
	// A create or MakeDir on a hidden name: drops the change that hid it.
	// True when something was dropped.
	bool Unhide(std::string_view dos_path);
	// The path or any of its parents is deleted or was renamed away.
	bool IsHidden(std::string_view dos_path) const;

	ArchiveToml::Document ToDocument() const;
	static std::optional<SidecarRecord> FromDocument(const ArchiveToml::Document& doc,
	                                                 std::string* error);
	bool Save(const std::filesystem::path& path, std::string* error) const;
	static std::optional<SidecarRecord> Load(const std::filesystem::path& path,
	                                         std::string* error);

private:
	std::unordered_set<std::string> hidden = {};
	size_t change_bytes                    = 0; // serialized, all changes
	int change_lines                       = 0;
	void Rebuild();
	bool Fits(const SidecarChange& change) const;
	ArchiveToml::Document HeaderDocument() const;
};

} // namespace ArchiveMount

#endif // DOSBOX_SIDECAR_RECORD_H
