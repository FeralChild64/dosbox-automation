// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//
#include "dos/archive/archive_toml.h"
#include "dos/archive/random_name.h"

#include "dos/archive/descriptor.h"
#include "dos/archive/exclusive_file.h"
#include "utils/checks.h"

#include <algorithm>
#include <cstdio>
#include <random>
#include <system_error>

#if defined(WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

CHECK_NARROWING();

namespace ArchiveToml {

namespace {

bool IsKeyByte(const char c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
	       (c >= '0' && c <= '9') || c == '_' || c == '-';
}

bool IsSpace(const char c)
{
	return c == ' ' || c == '\t';
}

std::string_view TrimRight(std::string_view s)
{
	while (!s.empty() && (IsSpace(s.back()) || s.back() == '\r')) {
		s.remove_suffix(1);
	}
	return s;
}

std::string_view TrimLeft(std::string_view s)
{
	while (!s.empty() && IsSpace(s.front())) {
		s.remove_prefix(1);
	}
	return s;
}

void AppendUtf8(std::string& out, const uint32_t cp)
{
	if (cp < 0x80) {
		out.push_back(static_cast<char>(cp));
	} else if (cp < 0x800) {
		out.push_back(static_cast<char>(0xc0 | (cp >> 6)));
		out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
	} else if (cp < 0x10000) {
		out.push_back(static_cast<char>(0xe0 | (cp >> 12)));
		out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
		out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
	} else {
		out.push_back(static_cast<char>(0xf0 | (cp >> 18)));
		out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3f)));
		out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
		out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
	}
}

std::optional<uint32_t> HexValue(std::string_view digits)
{
	uint32_t value = 0;
	for (const auto c : digits) {
		value <<= 4;
		if (c >= '0' && c <= '9') {
			value |= static_cast<uint32_t>(c - '0');
		} else if (c >= 'a' && c <= 'f') {
			value |= static_cast<uint32_t>(c - 'a' + 10);
		} else if (c >= 'A' && c <= 'F') {
			value |= static_cast<uint32_t>(c - 'A' + 10);
		} else {
			return {};
		}
	}
	return value;
}

// Parses a basic string starting at the opening quote; rest receives
// what follows the closing quote.
bool ParseString(std::string_view in, std::string& out, std::string_view& rest,
                 std::string& error)
{
	out.clear();
	size_t i = 1;
	while (i < in.size()) {
		const auto c = in[i];
		if (c == '"') {
			rest = in.substr(i + 1);
			return true;
		}
		if (static_cast<unsigned char>(c) < 0x20 && c != '\t') {
			error = "control byte inside a string";
			return false;
		}
		if (c != '\\') {
			out.push_back(c);
			++i;
		} else {
			if (i + 1 >= in.size()) {
				error = "escape at the end of the line";
				return false;
			}
			const auto e = in[i + 1];
			i += 2;
			switch (e) {
			case '"': out.push_back('"'); break;
			case '\\': out.push_back('\\'); break;
			case 'n': out.push_back('\n'); break;
			case 'r': out.push_back('\r'); break;
			case 't': out.push_back('\t'); break;
			case 'u':
			case 'U': {
				const size_t digits = e == 'u' ? 4 : 8;
				if (i + digits > in.size()) {
					error = "short unicode escape";
					return false;
				}
				const auto cp = HexValue(in.substr(i, digits));
				if (!cp || *cp > 0x10ffff ||
				    (*cp >= 0xd800 && *cp <= 0xdfff)) {
					error = "bad unicode escape";
					return false;
				}
				AppendUtf8(out, *cp);
				i += digits;
				break;
			}
			default: error = "unknown escape"; return false;
			}
		}
		if (out.size() > MaxValueBytes) {
			error = "string value too long";
			return false;
		}
	}
	error = "unterminated string";
	return false;
}

bool ParseInteger(std::string_view text, int64_t& out, std::string& error)
{
	if (text.empty()) {
		error = "empty integer";
		return false;
	}
	size_t i            = 0;
	const bool negative = text[0] == '-';
	if (negative) {
		i = 1;
	}
	if (i >= text.size()) {
		error = "sign without digits";
		return false;
	}
	// Accumulate negatively so INT64_MIN parses.
	int64_t value = 0;
	for (; i < text.size(); ++i) {
		const auto c = text[i];
		if (c < '0' || c > '9') {
			error = "not an integer";
			return false;
		}
		const int digit = c - '0';
		if (value < (INT64_MIN + digit) / 10) {
			error = "integer out of range";
			return false;
		}
		value = value * 10 - digit;
	}
	if (!negative) {
		if (value == INT64_MIN) {
			error = "integer out of range";
			return false;
		}
		value = -value;
	}
	out = value;
	return true;
}

// Only whitespace or a comment may follow a value or a header.
bool OnlyCommentFollows(std::string_view rest)
{
	rest = TrimLeft(rest);
	return rest.empty() || rest.front() == '#';
}

