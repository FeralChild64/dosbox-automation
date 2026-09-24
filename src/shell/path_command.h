// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//

#ifndef DOSBOX_SHELL_PATH_COMMAND_H
#define DOSBOX_SHELL_PATH_COMMAND_H

#include <string>
#include <string_view>
#include <variant>

namespace PathCommand {

enum class Action { Display, Set };

struct Request {
	Action action     = Action::Display;
	std::string value = {};
};

// Text after the value that is not blank or tab: "Too many parameters"
struct TooManyParameters {};

std::variant<TooManyParameters, Request> ParseArguments(std::string_view line);

} // namespace PathCommand

#endif
