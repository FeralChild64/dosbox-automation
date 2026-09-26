// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//

#ifndef DOSBOX_TEST_TEMP_DIR_H
#define DOSBOX_TEST_TEMP_DIR_H

#include <cstdint>
#include <filesystem>
#include <random>
#include <string>
#include <string_view>
#include <system_error>

namespace TestTempDir {

// %TEMP% is on the Windows mount denylist and cannot be whitelisted, so
// fixtures that get mounted live in the build tree there (ada-925x).
inline std::filesystem::path Base()
{
#if defined(WIN32)
#ifndef DOSBOX_TEST_SCRATCH_DIR
#error "tests/CMakeLists.txt must define DOSBOX_TEST_SCRATCH_DIR on Windows"
#endif
	return std::filesystem::path(DOSBOX_TEST_SCRATCH_DIR);
#else
	return std::filesystem::temp_directory_path();
#endif
}

// mkdtemp does not exist on Windows; create_directory fails on an existing
// path, so a random name plus creation check gives the same no-clobber
// guarantee. Returns the canonical path, empty on failure.
inline std::filesystem::path MakeUnique(std::string_view prefix,
                                        const std::filesystem::path& base = Base())
{
	if (prefix.find_first_of("/\\:") != std::string_view::npos ||
	    prefix.find("..") != std::string_view::npos) {
		return {};
	}

	std::error_code ec = {};
	std::filesystem::create_directories(base, ec);

	std::random_device rd = {};
	auto dist             = std::uniform_int_distribution<uint64_t>();
	for (int attempt = 0; attempt < 16; ++attempt) {
		const auto candidate = base / (std::string(prefix) +
		                               std::to_string(dist(rd)));
		if (std::filesystem::create_directory(candidate, ec) && !ec) {
			std::filesystem::permissions(candidate,
			                             std::filesystem::perms::owner_all,
			                             ec);
			return std::filesystem::canonical(candidate, ec);
		}
	}
	return {};
}

} // namespace TestTempDir

#endif // DOSBOX_TEST_TEMP_DIR_H
