// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//

#ifndef DOSBOX_PROGRAM_APPEND_H
#define DOSBOX_PROGRAM_APPEND_H

#include "dos/programs.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
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
	bool installed         = false;
	bool store_in_env      = false;
	bool exec_allowed      = false;
	bool search_exec       = false;
	bool search_with_path  = true;
	bool search_with_drive = true;
	bool enabled           = true;
	std::string list       = {};

	bool operator==(const State&) const = default;
};

enum class ApplyError { EnvNotFirstUse, ExecNotFirstUse };

std::optional<ApplyError> Apply(State& state, const Request& request);

State& ResidentState();

// The INT 2Fh AX=B706h/B707h state word, RBIL table 02980.
uint16_t StateBits(const State& state);
void ApplyStateBits(State& state, uint16_t bits);

// Answers INT 2Fh AH=B7h once APPEND has been run, and keeps the list that
// AX=B704h points at in step with the resident state.
void InstallResidentPart();

enum class SearchKind { Open, Find, Exec };

// The names to retry, in list order, after an open, find or exec of `name`
// failed where it was asked for. Empty when APPEND does not cover the call.
// FCB names always search; MS-DOS 4.0 APPEND.ASM, FCB_open, ignores /path.
std::vector<std::string> SearchCandidates(std::string_view name,
                                          SearchKind kind, bool fcb_name);

// The first candidate that exists as a file (EXEC and the shell's program
// lookup).
std::optional<std::string> FindInList(std::string_view name, SearchKind kind);

// Turns the search off for a scope: the retries themselves, EXEC's own open,
// and DIR (MS-DOS 4.0 COMMAND.COM, change A020).
class Suspend {
public:
	Suspend();
	~Suspend();
	Suspend(const Suspend&)            = delete;
	Suspend& operator=(const Suspend&) = delete;

private:
	bool was_suspended = false;
};

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
