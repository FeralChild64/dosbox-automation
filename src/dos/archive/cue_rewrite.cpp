// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//
#include "dos/archive/cue_rewrite.h"

#include <algorithm>
#include <cctype>

namespace ArchiveMount {

namespace {

// What operator>> skips in the loader (GetCueKeyword, GetCueString).
constexpr std::string_view Blanks = " \t\v\f\r";

bool IsBlank(const char c)
{
	return Blanks.find(c) != std::string_view::npos;
}

// The first word as GetCueKeyword reads it, uppercased.
std::string Keyword(const std::string_view line, size_t& pos)
{
	while (pos < line.size() && IsBlank(line[pos])) {
		++pos;
	}
	std::string word = {};
	while (pos < line.size() && !IsBlank(line[pos])) {
		word += static_cast<char>(
		        std::toupper(static_cast<unsigned char>(line[pos])));
		++pos;
	}
	return word;
}

// GetCueString's reading: a quoted string up to the next quote, or a token.
std::optional<std::string> Name(const std::string_view line, size_t& pos)
{
	while (pos < line.size() && IsBlank(line[pos])) {
		++pos;
	}
	if (pos >= line.size()) {
		return {};
	}
	if (line[pos] == '"') {
		const auto end = line.find('"', pos + 1);
		if (end == std::string_view::npos) {
			return {};
		}
		auto name = std::string(line.substr(pos + 1, end - pos - 1));
		pos       = end + 1;
		if (pos < line.size() && !IsBlank(line[pos])) {
			return {}; // GetCueString would read a different name
		}
		return name;
	}
	const auto start = pos;
	while (pos < line.size() && !IsBlank(line[pos])) {
		++pos;
	}
	return std::string(line.substr(start, pos - start));
}

} // namespace

CueRewrite RewriteCueSheet(const std::string_view cue, const CueFileResolver& resolve)
{
	CueRewrite out = {};
	if (cue.find('\0') != std::string_view::npos) {
		out.refused = "a NUL byte in the cue sheet";
		return out;
	}
	bool any_file = false;
	size_t start  = 0;
	while (start < cue.size()) {
		auto end = cue.find('\n', start);
		if (end == std::string_view::npos) {
			end = cue.size();
		}
		auto line = cue.substr(start, end - start);
		start     = end + 1;
		if (!line.empty() && line.back() == '\r') {
			line.remove_suffix(1);
		}
		if (line.size() > MaxCueLineBytes) {
			out.refused = "a line over " +
			              std::to_string(MaxCueLineBytes) + " bytes";
			return out;
		}
		size_t pos = 0;
		if (Keyword(line, pos) != "FILE") {
			out.text.append(line);
			out.text += '\n';
			continue;
		}
		any_file        = true;
		const auto name = Name(line, pos);
		if (!name || name->empty()) {
			out.refused = "a FILE line without a name: " +
			              std::string(line);
			return out;
		}
		const auto host = resolve(*name);
		if (!host) {
			out.refused = "FILE \"" + *name +
			              "\" is not a member of the archive";
			return out;
		}
		const auto host_text = host->string();
		if (host_text.size() > MaxCueNameBytes ||
		    host_text.find_first_of("\"\r\n") != std::string::npos) {
			out.refused = "the host file for \"" + *name +
			              "\" cannot be written into a cue sheet";
			return out;
		}
		const auto indent = line.substr(0, line.find_first_not_of(" \t"));
		auto rest = std::string(line.substr(pos));
		rest.erase(0, rest.find_first_not_of(Blanks));
		auto rewritten = std::string(indent) + "FILE \"" + host_text + "\"";
		if (!rest.empty()) {
			rewritten += " " + rest;
		}
		if (rewritten.size() > MaxCueLineBytes) {
			out.refused = "the rewritten line for \"" + *name +
			              "\" is too long";
			return out;
		}
		out.text += rewritten;
		out.text += '\n';
	}
	if (!any_file) {
		out.refused = "no FILE line";
		out.text.clear();
		return out;
	}
	out.ok = true;
	return out;
}

} // namespace ArchiveMount
