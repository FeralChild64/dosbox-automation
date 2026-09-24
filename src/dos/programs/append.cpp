// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//

#include "append.h"

#include "dos/dos.h"
#include "misc/messages.h"
#include "more_output.h"
#include "utils/checks.h"
#include "utils/string_utils.h"

#include <string>

CHECK_NARROWING();

extern unsigned int result_errorcode;

namespace AppendCommand {

std::variant<Error, Request> ParseArguments(const std::vector<std::string>& args)
{
	Request request                      = {};
	std::optional<std::string> parameter = {};

	auto set_flag = [](std::optional<bool>& flag, const bool value) {
		if (flag.has_value() && *flag != value) {
			return false;
		}
		flag = value;
		return true;
	};

	for (const auto& arg : args) {
		if (arg.starts_with('/')) {
			const auto name = lowcase(arg);
			bool consistent = true;
			if (name == "/e") {
				request.store_in_env = true;
			} else if (name == "/x" || name == "/x:on") {
				consistent = set_flag(request.search_exec, true);
			} else if (name == "/x:off") {
				consistent = set_flag(request.search_exec, false);
			} else if (name == "/path:on") {
				consistent = set_flag(request.search_with_path, true);
			} else if (name == "/path:off") {
				consistent = set_flag(request.search_with_path, false);
			} else {
				return Error{ErrorType::IllegalSwitch, arg};
			}
			if (!consistent) {
				return Error{ErrorType::ConflictingSwitches, arg};
			}
			continue;
		}
		if (parameter.has_value()) {
			return Error{ErrorType::TooManyParameters, arg};
		}
		parameter = arg;
	}

	if (!parameter.has_value()) {
		request.action = args.empty() ? Action::Display
		                              : Action::SwitchesOnly;
		return request;
	}

	// MS-DOS 5.0 reference, APPEND notes: /e and a path cannot share a
	// command line.
	if (request.store_in_env) {
		return Error{ErrorType::EnvWithPath, *parameter};
	}
	if (*parameter == ";") {
		request.action = Action::ClearList;
		return request;
	}
	if (parameter->size() > MaxListLength) {
		return Error{ErrorType::ListTooLong, *parameter};
	}
	request.action = Action::SetList;
	request.list   = *parameter;
	return request;
}

std::optional<ApplyError> Apply(State& state, const Request& request)
{
	const bool first_use = !state.installed;

	if (request.store_in_env && !first_use) {
		return ApplyError::EnvNotFirstUse;
	}
	if (request.search_exec == true && !first_use && !state.exec_allowed) {
		return ApplyError::ExecNotFirstUse;
	}

	state.installed = true;
	if (first_use && request.search_exec == true) {
		state.exec_allowed = true;
	}
	if (request.store_in_env) {
		state.store_in_env = true;
	}
	if (request.search_exec.has_value()) {
		state.search_exec = *request.search_exec;
	}
	if (request.search_with_path.has_value()) {
		state.search_with_path = *request.search_with_path;
	}

	switch (request.action) {
	case Action::SetList: state.list = request.list; break;
	case Action::ClearList: state.list.clear(); break;
	case Action::Display:
	case Action::SwitchesOnly: break;
	}
	return {};
}

State& ResidentState()
{
	static State state = {};
	return state;
}

} // namespace AppendCommand

using namespace AppendCommand;

void APPEND::Run()
{
	if (HelpRequested()) {
		MoreOutputStrings output(*this);
		output.AddString(MSG_Get("PROGRAM_APPEND_HELP_LONG"));
		output.Display();
		return;
	}

	// Error texts are the MS-DOS 4.0 APPEND messages (USA-MS.MSG, APPEND
	// block); which condition raised which one is our mapping.
	const auto parsed = ParseArguments(cmd->GetArguments());
	if (const auto error = std::get_if<Error>(&parsed)) {
		switch (error->type) {
		case ErrorType::IllegalSwitch:
		case ErrorType::TooManyParameters:
			WriteOut(MSG_Get("PROGRAM_APPEND_INVALID_PARAMETER"));
			break;
		case ErrorType::ConflictingSwitches:
		case ErrorType::EnvWithPath:
			WriteOut(MSG_Get("PROGRAM_APPEND_INVALID_COMBINATION"));
			break;
		case ErrorType::ListTooLong:
			WriteOut(MSG_Get("PROGRAM_APPEND_INVALID_PATH"));
			break;
		}
		result_errorcode = 1;
		return;
	}

	const auto& request = std::get<Request>(parsed);
	auto& state         = ResidentState();
	const auto before   = state;

	if (Apply(state, request)) {
		WriteOut(MSG_Get("PROGRAM_APPEND_INVALID_PARAMETER"));
		result_errorcode = 1;
		return;
	}

	if (request.action == Action::Display) {
		if (state.list.empty()) {
			WriteOut(MSG_Get("PROGRAM_APPEND_NO_LIST"));
		} else {
			WriteOut(MSG_Get("PROGRAM_APPEND_LIST"), state.list.c_str());
		}
		return;
	}

	const bool list_changed = request.action == Action::SetList ||
	                          request.action == Action::ClearList;
	if (state.store_in_env && list_changed) {
		// An empty value removes the variable, which is what APPEND ;
		// has to leave behind.
		DOS_PSP parent(psp->GetParent());
		if (!parent.SetEnvironmentValue("APPEND", state.list)) {
			state = before;
			WriteOut(MSG_Get("SHELL_CMD_SET_OUT_OF_SPACE"));
			result_errorcode = 1;
		}
	}
}

void APPEND::AddMessages()
{
	if (MSG_Exists("PROGRAM_APPEND_HELP")) {
		return;
	}
	MSG_Add("PROGRAM_APPEND_HELP",
	        "Let programs open data files in other directories.\n");

	MSG_Add("PROGRAM_APPEND_HELP_LONG",
	        "Let programs open data files in other directories.\n"
	        "\n"
	        "Usage:\n"
	        "  [color=light-green]append[reset] [color=light-cyan]path[reset][;[color=light-cyan]path[reset]...] [/x[:on|:off]] [/path:on|/path:off]\n"
	        "  [color=light-green]append[reset] /e\n"
	        "  [color=light-green]append[reset] ;\n"
	        "  [color=light-green]append[reset]\n"
	        "\n"
	        "Where:\n"
	        "  [color=light-cyan]path[reset]       directory to append, several separated by semicolons.\n"
	        "  ;          cancels the list of appended directories.\n"
	        "  /x:on      also search the list when running programs. /x is short\n"
	        "             for /x:on. It must be given the first time APPEND runs;\n"
	        "             after that you can switch between /x:on and /x:off.\n"
	        "  /path:on   search the list even when the file name carries a path\n"
	        "             (the default); /path:off turns that off.\n"
	        "  /e         keep the list in the environment variable APPEND as well.\n"
	        "             Only allowed the first time APPEND runs, and without a path.\n"
	        "\n"
	        "Notes:\n"
	        "  Running [color=light-green]append[reset] without parameters shows the current list.\n"
	        "  A new list replaces the old one.\n"
	        "  Programs do not search the list yet when they open files.\n");

	MSG_Add("PROGRAM_APPEND_NO_LIST", "No Append\n");
	MSG_Add("PROGRAM_APPEND_LIST", "APPEND=%s\n");
	MSG_Add("PROGRAM_APPEND_INVALID_PARAMETER", "Invalid parameter\n");
	MSG_Add("PROGRAM_APPEND_INVALID_COMBINATION",
	        "Invalid combination of parameters\n");
	MSG_Add("PROGRAM_APPEND_INVALID_PATH", "Invalid path\n");
}
