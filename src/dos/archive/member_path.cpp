// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//
#include "dos/archive/member_path.h"

#include "dos/archive/descriptor.h"
#include "dos/archive/head_sniff.h"

#include <algorithm>
#include <cctype>

namespace ArchiveMount {

namespace {

bool IsHostSeparator(const char c)
{
#if defined(WIN32)
	return c == '/' || c == '\\';
#else
	return c == '/';
#endif
}

std::string AsciiUpper(std::string s)
{
	std::transform(s.begin(), s.end(), s.begin(), [](const unsigned char c) {
		return static_cast<char>(std::toupper(c));
	});
	return s;
}

bool EqualsIgnoringAsciiCase(const std::string_view a, const std::string_view b)
{
	return a.size() == b.size() &&
	       std::equal(a.begin(),
	                  a.end(),
	                  b.begin(),
	                  [](const unsigned char x, const unsigned char y) {
		                  return std::toupper(x) == std::toupper(y);
	                  });
}

} // namespace

bool IsArchiveFile(const std::filesystem::path& host_path)
{
	static const char* suffixes[] = {".zip",
	                                 ".rar",
	                                 ".7z",
	                                 ".tar",
	                                 ".tar.gz",
	                                 ".tgz",
	                                 ".tar.xz",
	                                 ".txz",
	                                 ".tar.zst",
	                                 ".tzst",
	                                 ".tar.bz2",
	                                 ".tbz2"};
	auto lower                    = host_path.filename().string();
	std::transform(lower.begin(), lower.end(), lower.begin(), [](const unsigned char c) {
		return static_cast<char>(std::tolower(c));
	});
	const bool listed = std::any_of(std::begin(suffixes),
	                                std::end(suffixes),
	                                [&](const char* suffix) {
		                                return lower.ends_with(suffix);
	                                });
	if (!listed) {
		return false;
	}
	const int fd = OpenReadOnlyShared(host_path);
	if (fd < 0) {
		return false;
	}
	const auto sniff = SniffFile(fd);
	CloseDescriptor(fd);
	return sniff.kind != HeadKind::Unknown;
}

std::optional<MemberPath> SplitMemberPath(const std::string& raw)
{
	std::error_code ec = {};
	if (raw.empty() ||
	    std::filesystem::exists(std::filesystem::symlink_status(raw, ec))) {
		return {};
	}
	for (size_t i = 1; i < raw.size(); ++i) {
		if (!IsHostSeparator(raw[i])) {
			continue;
		}
		const auto prefix = raw.substr(0, i);
		ec.clear();
		const auto status = std::filesystem::status(prefix, ec);
		if (ec) {
			return {};
		}
		if (std::filesystem::is_directory(status)) {
			continue;
		}
		if (!std::filesystem::is_regular_file(status) ||
		    !IsArchiveFile(prefix)) {
			return {};
		}
		auto member = raw.substr(i + 1);
		if (member.empty()) {
			return {};
		}
		return MemberPath{prefix, std::move(member)};
	}
	return {};
}

const Entry* FindMember(const std::vector<Entry>& entries,
                        const std::string_view typed,
                        ArchiveNames::Rejection* rejection)
{
	const auto split = ArchiveNames::SplitEntryPath(typed, rejection);
	if (!split || split->components.empty() || split->is_dir) {
		return nullptr;
	}
	std::string utf8 = {};
	std::string dos  = {};
	for (const auto& component : split->components) {
		if (!utf8.empty()) {
			utf8 += '/';
			dos += '\\';
		}
		utf8 += component;
		dos += AsciiUpper(component);
	}
	const auto is_file = [](const Entry& e) {
		return !e.is_dir && e.ordinal != ImpliedOrdinal;
	};
	for (const auto& e : entries) {
		if (is_file(e) && e.utf8_path == utf8) {
			return &e;
		}
	}
	for (const auto& e : entries) {
		if (is_file(e) && e.dos_path == dos) {
			return &e;
		}
	}
	const Entry* found = nullptr;
	for (const auto& e : entries) {
		if (is_file(e) && EqualsIgnoringAsciiCase(e.utf8_path, utf8)) {
			if (found) {
				return nullptr;
			}
			found = &e;
		}
	}
	return found;
}

} // namespace ArchiveMount
