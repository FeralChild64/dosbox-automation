// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//

#include "join.h"

#include "misc/messages.h"
#include "more_output.h"
#include "utils/checks.h"
#include "utils/string_utils.h"

#include <optional>
#include <string>

CHECK_NARROWING();

extern unsigned int result_errorcode;

namespace JoinCommand {

static std::optional<char> parse_drive(const std::string& arg)
{
	if (arg.size() != 2 || arg[1] != ':') {
		return {};
	}
	const char letter = arg[0];
	if (letter >= 'a' && letter <= 'z') {
		return static_cast<char>(letter - 'a' + 'A');
	}
	if (letter >= 'A' && letter <= 'Z') {
		return letter;
	}
	return {};
}

static bool is_root(const std::string& target)
{
	const auto after_drive = (target.size() >= 2 && target[1] == ':')
	                               ? target.substr(2)
	                               : target;
	return after_drive == "\\" || after_drive == "/";
}

std::variant<Error, Request> ParseArguments(const std::vector<std::string>& args)
{
	bool disconnect                 = false;
	std::vector<std::string> params = {};

	for (const auto& arg : args) {
		if (arg.starts_with('/')) {
			if (lowcase(arg) != "/d") {
				return Error{ErrorType::IllegalSwitch, arg};
			}
			disconnect = true;
			continue;
		}
		params.push_back(arg);
	}

	if (params.empty()) {
		if (disconnect) {
			return Error{ErrorType::MissingParameter, "/d"};
		}
		return Request{};
	}
	if (params.size() > 2) {
		return Error{ErrorType::TooManyParameters, params[2]};
	}

	const auto drive = parse_drive(params[0]);
	if (!drive) {
		return Error{ErrorType::InvalidDrive, params[0]};
	}

	if (disconnect) {
		if (params.size() > 1) {
			return Error{ErrorType::TooManyParameters, params[1]};
		}
		return Request{Action::Disconnect, *drive, {}};
	}
	if (params.size() < 2) {
		return Error{ErrorType::MissingParameter, params[0]};
	}

	const auto& target = params[1];
	if (parse_drive(target)) {
		return Error{ErrorType::MissingParameter, target};
	}
	if (is_root(target)) {
		return Error{ErrorType::RootTarget, target};
	}
	if (target.size() >= 2 && target[1] == ':' && ciequals(target[0], *drive)) {
		return Error{ErrorType::SameDrive, target};
	}
	return Request{Action::Join, *drive, target};
}

} // namespace JoinCommand

using namespace JoinCommand;

void JOIN::Run()
{
	if (HelpRequested()) {
		MoreOutputStrings output(*this);
		output.AddString(MSG_Get("PROGRAM_JOIN_HELP_LONG"));
		output.Display();
		return;
	}

	const auto parsed = ParseArguments(cmd->GetArguments());
	if (const auto error = std::get_if<Error>(&parsed)) {
		switch (error->type) {
		case ErrorType::IllegalSwitch:
			WriteOut(MSG_Get("SHELL_ILLEGAL_SWITCH"),
			         error->argument.c_str());
			break;
		case ErrorType::MissingParameter:
			WriteOut(MSG_Get("SHELL_MISSING_PARAMETER"));
			break;
		case ErrorType::TooManyParameters:
			WriteOut(MSG_Get("SHELL_TOO_MANY_PARAMETERS"));
			break;
		case ErrorType::InvalidDrive:
			WriteOut(MSG_Get("PROGRAM_JOIN_INVALID_DRIVE"),
			         error->argument.c_str());
			break;
		case ErrorType::RootTarget:
			WriteOut(MSG_Get("PROGRAM_JOIN_ROOT_TARGET"));
			break;
		case ErrorType::SameDrive:
			WriteOut(MSG_Get("PROGRAM_JOIN_SAME_DRIVE"));
			break;
		}
		result_errorcode = 1;
		return;
	}

	// No drive can be joined yet (ada-622b), so the list is always empty
	// and every join or disconnect fails.
	const auto& request = std::get<Request>(parsed);
	switch (request.action) {
	case Action::List: break;
	case Action::Disconnect:
		WriteOut(MSG_Get("PROGRAM_JOIN_NOT_JOINED"), request.drive);
		result_errorcode = 1;
		break;
	case Action::Join:
		WriteOut(MSG_Get("PROGRAM_JOIN_NOT_IMPLEMENTED"));
		result_errorcode = 1;
		break;
	}
}

void JOIN::AddMessages()
{
	if (MSG_Exists("PROGRAM_JOIN_HELP")) {
		return;
	}
	MSG_Add("PROGRAM_JOIN_HELP",
	        "Join a drive to a directory on another drive.\n");

	MSG_Add("PROGRAM_JOIN_HELP_LONG",
	        "Join a drive to a directory on another drive.\n"
	        "\n"
	        "Usage:\n"
	        "  [color=light-green]join[reset] [color=white]drive1:[reset] [color=light-cyan][drive2:]path[reset]\n"
	        "  [color=light-green]join[reset] [color=white]drive1:[reset] /d\n"
	        "  [color=light-green]join[reset]\n"
	        "\n"
	        "Where:\n"
	        "  [color=white]drive1:[reset]  the drive whose contents should appear in the directory.\n"
	        "  [color=light-cyan]path[reset]     the directory where they appear; not the root directory.\n"
	        "  /d       cancels the join for [color=white]drive1:[reset].\n"
	        "\n"
	        "Notes:\n"
	        "  Running [color=light-green]join[reset] without parameters lists the joined drives.\n"
	        "  Joining is not available yet: the command checks its parameters and\n"
	        "  then fails with errorlevel 1. To reach a directory through a drive\n"
	        "  letter, use SUBST.\n");

	MSG_Add("PROGRAM_JOIN_INVALID_DRIVE", "Invalid drive specification: %s\n");
	MSG_Add("PROGRAM_JOIN_ROOT_TARGET",
	        "Cannot join a drive to a root directory.\n");
	MSG_Add("PROGRAM_JOIN_SAME_DRIVE", "Cannot join a drive to itself.\n");
	MSG_Add("PROGRAM_JOIN_NOT_JOINED", "Drive %c: is not joined.\n");
	MSG_Add("PROGRAM_JOIN_NOT_IMPLEMENTED",
	        "Joining drives is not available yet. Use SUBST to map a "
	        "directory to a drive letter.\n");
}
