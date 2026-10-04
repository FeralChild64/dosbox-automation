// SPDX-FileCopyrightText:  2021-2026 The DOSBox Staging Team
// SPDX-FileCopyrightText:  2002-2021 The DOSBox Team
// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 dosbox-automation contributors

#include "dos/dos_names.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>

static bool is_sfn_illegal(const unsigned char c)
{
	if (c < 0x20) {
		return true;
	}
	if (c == 0x7F) {
		return true;
	}
	return is_special_character(static_cast<char>(c));
}

SfnBasis sfn_clean_basis(const char* input)
{
	SfnBasis result = {};
	result.name[0]  = '\0';
	result.ext[0]   = '\0';
	result.lossy    = false;
	result.not_8x3  = false;

	if (!input || !input[0]) {
		result.name[0] = '_';
		result.name[1] = '\0';
		result.lossy   = true;
		result.not_8x3 = true;
		return result;
	}

	// "." and ".." pass through as directory entries
	if (std::strcmp(input, ".") == 0 || std::strcmp(input, "..") == 0) {
		std::strncpy(result.name, input, 8);
		result.name[8] = '\0';
		return result;
	}

	const char* p = input;

	while (*p == ' ' || *p == '.') {
		++p;
	}
	if (p != input) {
		result.not_8x3 = true;
	}

	const char* end = input + std::strlen(input);
	while (end > p && (end[-1] == ' ' || end[-1] == '.')) {
		--end;
	}
	if (end != input + std::strlen(input)) {
		result.not_8x3 = true;
	}

	if (p >= end) {
		result.name[0] = '_';
		result.name[1] = '\0';
		result.lossy   = true;
		result.not_8x3 = true;
		return result;
	}

	const char* first_dot = nullptr;
	const char* last_dot  = nullptr;
	for (const char* s = p; s < end; ++s) {
		if (*s == '.') {
			if (!first_dot) {
				first_dot = s;
			}
			last_dot = s;
		}
	}

	if (first_dot && last_dot && first_dot != last_dot) {
		result.not_8x3 = true;
	}

	const char* prim_end  = first_dot ? first_dot : end;
	const char* ext_start = last_dot ? last_dot + 1 : nullptr;
	const char* ext_end   = last_dot ? end : nullptr;

	// Scan the full input for illegal chars and spaces before the
	// truncating copy (FATGEN103 replaces over the whole name first).
	for (const char* s = p; s < prim_end; ++s) {
		unsigned char c = static_cast<unsigned char>(*s);
		if (c == ' ') {
			result.not_8x3 = true;
		} else if (is_sfn_illegal(c)) {
			result.lossy   = true;
			result.not_8x3 = true;
		}
	}
	if (ext_start) {
		for (const char* s = ext_start; s < ext_end; ++s) {
			unsigned char c = static_cast<unsigned char>(*s);
			if (c == ' ') {
				result.not_8x3 = true;
			} else if (is_sfn_illegal(c)) {
				result.lossy   = true;
				result.not_8x3 = true;
			}
		}
	}

	int prim_count = 0;
	for (const char* s = p; s < prim_end; ++s) {
		if (*s != ' ') {
			++prim_count;
		}
	}
	if (prim_count > 8) {
		result.not_8x3 = true;
	}

	int ext_count = 0;
	if (ext_start) {
		for (const char* s = ext_start; s < ext_end; ++s) {
			if (*s != ' ') {
				++ext_count;
			}
		}
		if (ext_count > 3) {
			result.not_8x3 = true;
		}
	}

	int ni = 0;
	for (const char* s = p; s < prim_end && ni < 8; ++s) {
		unsigned char c = static_cast<unsigned char>(*s);
		if (c == ' ') {
			continue;
		}
		if (is_sfn_illegal(c)) {
			result.name[ni++] = '_';
		} else if (c >= 'a' && c <= 'z') {
			result.name[ni++] = static_cast<char>(c - 32);
		} else {
			result.name[ni++] = static_cast<char>(c);
		}
	}
	result.name[ni] = '\0';

	if (ext_start) {
		int ei = 0;
		for (const char* s = ext_start; s < ext_end && ei < 3; ++s) {
			unsigned char c = static_cast<unsigned char>(*s);
			if (c == ' ') {
				continue;
			}
			if (is_sfn_illegal(c)) {
				result.ext[ei++] = '_';
			} else if (c >= 'a' && c <= 'z') {
				result.ext[ei++] = static_cast<char>(c - 32);
			} else {
				result.ext[ei++] = static_cast<char>(c);
			}
		}
		result.ext[ei] = '\0';
	}

	if (result.name[0] == '\0') {
		result.name[0] = '_';
		result.name[1] = '\0';
		result.lossy   = true;
		result.not_8x3 = true;
	}

	return result;
}

/* Generate 8.3 names from LFNs, with tilde usage (from ~1 to ~999999). */
std::string generate_8x3(const char* lfn, const unsigned int num, const bool /*start*/)
{
	constexpr unsigned int tilde_limit = 1000000;
	if (num == 0 || num >= tilde_limit) {
		return "";
	}

	const auto basis = sfn_clean_basis(lfn);

	const unsigned int tilde_pos = 6 - static_cast<unsigned int>(
	                                           floor(log10(num)));

	std::string result(basis.name,
	                   std::min(static_cast<size_t>(tilde_pos),
	                            std::strlen(basis.name)));
	result += '~';
	result += std::to_string(num);

	if (basis.ext[0] != '\0') {
		result += '.';
		result += basis.ext;
	}
	return result;
}

namespace DosNames {

bool IsWindowsReservedDeviceName(const std::string& path_component)
{
	// Win32 strips trailing dots and spaces before name resolution and
	// the stem before the first dot names the device, so "CON." and
	// "COM1 .txt" both reach a device (Win32 file naming rules).
	const auto end = path_component.find_last_not_of(" .");
	if (end == std::string::npos) {
		return false;
	}
	const auto trimmed = path_component.substr(0, end + 1);

	const auto dot = trimmed.find('.');
	auto stem = (dot == std::string::npos) ? trimmed : trimmed.substr(0, dot);
	const auto stem_end = stem.find_last_not_of(' ');
	if (stem_end == std::string::npos) {
		return false;
	}
	stem = stem.substr(0, stem_end + 1);

	auto upper = std::string();
	upper.reserve(stem.size());
	for (const char c : stem) {
		upper.push_back(static_cast<char>(
		        std::toupper(static_cast<unsigned char>(c))));
	}

	static constexpr std::array<const char*, 7> fixed_names = {
	        "CON", "PRN", "AUX", "NUL", "CLOCK$", "CONIN$", "CONOUT$"};
	for (const auto* name : fixed_names) {
		if (upper == name) {
			return true;
		}
	}

	if (upper.size() >= 4 &&
	    (upper.compare(0, 3, "COM") == 0 || upper.compare(0, 3, "LPT") == 0)) {
		const auto tail = upper.substr(3);
		if (tail.size() == 1 && tail[0] >= '1' && tail[0] <= '9') {
			return true;
		}
		// Superscript one/two/three count too (U+00B9/B2/B3, UTF-8)
		if (tail.size() == 2 && static_cast<unsigned char>(tail[0]) == 0xC2 &&
		    (static_cast<unsigned char>(tail[1]) == 0xB9 ||
		     static_cast<unsigned char>(tail[1]) == 0xB2 ||
		     static_cast<unsigned char>(tail[1]) == 0xB3)) {
			return true;
		}
	}
	return false;
}

} // namespace DosNames
