// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//
#include "dos/archive/archive_tree.h"

#include "utils/checks.h"

#include <algorithm>
#include <ctime>

CHECK_NARROWING();

namespace ArchiveMount {

namespace {

constexpr int64_t FatEpochSeconds = 315532800;  // 1980-01-01 00:00:00 UTC
constexpr int64_t FatLastSeconds  = 4354819199; // 2107-12-31 23:59:59 UTC

bool LocalTime(const int64_t seconds, struct tm& out)
{
	const auto t = static_cast<time_t>(seconds);
#if defined(WIN32)
	return localtime_s(&out, &t) == 0;
#else
	return localtime_r(&t, &out) != nullptr;
#endif
}

// A DOS path in the index's form: no empty component, no leading or
// trailing backslash. Case and character set are the index's business.
bool WellFormedDosPath(const std::string_view path)
{
	if (path.empty()) {
		return false;
	}
	if (path.front() == '\\' || path.back() == '\\') {
		return false;
	}
	return path.find("\\\\") == std::string_view::npos;
}

std::string_view ParentOf(const std::string_view path)
{
	const auto slash = path.rfind('\\');
	return slash == std::string_view::npos ? std::string_view{}
	                                       : path.substr(0, slash);
}

std::string_view LeafOf(const std::string_view path)
{
	const auto slash = path.rfind('\\');
	return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

} // namespace

DosDateTime PackDosDateTime(const int64_t unix_seconds)
{
	constexpr DosDateTime first = {(0 << 9) | (1 << 5) | 1, 0};
	constexpr DosDateTime last  = {(127 << 9) | (12 << 5) | 31,
	                               (23 << 11) | (59 << 5) | 29};
	if (unix_seconds <= FatEpochSeconds) {
		return first;
	}
	if (unix_seconds >= FatLastSeconds) {
		return last;
	}
	struct tm local = {};
	if (!LocalTime(unix_seconds, local)) {
		return first;
	}
	// A zone west of UTC puts the first hours of 1980 into 1979 and a
	// zone east of it the last of 2107 into 2108: those are the edges.
	const int year = local.tm_year + 1900;
	if (year < 1980) {
		return first;
	}
	if (year > 2107) {
		return last;
	}
	DosDateTime out = {};
	out.date        = static_cast<uint16_t>(
                ((year - 1980) << 9) | ((local.tm_mon + 1) << 5) | local.tm_mday);
	out.time = static_cast<uint16_t>((local.tm_hour << 11) |
	                                 (local.tm_min << 5) | (local.tm_sec / 2));
	return out;
}

std::optional<ArchiveTree> ArchiveTree::Build(const std::vector<Entry>& entries,
                                              const std::string_view root_dos_path,
                                              std::string* error)
{
	auto fail = [&](const std::string& what) {
		if (error) {
			*error = what;
		}
		return std::optional<ArchiveTree>{};
	};
	if (!root_dos_path.empty() && !WellFormedDosPath(root_dos_path)) {
		return fail("root path is not a DOS path: " +
		            std::string(root_dos_path));
	}
	ArchiveTree tree = {};
	TreeNode root    = {};
	root.is_dir      = true;
	tree.nodes.push_back(root);
	tree.by_path.emplace("", 0);

	const auto prefix = root_dos_path.empty()
	                          ? std::string()
	                          : std::string(root_dos_path) + "\\";
	bool root_seen    = root_dos_path.empty();

	for (size_t i = 0; i < entries.size(); ++i) {
		const auto& entry = entries[i];
		if (!WellFormedDosPath(entry.dos_path)) {
			return fail("entry has no DOS path in the index's form: " +
			            entry.utf8_path);
		}
		if (!root_dos_path.empty()) {
			if (entry.dos_path == root_dos_path) {
				if (!entry.is_dir) {
					return fail("root path names a file: " +
					            std::string(root_dos_path));
				}
				root_seen = true;
				continue;
			}
			if (!entry.dos_path.starts_with(prefix)) {
				continue;
			}
		}
		const auto relative =
		        std::string_view(entry.dos_path).substr(prefix.size());
		const auto parent = ParentOf(relative);
		const auto found  = tree.by_path.find(std::string(parent));
		if (found == tree.by_path.end()) {
			return fail("parent directory missing in the index for " +
			            entry.dos_path);
		}
		auto& parent_node = tree.nodes[static_cast<size_t>(found->second)];
		if (!parent_node.is_dir) {
			return fail("a file stands where a directory is needed: " +
			            entry.dos_path);
		}
		if (tree.by_path.contains(std::string(relative))) {
			return fail("duplicate DOS path: " + entry.dos_path);
		}
		TreeNode node    = {};
		node.name        = std::string(LeafOf(relative));
		node.dos_path    = std::string(relative);
		node.entry_index = static_cast<int>(i);
		node.ordinal     = entry.ordinal;
		node.is_dir      = entry.is_dir;
		node.size        = entry.is_dir ? 0 : entry.size;
		const auto stamp = PackDosDateTime(entry.mtime);
		node.dos_date    = stamp.date;
		node.dos_time    = stamp.time;
		const auto index = static_cast<int>(tree.nodes.size());
		tree.nodes.push_back(std::move(node));
		tree.nodes[static_cast<size_t>(found->second)].children.push_back(index);
		tree.by_path.emplace(std::string(relative), index);
		if (!entry.is_dir) {
			tree.total_bytes += entry.size;
			++tree.file_count;
		}
	}
	if (!root_seen) {
		return fail("root path not in the archive: " +
		            std::string(root_dos_path));
	}
	return tree;
}

const TreeNode* ArchiveTree::Find(const std::string_view dos_path) const
{
	const auto found = by_path.find(std::string(dos_path));
	if (found == by_path.end()) {
		return nullptr;
	}
	return &nodes[static_cast<size_t>(found->second)];
}

std::string SoloRootDirectory(const std::vector<Entry>& entries)
{
	std::string top = {};
	for (const auto& entry : entries) {
		const auto first = std::string_view(entry.dos_path);
		const auto slash = first.find('\\');
		if (slash == std::string_view::npos) {
			// A top-level entry: only the one directory may be here.
			if (!entry.is_dir) {
				return {};
			}
			if (!top.empty() && top != entry.dos_path) {
				return {};
			}
			top = entry.dos_path;
			continue;
		}
		const auto head = std::string(first.substr(0, slash));
		if (top.empty()) {
			top = head;
		} else if (top != head) {
			return {};
		}
	}
	return top;
}

} // namespace ArchiveMount
