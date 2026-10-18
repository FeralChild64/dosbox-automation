// SPDX-FileCopyrightText:  2021-2026 The DOSBox Staging Team
// SPDX-FileCopyrightText:  2002-2021 The DOSBox Team
// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 dosbox-automation contributors

#ifndef DOSBOX_DOS_NAMES_H
#define DOSBOX_DOS_NAMES_H

#include <string>
#include <string_view>

// Pure name logic with no engine state, so the archive reader library and
// its helper process link it without the DOS layer.

struct SfnBasis {
	char name[9];
	char ext[4];
	bool lossy;   // a character was replaced with underscore
	bool not_8x3; // the original name does not fit 8.3 unmodified
};

SfnBasis sfn_clean_basis(const char* input);

std::string generate_8x3(const char* lfn, const unsigned int num,
                         const bool start = false);

constexpr bool is_special_character(const char c)
{
	constexpr auto special_characters = std::string_view("\"+=,;:<>[]|?*/\\");
	return special_characters.find(c) != std::string_view::npos;
}

namespace DosNames {

bool IsWindowsReservedDeviceName(const std::string& path_component);

// DOS reads "NAME." as NAME: the empty extension of each component of a
// backslash path goes, a dot elsewhere stays (G8/G9, E4).
std::string WithoutEmptyExtensions(std::string_view dos_path);

} // namespace DosNames

#endif // DOSBOX_DOS_NAMES_H
