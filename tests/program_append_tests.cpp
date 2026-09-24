// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//

#include "dos/programs/append.h"

#include <gtest/gtest.h>

#include <string>
#include <variant>
#include <vector>

using namespace AppendCommand;

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

TEST(AppendParse, no_arguments_displays_the_list)
{
	const auto request = parse_ok({});
	EXPECT_EQ(request.action, Action::Display);
	EXPECT_FALSE(request.store_in_env);
	EXPECT_FALSE(request.search_exec.has_value());
	EXPECT_FALSE(request.search_with_path.has_value());
}

TEST(AppendParse, path_list_sets_the_list)
{
	const auto request = parse_ok({"b:\\letters;a:\\reports"});
	EXPECT_EQ(request.action, Action::SetList);
	EXPECT_EQ(request.list, "b:\\letters;a:\\reports");
}

TEST(AppendParse, lone_semicolon_clears_the_list)
{
	EXPECT_EQ(parse_ok({";"}).action, Action::ClearList);
}

TEST(AppendParse, switches_are_not_part_of_the_list)
{
	const auto request = parse_ok({"/x:on", "c:\\data"});
	EXPECT_EQ(request.action, Action::SetList);
	EXPECT_EQ(request.list, "c:\\data");
	EXPECT_EQ(request.search_exec, true);

	const auto trailing = parse_ok({"c:\\data", "/path:off"});
	EXPECT_EQ(trailing.list, "c:\\data");
	EXPECT_EQ(trailing.search_with_path, false);
}

TEST(AppendParse, every_documented_switch_is_accepted)
{
	EXPECT_EQ(parse_ok({"/x"}).search_exec, true);
	EXPECT_EQ(parse_ok({"/x:on"}).search_exec, true);
	EXPECT_EQ(parse_ok({"/x:off"}).search_exec, false);
	EXPECT_EQ(parse_ok({"/path:on"}).search_with_path, true);
	EXPECT_EQ(parse_ok({"/path:off"}).search_with_path, false);
	EXPECT_TRUE(parse_ok({"/e"}).store_in_env);
}

TEST(AppendParse, switch_only_line_changes_flags_without_display)
{
	EXPECT_EQ(parse_ok({"/x:off"}).action, Action::SwitchesOnly);
	EXPECT_EQ(parse_ok({"/e"}).action, Action::SwitchesOnly);
}

TEST(AppendParse, switches_match_case_insensitively)
{
	EXPECT_EQ(parse_ok({"/X:OFF"}).search_exec, false);
	EXPECT_EQ(parse_ok({"/Path:Off"}).search_with_path, false);
	EXPECT_TRUE(parse_ok({"/E"}).store_in_env);
}

TEST(AppendParse, unknown_switch_is_rejected_by_name)
{
	const auto error = parse_error({"/q", "c:\\data"});
	EXPECT_EQ(error.type, ErrorType::IllegalSwitch);
	EXPECT_EQ(error.argument, "/q");

	EXPECT_EQ(parse_error({"/x:maybe"}).type, ErrorType::IllegalSwitch);
	EXPECT_EQ(parse_error({"/path"}).type, ErrorType::IllegalSwitch);
	EXPECT_EQ(parse_error({"/"}).type, ErrorType::IllegalSwitch);
}

// Reference p364: "You cannot specify /e and [drive:]path on the same
// command line."
TEST(AppendParse, env_switch_with_a_path_is_rejected)
{
	EXPECT_EQ(parse_error({"/e", "c:\\data"}).type, ErrorType::EnvWithPath);
	EXPECT_EQ(parse_error({"c:\\data", "/e"}).type, ErrorType::EnvWithPath);
	EXPECT_EQ(parse_error({"/e", ";"}).type, ErrorType::EnvWithPath);
}

TEST(AppendParse, contradicting_switches_are_rejected)
{
	EXPECT_EQ(parse_error({"/x:on", "/x:off"}).type,
	          ErrorType::ConflictingSwitches);
	EXPECT_EQ(parse_error({"/path:on", "/path:off"}).type,
	          ErrorType::ConflictingSwitches);
}

