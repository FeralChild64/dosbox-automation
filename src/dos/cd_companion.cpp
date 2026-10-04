// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//
#include "dos/cd_companion.h"

#include "dos/dos_names.h"
#include "utils/checks.h"

#include <algorithm>
#include <system_error>
#include <utility>
#include <vector>

CHECK_NARROWING();

namespace CdCompanion {

namespace {

namespace fs = std::filesystem;

std::optional<Resolved> Refuse(std::string* why, const std::string& text)
{
	if (why) {
		*why = text;
	}
	return {};
}

// A name with a drive or a directory in it is a path from the ripper's
// machine; only its last component can mean anything here.
bool HasDirectoryPart(const std::string_view name)
{
	return name.find_first_of("/\\") != std::string_view::npos ||
	       (name.size() >= 2 && name[1] == ':');
}

std::string LastComponent(const std::string_view name)
{
	auto last = name;
	if (const auto slash = last.find_last_of("/\\");
	    slash != std::string_view::npos) {
		last.remove_prefix(slash + 1);
	} else if (last.size() >= 2 && last[1] == ':') {
		last.remove_prefix(2);
	}
	return std::string(last);
}

// ASCII case only, over the host's own spelling of names: a cue's bytes
// are whatever its ripper wrote, not UTF-8.
template <typename String>
bool EqualsIgnoringAsciiCase(const String& a, const String& b)
{
	const auto fold = [](const auto c) {
		return (c >= 'a' && c <= 'z') ? c - ('a' - 'A') : c;
	};
	return a.size() == b.size() &&
	       std::equal(a.begin(), a.end(), b.begin(), [&](const auto x, const auto y) {
		       return fold(x) == fold(y);
	       });
}

// The canonical path of candidate when it is a regular file, not a link,
// and a direct child of dir.
std::optional<fs::path> PlainChild(const fs::path& candidate, const fs::path& dir)
{
	std::error_code ec = {};
	const auto status  = fs::symlink_status(candidate, ec);
	if (ec || !fs::is_regular_file(status)) {
		return {};
	}
	auto canonical = fs::canonical(candidate, ec);
	if (ec || canonical.parent_path() != dir) {
		return {};
	}
	return canonical;
}

} // namespace

std::optional<Resolved> Resolve(const fs::path& image,
                                const std::string_view name, std::string* why)
{
	std::error_code ec = {};
	const auto dir     = fs::canonical(image, ec).parent_path();
	if (ec) {
		return Refuse(why, "the image's directory cannot be resolved");
	}
	const auto last = LastComponent(name);
	if (last.empty() || last.front() == '.') {
		return Refuse(why, "'" + std::string(name) + "' is empty or dot-named");
	}
	if (DosNames::IsWindowsReservedDeviceName(last)) {
		return Refuse(why, "'" + last + "' is a device name");
	}

	if (!HasDirectoryPart(name)) {
		if (auto exact = PlainChild(dir / fs::path(last), dir)) {
			return Resolved{std::move(*exact), false};
		}
	}

	const auto want              = fs::path(last).native();
	std::vector<fs::path> folded = {};
	auto it                      = fs::directory_iterator(dir, ec);
	for (; !ec && it != fs::directory_iterator(); it.increment(ec)) {
		const auto file = it->path().filename().native();
		if (file == want) {
			folded.assign(1, it->path());
			break;
		}
		if (EqualsIgnoringAsciiCase(file, want)) {
			folded.push_back(it->path());
		}
	}
	if (ec) {
		return Refuse(why, "the image's directory cannot be listed");
	}
	if (folded.empty()) {
		return Refuse(why, "no '" + last + "' beside the image");
	}
	if (folded.size() > 1) {
		return Refuse(why, "'" + last + "' is ambiguous beside the image");
	}
	auto plain = PlainChild(folded.front(), dir);
	if (!plain) {
		return Refuse(why, "'" + last + "' beside the image is not a plain file");
	}
	return Resolved{std::move(*plain), true};
}

} // namespace CdCompanion
