// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//

#ifndef DOSBOX_PROGRAM_APPEND_H
#define DOSBOX_PROGRAM_APPEND_H

#include "dos/programs.h"

#include <cstddef>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace AppendCommand {

// The DOS command tail holds at most 127 characters, so no valid list is
// longer.
constexpr size_t MaxListLength = 127;

enum class Action { Display, SetList, ClearList, SwitchesOnly };

struct Request {
	Action action                        = Action::Display;
	std::string list                     = {};
	bool store_in_env                    = false;
	std::optional<bool> search_exec      = {};
	std::optional<bool> search_with_path = {};
};

enum class ErrorType {
	IllegalSwitch,
	ConflictingSwitches,
	EnvWithPath,
	TooManyParameters,
	ListTooLong,
};

struct Error {
	ErrorType type       = ErrorType::IllegalSwitch;
	std::string argument = {};
};

std::variant<Error, Request> ParseArguments(const std::vector<std::string>& args);

// What the resident part of APPEND remembers between runs.
struct State {
	bool installed        = false;
	bool store_in_env     = false;
	bool exec_allowed     = false;
	bool search_exec      = false;
	bool search_with_path = true;
	std::string list      = {};

	bool operator==(const State&) const = default;
};

enum class ApplyError { EnvNotFirstUse, ExecNotFirstUse };

std::optional<ApplyError> Apply(State& state, const Request& request);

State& ResidentState();

} // namespace AppendCommand

class APPEND final : public Program {
public:
	APPEND()
	{
		AddMessages();
		help_detail = {HELP_Filter::All,
		               HELP_Category::File,
		               HELP_CmdType::Program,
		               "APPEND"};
	}
	void Run() override;

private:
	void AddMessages();
};

#endif