bool ParseValue(std::string_view text, Value& value, std::string& error)
{
	text = TrimLeft(text);
	if (text.empty()) {
		error = "missing value";
		return false;
	}
	if (text.front() == '"') {
		std::string_view rest = {};
		if (!ParseString(text, value.text, rest, error)) {
			return false;
		}
		if (!OnlyCommentFollows(rest)) {
			error = "text after the string";
			return false;
		}
		value.kind = Value::Kind::String;
		return true;
	}
	// A bare token ends at whitespace or a comment.
	size_t end = 0;
	while (end < text.size() && !IsSpace(text[end]) && text[end] != '#') {
		++end;
	}
	const auto token = text.substr(0, end);
	if (!OnlyCommentFollows(text.substr(end))) {
		error = "text after the value";
		return false;
	}
	if (token == "true" || token == "false") {
		value.kind    = Value::Kind::Boolean;
		value.boolean = token == "true";
		return true;
	}
	if (!ParseInteger(token, value.integer, error)) {
		return false;
	}
	value.kind = Value::Kind::Integer;
	return true;
}

bool ParseHeader(std::string_view line, std::string& name, std::string& error)
{
	// line starts with "[["
	const auto close = line.find("]]");
	if (close == std::string_view::npos) {
		error = "unterminated table header";
		return false;
	}
	const auto key = line.substr(2, close - 2);
	if (key.empty() || key.size() > MaxKeyBytes ||
	    !std::all_of(key.begin(), key.end(), IsKeyByte)) {
		error = "bad table name";
		return false;
	}
	if (!OnlyCommentFollows(line.substr(close + 2))) {
		error = "text after the table header";
		return false;
	}
	name = std::string(key);
	return true;
}

void AppendQuoted(std::string& out, std::string_view text)
{
	out.push_back('"');
	for (const auto c : text) {
		const auto byte = static_cast<unsigned char>(c);
		switch (c) {
		case '"': out += "\\\""; break;
		case '\\': out += "\\\\"; break;
		case '\n': out += "\\n"; break;
		case '\r': out += "\\r"; break;
		case '\t': out += "\\t"; break;
		default:
			if (byte < 0x20 || byte == 0x7f) {
				char buf[8] = {};
				std::snprintf(buf, sizeof(buf), "\\u%04X", byte);
				out += buf;
			} else {
				out.push_back(c);
			}
		}
	}
	out.push_back('"');
}

void AppendTable(std::string& out, const Table& table)
{
	for (const auto& [key, value] : table.pairs) {
		out += key;
		out += " = ";
		switch (value.kind) {
		case Value::Kind::String: AppendQuoted(out, value.text); break;
		case Value::Kind::Integer:
			out += std::to_string(value.integer);
			break;
		case Value::Kind::Boolean:
			out += value.boolean ? "true" : "false";
			break;
		}
		out.push_back('\n');
	}
}

} // namespace

const Value* Table::Find(const std::string_view key) const
{
	for (const auto& pair : pairs) {
		if (pair.first == key) {
			return &pair.second;
		}
	}
	return nullptr;
}

std::optional<std::string> Table::String(const std::string_view key) const
{
	const auto* v = Find(key);
	if (!v || v->kind != Value::Kind::String) {
		return {};
	}
	return v->text;
}

std::optional<int64_t> Table::Integer(const std::string_view key) const
{
	const auto* v = Find(key);
	if (!v || v->kind != Value::Kind::Integer) {
		return {};
	}
	return v->integer;
}

std::optional<bool> Table::Boolean(const std::string_view key) const
{
	const auto* v = Find(key);
	if (!v || v->kind != Value::Kind::Boolean) {
		return {};
	}
	return v->boolean;
}

namespace {

Value& SlotFor(Table& table, const std::string_view key)
{
	for (auto& pair : table.pairs) {
		if (pair.first == key) {
			return pair.second;
		}
	}
	table.pairs.emplace_back(std::string(key), Value{});
	return table.pairs.back().second;
}

} // namespace

void Table::Set(const std::string_view key, const std::string_view text)
{
	auto& v = SlotFor(*this, key);
	v       = Value{};
	v.kind  = Value::Kind::String;
	v.text  = std::string(text);
}

void Table::Set(const std::string_view key, const char* text)
{
	Set(key, std::string_view(text));
}

void Table::Set(const std::string_view key, const int64_t integer)
{
	auto& v   = SlotFor(*this, key);
	v         = Value{};
	v.kind    = Value::Kind::Integer;
	v.integer = integer;
}

void Table::Set(const std::string_view key, const bool boolean)
{
	auto& v   = SlotFor(*this, key);
	v         = Value{};
	v.kind    = Value::Kind::Boolean;
	v.boolean = boolean;
}

std::vector<const Table*> Document::Array(const std::string_view name) const
{
	std::vector<const Table*> out = {};
	for (const auto& [array_name, table] : arrays) {
		if (array_name == name) {
			out.push_back(&table);
		}
	}
	return out;
}

