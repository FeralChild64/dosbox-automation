// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//
#ifndef DOSBOX_ARCHIVE_TREE_H
#define DOSBOX_ARCHIVE_TREE_H

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "dos/archive/archive_source.h"

namespace ArchiveMount {

struct DosDateTime {
	uint16_t date = 0;
	uint16_t time = 0;
};

// The FAT date and time fields of a Unix time in the local zone, as
// DOS_PackDate and DOS_PackTime in dos/dos.h pack them (asserted equal
// in drive_archive_tests); clamped to 1980-01-01 and 2107-12-31.
DosDateTime PackDosDateTime(int64_t unix_seconds);

struct TreeNode {
	std::string name     = {}; // the 8.3 component, uppercase
	std::string dos_path = {}; // relative to the tree's root, "" for it
	int entry_index      = -1; // into the entries the tree was built from
	int ordinal          = ImpliedOrdinal;
	bool is_dir          = false;
	int64_t size         = 0;
	uint16_t dos_date    = 0;
	uint16_t dos_time    = 0;
	std::vector<int> children = {}; // node indices, index order
};

class ArchiveTree {
public:
	// root_dos_path: "" for the whole archive, or the directory that
	// becomes the root (design D10); entries outside it are absent. Refuses
	// a missing parent, a duplicate, a file as a directory, a malformed path.
	static std::optional<ArchiveTree> Build(const std::vector<Entry>& entries,
	                                        std::string_view root_dos_path,
	                                        std::string* error);

	const TreeNode& Root() const
	{
		return nodes[0];
	}
	const TreeNode& Node(const int index) const
	{
		return nodes[static_cast<size_t>(index)];
	}
	// Exact match on the DOS path as DOS_MakeName produces it: uppercase,
	// backslashes, no leading or trailing backslash. "" is the root.
	const TreeNode* Find(std::string_view dos_path) const;

	int64_t TotalBytes() const
	{
		return total_bytes;
	}
	int FileCount() const
	{
		return file_count;
	}

private:
	std::vector<TreeNode> nodes                  = {};
	std::unordered_map<std::string, int> by_path = {};
	int64_t total_bytes                          = 0;
	int file_count                               = 0;
};

// The one top-level directory every entry lives under (design D10), or
// "" when the root holds anything else or nothing.
std::string SoloRootDirectory(const std::vector<Entry>& entries);

} // namespace ArchiveMount

#endif // DOSBOX_ARCHIVE_TREE_H
