// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//
#include "dos/archive/random_name.h"

#include "utils/checks.h"

#include <cassert>
#include <random>

CHECK_NARROWING();

namespace ArchiveMount {

std::string RandomHex(const int words)
{
	assert(words > 0);
	auto device = std::random_device{};
	auto out    = std::string();
	for (auto i = 0; i < words; ++i) {
		const auto word = device();
		for (auto shift = 0; shift < 32; shift += 4) {
			out.push_back("0123456789abcdef"[(word >> shift) & 0xf]);
		}
	}
	return out;
}

} // namespace ArchiveMount
