// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//

#include "shell/path_command.h"

#include <gtest/gtest.h>

#include <string>
#include <variant>

using namespace PathCommand;

namespace {

Request parse_ok(const std::string& line)
{
	const auto result = ParseArguments(line);
	if (std::holds_alternative<TooManyParameters>(result)) {
		ADD_FAILURE() << "unexpected 'Too many parameters' for '"
		              << line << "'";
		return {};
	}
	return std::get<Request>(result);
}

bool parse_too_many(const std::string& line)
{
	return std::holds_alternative<TooManyParameters>(ParseArguments(line));
}

} // namespace

TEST(PathParse, no_arguments_displays_the_path)
{
	EXPECT_EQ(parse_ok("").action, Action::Display);
	EXPECT_EQ(parse_ok("   ").action, Action::Display);
	EXPECT_EQ(parse_ok("\t").action, Action::Display);
}

TEST(PathParse, value_is_stored_upper_cased)
{
	const auto request = parse_ok("c:\\one;d:\\two");
	EXPECT_EQ(request.action, Action::Set);
	EXPECT_EQ(request.value, "C:\\ONE;D:\\TWO");
}

TEST(PathParse, leading_delimiters_before_the_value_are_skipped)
{
	EXPECT_EQ(parse_ok("  c:\\one").value, "C:\\ONE");
	EXPECT_EQ(parse_ok("=c:\\one").value, "C:\\ONE");
	EXPECT_EQ(parse_ok(",\tc:\\one").value, "C:\\ONE");
}

TEST(PathParse, trailing_blanks_and_tabs_are_accepted)
{
	EXPECT_EQ(parse_ok("c:\\one   ").value, "C:\\ONE");
	EXPECT_EQ(parse_ok("c:\\one\t \t").value, "C:\\ONE");
}

TEST(PathParse, lone_semicolon_clears_the_path)
{
	const auto request = parse_ok(";");
	EXPECT_EQ(request.action, Action::Set);
	EXPECT_EQ(request.value, "");
	EXPECT_EQ(parse_ok("; \t").value, "");
	EXPECT_EQ(parse_ok(" ;").value, "");
}

TEST(PathParse, text_after_the_clearing_semicolon_is_too_many_parameters)
{
	EXPECT_TRUE(parse_too_many(";x"));
	EXPECT_TRUE(parse_too_many("; c:\\one"));
	EXPECT_TRUE(parse_too_many(";;"));
}

TEST(PathParse, blank_inside_the_value_ends_it_and_the_rest_is_too_many_parameters)
{
	EXPECT_TRUE(parse_too_many("c:\\a b"));
	EXPECT_TRUE(parse_too_many("c:\\a\tb"));
}

TEST(PathParse, comma_and_equals_are_delimiters_not_path_characters)
{
	EXPECT_TRUE(parse_too_many("c:\\dos,c:\\x"));
	EXPECT_TRUE(parse_too_many("c:\\dos=c:\\x"));
}

TEST(PathParse, the_delimiter_that_ends_the_value_is_consumed)
{
	EXPECT_EQ(parse_ok("c:\\dos,").value, "C:\\DOS");
	EXPECT_EQ(parse_ok("c:\\dos= ").value, "C:\\DOS");
	EXPECT_EQ(parse_ok("c:\\dos,\t").value, "C:\\DOS");
	EXPECT_TRUE(parse_too_many("c:\\dos,,"));
}

TEST(PathParse, line_feed_is_a_delimiter)
{
	EXPECT_EQ(parse_ok("\nc:\\one").value, "C:\\ONE");
	EXPECT_EQ(parse_ok("c:\\one\n").value, "C:\\ONE");
	EXPECT_TRUE(parse_too_many("c:\\a\nb"));
}

TEST(PathParse, semicolons_inside_the_value_are_kept)
{
	EXPECT_EQ(parse_ok("c:\\a;;c:\\b;").value, "C:\\A;;C:\\B;");
}

TEST(PathParse, bytes_above_ascii_are_left_alone)
{
	const std::string value = "c:\\\x81\x9a";
	EXPECT_EQ(parse_ok(value).value, "C:\\\x81\x9a");
}

TEST(PathParse, long_values_are_kept_whole)
{
	const std::string entry(60, 'x');
	const std::string line     = entry + ";" + entry + ";" + entry;
	const std::string expected = std::string(60, 'X') + ";" +
	                             std::string(60, 'X') + ";" +
	                             std::string(60, 'X');
	EXPECT_EQ(parse_ok(line).value, expected);
}