TEST(AppendParse, a_second_path_parameter_is_rejected)
{
	const auto error = parse_error({"c:\\data", "d:\\more"});
	EXPECT_EQ(error.type, ErrorType::TooManyParameters);
	EXPECT_EQ(error.argument, "d:\\more");
}

TEST(AppendParse, oversized_path_list_is_rejected)
{
	const std::string long_list(MaxListLength + 1, 'a');
	EXPECT_EQ(parse_error({long_list}).type, ErrorType::ListTooLong);

	const std::string fitting_list(MaxListLength, 'a');
	EXPECT_EQ(parse_ok({fitting_list}).list, fitting_list);
}

TEST(AppendApply, env_switch_is_only_allowed_on_first_use)
{
	State state = {};
	EXPECT_FALSE(Apply(state, parse_ok({"/e"})).has_value());
	EXPECT_TRUE(state.store_in_env);

	State used = {};
	EXPECT_FALSE(Apply(used, parse_ok({"c:\\data"})).has_value());
	EXPECT_EQ(Apply(used, parse_ok({"/e"})), ApplyError::EnvNotFirstUse);
	EXPECT_FALSE(used.store_in_env);
}

// Reference p363: "If you want to specify x:on, you must do it the first
// time you use append after starting your system. After that, you can
// switch between x:on and x:off."
TEST(AppendApply, exec_search_can_only_be_enabled_on_first_use)
{
	State late = {};
	EXPECT_FALSE(Apply(late, parse_ok({"c:\\data"})).has_value());
	EXPECT_EQ(Apply(late, parse_ok({"/x:on"})), ApplyError::ExecNotFirstUse);
	EXPECT_FALSE(late.search_exec);

	State early = {};
	EXPECT_FALSE(Apply(early, parse_ok({"/x:on"})).has_value());
	EXPECT_FALSE(Apply(early, parse_ok({"/x:off"})).has_value());
	EXPECT_FALSE(early.search_exec);
	EXPECT_FALSE(Apply(early, parse_ok({"/x"})).has_value());
	EXPECT_TRUE(early.search_exec);
}

TEST(AppendApply, a_rejected_request_leaves_the_state_unchanged)
{
	State state = {};
	EXPECT_FALSE(Apply(state, parse_ok({"c:\\data"})).has_value());
	const State before = state;
	EXPECT_EQ(Apply(state, parse_ok({"/x:on", "d:\\other"})),
	          ApplyError::ExecNotFirstUse);
	EXPECT_EQ(state, before);
}

TEST(AppendApply, new_list_replaces_the_old_one)
{
	State state = {};
	EXPECT_FALSE(Apply(state, parse_ok({"c:\\one"})).has_value());
	EXPECT_FALSE(Apply(state, parse_ok({"c:\\two;c:\\three"})).has_value());
	EXPECT_EQ(state.list, "c:\\two;c:\\three");
}

TEST(AppendApply, clear_empties_the_list)
{
	State state = {};
	EXPECT_FALSE(Apply(state, parse_ok({"c:\\data"})).has_value());
	EXPECT_FALSE(Apply(state, parse_ok({";"})).has_value());
	EXPECT_TRUE(state.list.empty());
}

TEST(AppendApply, path_search_defaults_on_and_toggles)
{
	State state = {};
	EXPECT_TRUE(state.search_with_path);
	EXPECT_FALSE(Apply(state, parse_ok({"/path:off"})).has_value());
	EXPECT_FALSE(state.search_with_path);
	EXPECT_FALSE(Apply(state, parse_ok({"/path:on"})).has_value());
	EXPECT_TRUE(state.search_with_path);
}

// Inference, not from the manual: APPEND.EXE goes resident on its first
// run whatever the arguments, so a bare display already uses up /e.
TEST(AppendApply, display_counts_as_first_use)
{
	State state = {};
	EXPECT_FALSE(Apply(state, parse_ok({})).has_value());
	EXPECT_EQ(Apply(state, parse_ok({"/e"})), ApplyError::EnvNotFirstUse);
}
