// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//

#include "dos/programs/join.h"

#include <gtest/gtest.h>

#include <string>
#include <variant>
#include <vector>

using namespace JoinCommand;

namespace {

Request parse_ok(const std::vector<std::string>& args)
{
	const auto result = ParseArguments(args);
	if (const auto error = std::get_if<Error>(&result)) {
		ADD_FAILURE() << "unexpected parse error, argument '"
		              << error->argument << "'";
		return {};
	}
	return std::get<Request>(result);
}

Error parse_error(const std::vector<std::string>& args)
{
	const auto result = ParseArguments(args);
	if (std::holds_alternative<Request>(result)) {
		ADD_FAILURE() << "parse succeeded, expected an error";
		return {};
	}
	return std::get<Error>(result);
}

} // namespace

TEST(JoinParse, no_arguments_lists_joins)
{
	EXPECT_EQ(parse_ok({}).action, Action::List);
}

TEST(JoinParse, drive_and_path_requests_a_join)
{
	const auto request = parse_ok({"d:", "c:\\sales\\october"});
	EXPECT_EQ(request.action, Action::Join);
	EXPECT_EQ(request.drive, 'D');
	EXPECT_EQ(request.target, "c:\\sales\\october");
}

TEST(JoinParse, target_drive_letter_is_optional)
{
	const auto request = parse_ok({"a:", "\\floppy"});
	EXPECT_EQ(request.action, Action::Join);
	EXPECT_EQ(request.target, "\\floppy");
}

TEST(JoinParse, drive_with_d_switch_disconnects)
{
	const auto request = parse_ok({"d:", "/d"});
	EXPECT_EQ(request.action, Action::Disconnect);
	EXPECT_EQ(request.drive, 'D');

	EXPECT_EQ(parse_ok({"/D", "e:"}).action, Action::Disconnect);
}

TEST(JoinParse, drive_letter_is_case_insensitive)
{
	EXPECT_EQ(parse_ok({"e:", "c:\\x"}).drive, 'E');
}

TEST(JoinParse, drive_without_target_or_switch_is_rejected)
{
	EXPECT_EQ(parse_error({"d:"}).type, ErrorType::MissingParameter);
}

TEST(JoinParse, d_switch_without_drive_is_rejected)
{
	EXPECT_EQ(parse_error({"/d"}).type, ErrorType::MissingParameter);
}

TEST(JoinParse, d_switch_with_a_target_is_rejected)
{
	EXPECT_EQ(parse_error({"d:", "c:\\x", "/d"}).type,
	          ErrorType::TooManyParameters);
}

TEST(JoinParse, malformed_drive_is_rejected)
{
	for (const auto& bad : {"d", "dd:", "1:", ":", "d:\\x", "\xE4:"}) {
		const auto error = parse_error({bad, "c:\\x"});
		EXPECT_EQ(error.type, ErrorType::InvalidDrive) << bad;
		EXPECT_EQ(error.argument, bad);
	}
}

// Reference p511: the path "must also be a directory other than the root
// directory."
TEST(JoinParse, root_directory_target_is_rejected)
{
	for (const auto& root : {"c:\\", "\\", "c:/"}) {
		EXPECT_EQ(parse_error({"d:", root}).type, ErrorType::RootTarget)
		        << root;
	}
}

TEST(JoinParse, bare_drive_as_target_is_rejected)
{
	EXPECT_EQ(parse_error({"d:", "c:"}).type, ErrorType::MissingParameter);
}

TEST(JoinParse, joining_a_drive_into_itself_is_rejected)
{
	EXPECT_EQ(parse_error({"c:", "c:\\inner"}).type, ErrorType::SameDrive);
	EXPECT_EQ(parse_error({"C:", "c:\\inner"}).type, ErrorType::SameDrive);
}

TEST(JoinParse, unknown_switch_is_rejected_by_name)
{
	const auto error = parse_error({"d:", "c:\\x", "/q"});
	EXPECT_EQ(error.type, ErrorType::IllegalSwitch);
	EXPECT_EQ(error.argument, "/q");
}

TEST(JoinParse, extra_parameter_is_rejected)
{
	const auto error = parse_error({"d:", "c:\\x", "c:\\y"});
	EXPECT_EQ(error.type, ErrorType::TooManyParameters);
	EXPECT_EQ(error.argument, "c:\\y");
}
