// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//

#ifndef DOSBOX_PROGRAM_JOIN_H
#define DOSBOX_PROGRAM_JOIN_H

#include "dos/programs.h"

#include <string>
#include <variant>
#include <vector>

namespace JoinCommand {

enum class Action { List, Join, Disconnect };

struct Request {
	Action action      = Action::List;
	char drive         = '\0';
	std::string target = {};
};

enum class ErrorType {
	IllegalSwitch,
	InvalidDrive,
	MissingParameter,
	TooManyParameters,
	RootTarget,
	SameDrive,
};

struct Error {
	ErrorType type       = ErrorType::IllegalSwitch;
	std::string argument = {};
};

std::variant<Error, Request> ParseArguments(const std::vector<std::string>& args);

} // namespace JoinCommand

class JOIN final : public Program {
public:
	JOIN()
	{
		AddMessages();
		help_detail = {HELP_Filter::All,
		               HELP_Category::File,
		               HELP_CmdType::Program,
		               "JOIN"};
	}
	void Run() override;

private:
	void AddMessages();
};

#endif
