// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//
#include "dos/archive/member_path.h"

#include "dos/archive/descriptor.h"
#include "dos/archive/head_sniff.h"

#include <algorithm>
#include <cctype>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>

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

// The suffixes are ASCII, so anything else stands as one 0x80 byte: on
// Windows a narrow conversion of the name would throw outside the code page.
template <typename Char>
std::string lower_ascii(const std::basic_string_view<Char> name)
{
	auto lower = std::string();
	lower.reserve(name.size());
	for (const auto c : name) {
		const auto code = static_cast<std::make_unsigned_t<Char>>(c);
		lower.push_back(code < 0x80 ? static_cast<char>(std::tolower(code))
		                            : '\x80');
	}
	return lower;
}

std::string lower_file_name(const std::filesystem::path& path)
{
	const auto name = path.filename();
	return lower_ascii(std::basic_string_view(name.native()));
}

std::string lower_file_name(const std::string_view name)
{
	const auto slash = name.find_last_of("/\\");
	return lower_ascii(
	        slash == std::string_view::npos ? name : name.substr(slash + 1));
}

} // namespace

std::string PathToUtf8(const std::filesystem::path& path)
{
#if defined(WIN32)
	const auto u8 = path.u8string();
	return std::string(u8.begin(), u8.end());
#else
	return path.string();
#endif
}

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
	                                 ".tbz2",
	                                 ".tar.lzma"};
	const auto lower              = lower_file_name(host_path);
	const bool listed             = std::any_of(std::begin(suffixes),
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

namespace {

// The filter a compressed image name promises, or nothing.
std::optional<HeadKind> compressed_image_filter(const std::string& lower)
{
	static const std::pair<const char*, HeadKind> filters[] = {
	        { ".gz",  HeadKind::Gzip},
	        {".bz2", HeadKind::Bzip2},
	        { ".xz",    HeadKind::Xz},
	        {".zst",  HeadKind::Zstd},
	};
	static const char* images[] = {".img",
	                               ".ima",
	                               ".dsk",
	                               ".vhd",
	                               ".iso",
	                               ".bin",
	                               ".flp",
	                               ".vfd",
	                               ".360",
	                               ".720",
	                               ".1200",
	                               ".1440"};
	for (const auto& [suffix, kind] : filters) {
		if (!lower.ends_with(suffix)) {
			continue;
		}
		const auto rest = std::string_view(lower).substr(
		        0, lower.size() - std::string_view(suffix).size());
		for (const auto* image : images) {
			if (rest.ends_with(image) &&
			    rest.size() > std::string_view(image).size()) {
				return kind;
			}
		}
		return {};
	}
	return {};
}

} // namespace

bool IsCompressedImageName(const std::string_view name)
{
	return compressed_image_filter(lower_file_name(name)).has_value();
}

std::optional<HeadKind> ContainerKindByName(const std::filesystem::path& host_path)
{
	const auto lower = lower_file_name(host_path);
	if (const auto filter = compressed_image_filter(lower)) {
		return filter;
	}
	if (lower.size() > 4 && lower.ends_with(".imz")) {
		return HeadKind::Zip;
	}
	if (lower.size() > 4 && lower.ends_with(".isz")) {
		return HeadKind::Isz;
	}
	return {};
}

bool IsSingleImageContainer(const std::filesystem::path& host_path)
{
	const auto expected = ContainerKindByName(host_path);
	if (!expected) {
		return false;
	}
	const int fd = OpenReadOnlyShared(host_path);
	if (fd < 0) {
		return false;
	}
	const auto sniff = SniffFile(fd);
	CloseDescriptor(fd);
	return sniff.kind == *expected;
}

std::optional<MemberPath> LoneImageMember(const std::filesystem::path& host_path)
{
	if (!IsSingleImageContainer(host_path)) {
		return {};
	}
	return MemberPath{host_path, {}, true};
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