std::optional<Document> Parse(const std::string_view text, std::string* error)
{
	auto fail = [&](const std::string& what, const int line_number) {
		if (error) {
			*error = "line " + std::to_string(line_number) + ": " + what;
		}
		return std::optional<Document>{};
	};
	if (text.size() > MaxDocumentBytes) {
		return fail("document too large", 0);
	}
	Document doc    = {};
	Table* current  = &doc.root;
	int line_number = 0;
	size_t pos      = 0;
	while (pos <= text.size()) {
		if (pos == text.size()) {
			break;
		}
		++line_number;
		if (line_number > MaxDocumentLines) {
			return fail("too many lines", line_number);
		}
		auto end = text.find('\n', pos);
		if (end == std::string_view::npos) {
			end = text.size();
		}
		const auto raw  = text.substr(pos, end - pos);
		pos             = end + 1;
		const auto line = TrimRight(TrimLeft(raw));
		if (line.empty() || line.front() == '#') {
			continue;
		}
		if (line.starts_with("[[")) {
			auto name = std::string();
			auto why  = std::string();
			if (!ParseHeader(line, name, why)) {
				return fail(why, line_number);
			}
			doc.arrays.emplace_back(name, Table{});
			current = &doc.arrays.back().second;
			continue;
		}
		if (line.front() == '[') {
			return fail("plain tables are not used here", line_number);
		}
		size_t key_end = 0;
		while (key_end < line.size() && IsKeyByte(line[key_end])) {
			++key_end;
		}
		if (key_end == 0 || key_end > MaxKeyBytes) {
			return fail("bad key", line_number);
		}
		const auto key = line.substr(0, key_end);
		auto rest      = TrimLeft(line.substr(key_end));
		if (rest.empty() || rest.front() != '=') {
			return fail("expected '=' after the key", line_number);
		}
		rest.remove_prefix(1);
		if (current->Find(key)) {
			return fail("duplicate key", line_number);
		}
		Value value = {};
		auto why    = std::string();
		if (!ParseValue(rest, value, why)) {
			return fail(why, line_number);
		}
		current->pairs.emplace_back(std::string(key), std::move(value));
	}
	return doc;
}

std::string Serialize(const Document& document)
{
	auto out = std::string();
	AppendTable(out, document.root);
	for (const auto& [name, table] : document.arrays) {
		out += "\n[[";
		out += name;
		out += "]]\n";
		AppendTable(out, table);
	}
	return out;
}

std::optional<Document> ReadFile(const std::filesystem::path& path, std::string* error)
{
	const auto fail = [&](const char* why) -> std::optional<Document> {
		if (error) {
			*error = path.string() + ": " + why;
		}
		return {};
	};
	const auto fd = ArchiveMount::OpenRegularFileNoFollow(path);
	if (fd < 0) {
		return fail("not a regular file");
	}
	ArchiveMount::StatPair st = {};
	if (!ArchiveMount::StatDescriptor(fd, st) || st.size < 0 ||
	    st.size > static_cast<int64_t>(MaxDocumentBytes)) {
		ArchiveMount::CloseDescriptor(fd);
		return fail("too large to be ours");
	}
	auto text   = std::string(static_cast<size_t>(st.size), '\0');
	size_t done = 0;
	while (done < text.size()) {
		const auto want = static_cast<unsigned>(text.size() - done);
#if defined(WIN32)
		const auto got = _read(fd, text.data() + done, want);
#else
		const auto got = read(fd, text.data() + done, want);
#endif
		if (got <= 0) {
			break;
		}
		done += static_cast<size_t>(got);
	}
	ArchiveMount::CloseDescriptor(fd);
	if (done != text.size()) {
		return fail("short read");
	}
	auto why = std::string();
	auto doc = Parse(text, &why);
	if (!doc && error) {
		*error = path.string() + ": " + why;
	}
	return doc;
}

bool WriteAtomically(const std::filesystem::path& path,
                     const Document& document, std::string* error)
{
	const auto text = Serialize(document);
	const auto temp = path.parent_path() / (path.filename().string() + "." +
	                                        ArchiveMount::RandomHex(4) + ".tmp");
	FILE* f         = ArchiveMount::OpenExclusive(temp);
	if (!f) {
		if (error) {
			*error = "cannot create " + temp.string();
		}
		return false;
	}
	const auto written = std::fwrite(text.data(), 1, text.size(), f) ==
	                     text.size();
	const auto closed  = std::fclose(f) == 0;
	std::error_code ec = {};
	if (!written || !closed) {
		std::filesystem::remove(temp, ec);
		if (error) {
			*error = "cannot write " + temp.string();
		}
		return false;
	}
	std::filesystem::rename(temp, path, ec);
	if (ec) {
		std::filesystem::remove(temp, ec);
		if (error) {
			*error = "cannot rename into " + path.string();
		}
		return false;
	}
	return true;
}

} // namespace ArchiveToml
