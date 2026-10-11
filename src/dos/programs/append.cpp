// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//

#include "append.h"

#include "cpu/registers.h"
#include "dos/dos.h"
#include "dos/dos_system.h"
#include "hardware/memory.h"
#include "misc/messages.h"
#include "more_output.h"
#include "utils/checks.h"
#include "utils/string_utils.h"

#include <algorithm>
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

// APPEND.ASM mode_flags.
constexpr uint16_t EnabledBit = 0x0001;
constexpr uint16_t DriveBit   = 0x1000;
constexpr uint16_t PathBit    = 0x2000;
constexpr uint16_t EnvBit     = 0x4000;
constexpr uint16_t ExecBit    = 0x8000;

uint16_t StateBits(const State& state)
{
	uint16_t bits = 0;
	bits |= state.enabled ? EnabledBit : 0;
	bits |= state.search_with_drive ? DriveBit : 0;
	bits |= state.search_with_path ? PathBit : 0;
	bits |= state.store_in_env ? EnvBit : 0;
	bits |= state.search_exec ? ExecBit : 0;
	return bits;
}

void ApplyStateBits(State& state, const uint16_t bits)
{
	state.enabled           = (bits & EnabledBit) != 0;
	state.search_with_drive = (bits & DriveBit) != 0;
	state.search_with_path  = (bits & PathBit) != 0;
	state.store_in_env      = (bits & EnvBit) != 0;
	state.search_exec       = (bits & ExecBit) != 0;
}

// Room for the longest list plus its terminator, as in APPEND.ASM's dirlst.
constexpr uint16_t ListParagraphs = 8;
static_assert(ListParagraphs * 16 > MaxListLength);

static uint16_t list_segment = 0;

static void write_resident_list(const std::string& list)
{
	const auto bytes = std::min(list.size(), MaxListLength);
	MEM_BlockWrite(PhysicalMake(list_segment, 0), list.c_str(), bytes);
	mem_writeb(PhysicalMake(list_segment, static_cast<uint16_t>(bytes)), 0);
}

// INT 2Fh AH=B7h per RBIL; AL=03h (TopView) and AL=11h (return the found
// name) are not answered.
static bool append_multiplex()
{
	if (reg_ah != 0xb7) {
		return false;
	}
	auto& state = ResidentState();
	switch (reg_al) {
	case 0x00: reg_al = 0xff; return true;
	case 0x02: reg_ax = 0xffff; return true;
	case 0x04:
		SegSet16(es, list_segment);
		reg_di = 0;
		return true;
	case 0x06: reg_bx = StateBits(state); return true;
	case 0x07: ApplyStateBits(state, reg_bx); return true;
	case 0x10:
		// MS-DOS 5.00 APPEND answers BX = CX = 0 and its own version.
		reg_ax = StateBits(state);
		reg_bx = 0;
		reg_cx = 0;
		reg_dl = 5;
		reg_dh = 0;
		return true;
	default: return false;
	}
}

void InstallResidentPart()
{
	// DOS_GetMemory never frees, so the list buffer and the handler are
	// set up once and stay for the session.
	if (list_segment == 0) {
		list_segment = DOS_GetMemory(ListParagraphs);
		DOS_AddMultiplexHandler(append_multiplex);
	}
	write_resident_list(ResidentState().list);
}

static bool search_suspended = false;

Suspend::Suspend() : was_suspended(search_suspended)
{
	search_suspended = true;
}

Suspend::~Suspend()
{
	search_suspended = was_suspended;
}

// MS-DOS 4.0 APPEND.ASM, address_path: under /e the list is APPEND= of the
// calling process; the resident copy only when it has no environment.
static std::string active_list(const State& state)
{
	if (!state.store_in_env) {
		return state.list;
	}
	DOS_PSP caller(dos.psp());
	if (caller.GetEnvironment() == 0) {
		return state.list;
	}
	return caller.GetEnvironmentValue("APPEND").value_or("");
}

std::vector<std::string> SearchCandidates(const std::string_view name,
                                          const SearchKind kind, const bool fcb_name)
{
	const auto& state = ResidentState();
	if (search_suspended || !state.installed || !state.enabled) {
		return {};
	}
	if (kind != SearchKind::Open && !state.search_exec) {
		return {};
	}

	// APPEND.ASM check_path_mode: with /path:off a drive alone also stops
	// the search.
	const bool has_drive = name.size() >= 2 && name[1] == ':';
	const bool has_path  = name.find_first_of("\\/") != name.npos;
	if (!fcb_name) {
		if (has_drive && !state.search_with_drive) {
			return {};
		}
		if (!state.search_with_path && (has_drive || has_path)) {
			return {};
		}
	}

	const auto name_start = name.find_last_of("\\/:");
	const auto file_name  = name_start == name.npos
	                              ? name
	                              : name.substr(name_start + 1);
	if (file_name.empty()) {
		return {};
	}

	// APPEND.ASM treats a value that starts with a blank or ';' as no list.
	const auto list = active_list(state);
	if (list.empty() || list.front() == ' ' || list.front() == ';') {
		return {};
	}

	std::vector<std::string> candidates = {};
	for (const auto& directory : split_with_empties(list, ';')) {
		if (directory.empty()) {
			continue;
		}
		const char last            = directory.back();
		const bool needs_separator = last != '\\' && last != '/' &&
		                             last != ':';
		candidates.push_back(directory + (needs_separator ? "\\" : "") +
		                     std::string(file_name));
	}
	return candidates;
}

std::optional<std::string> FindInList(const std::string_view name,
                                      const SearchKind kind)
{
	const auto candidates = SearchCandidates(name, kind, false);
	if (candidates.empty()) {
		return {};
	}
	Suspend no_nested_search;
	for (const auto& candidate : candidates) {
		if (DOS_FileExists(candidate.c_str())) {
			return candidate;
		}
	}
	return {};
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
	InstallResidentPart();

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
			InstallResidentPart();
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
	        "  New files are always created in the current directory.\n"
	        "  DIR does not show files found through the list.\n");

	MSG_Add("PROGRAM_APPEND_NO_LIST", "No Append\n");
	MSG_Add("PROGRAM_APPEND_LIST", "APPEND=%s\n");
	MSG_Add("PROGRAM_APPEND_INVALID_PARAMETER", "Invalid parameter\n");
	MSG_Add("PROGRAM_APPEND_INVALID_COMBINATION",
	        "Invalid combination of parameters\n");
	MSG_Add("PROGRAM_APPEND_INVALID_PATH", "Invalid path\n");
}
