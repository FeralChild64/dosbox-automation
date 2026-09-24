// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//

#include "shell/path_command.h"

#include "utils/checks.h"

CHECK_NARROWING();

namespace PathCommand {

// PATH in MS-DOS 4.0 COMMAND.COM, TCMD2A.ASM: PGetarg skips the delimiters
// before the value, ';' alone clears it, otherwise each character is
// upper-cased and stored until a delimiter other than ';' ends the value;
// after that only blanks and tabs may follow (scan_white). DELIM is blank,
// tab, comma, equals, semicolon and line feed.
namespace {

bool is_blank(const char c)
{
	return c == ' ' || c == '\t';
}

bool is_delimiter(const char c)
{
	return is_blank(c) || c == ',' || c == '=' || c == ';' || c == '\n';
}

// upconv goes through the DOS country table; this covers ASCII only and
// leaves code page bytes above 127 as typed.
char upcase_ascii(const char c)
{
	return (c >= 'a' && c <= 'z') ? static_cast<char>(c - ('a' - 'A')) : c;
}

bool only_blanks_follow(const std::string_view rest)
{
	for (const auto c : rest) {
		if (!is_blank(c)) {
			return false;
		}
	}
	return true;
}

} // namespace

std::variant<TooManyParameters, Request> ParseArguments(const std::string_view line)
{
	size_t pos = 0;
	while (pos < line.size() && line[pos] != ';' && is_delimiter(line[pos])) {
		++pos;
	}
	if (pos == line.size()) {
		return Request{Action::Display, {}};
	}

	Request request = {Action::Set, {}};
	if (line[pos] == ';') {
		++pos;
	} else {
		while (pos < line.size()) {
			const auto c = line[pos];
			if (c != ';' && is_delimiter(c)) {
				// lodsb has taken the delimiter before
				// scan_white runs
				++pos;
				break;
			}
			request.value.push_back(upcase_ascii(c));
			++pos;
		}
	}
	if (!only_blanks_follow(line.substr(pos))) {
		return TooManyParameters{};
	}
	return request;
}

} // namespace PathCommand
