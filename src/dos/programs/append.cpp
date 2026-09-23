// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//

#include "append.h"

#include "dos/dos.h"
#include "more_output.h"
#include "shell/shell.h"
#include "utils/string_utils.h"

void APPEND::Run()
{
	if (HelpRequested()) {
		MoreOutputStrings output(*this);
		output.AddString(MSG_Get("PROGRAM_APPEND_HELP_LONG"));
		output.Display();
		return;
	}

	std::string tmp = {};
	cmd->GetStringRemain(tmp);
	trim(tmp);

	const bool has_e = cmd->FindExist("/e", true);
	const bool has_x = cmd->FindExist("/x:on", true) ||
	                   cmd->FindExist("/x", true);

	if (tmp.empty() && !has_e && !has_x) {
		if (const auto val = psp->GetEnvironmentValue("APPEND")) {
			WriteOut("%s\n", val->c_str());
		} else {
			WriteOut(MSG_Get("PROGRAM_APPEND_NO_PATH"));
		}
		return;
	}

	if (has_e) {
		WriteOut(MSG_Get("PROGRAM_APPEND_ENV_SET"));
	}

	if (!tmp.empty()) {
		auto shell = DOS_GetFirstShell();
		assert(shell);
		shell->SetEnv("APPEND", tmp.c_str());
		WriteOut(MSG_Get("PROGRAM_APPEND_PATH_SET"), tmp.c_str());
	}
}

void APPEND::AddMessages()
{
	if (MSG_Exists("PROGRAM_APPEND_HELP")) {
		return;
	}
	MSG_Add("PROGRAM_APPEND_HELP", "Set a search path for data files.\n");

	MSG_Add("PROGRAM_APPEND_HELP_LONG",
	        "Set a search path for data files.\n"
	        "\n"
	        "Usage:\n"
	        "  [color=light-green]append[reset] [color=light-cyan]path[reset][;[color=light-cyan]path[reset]...]\n"
	        "  [color=light-green]append[reset] /e\n"
	        "  [color=light-green]append[reset]              (display current path)\n"
	        "  [color=light-green]append[reset] ;             (clear the path)\n"
	        "\n"
	        "  /e    Store the append path in the environment variable APPEND.\n"
	        "        Must be the first use of APPEND after starting.\n"
	        "  /x:on Also search appended directories for program execution.\n"
	        "\n"
	        "Notes:\n"
	        "  This is a compatibility stub. It sets the APPEND environment\n"
	        "  variable so installers that check for APPEND find it present.\n"
	        "  The data-file search path is not hooked into INT 21h file\n"
	        "  operations. Programs that rely on APPEND to locate data files\n"
	        "  in other directories will not find them.\n"
	        "\n"
	        "  Contributions to add the full INT 21h hooking are welcome.\n");

	MSG_Add("PROGRAM_APPEND_NO_PATH", "No APPEND path set.\n");
	MSG_Add("PROGRAM_APPEND_ENV_SET",
	        "APPEND path will be stored in environment.\n");
	MSG_Add("PROGRAM_APPEND_PATH_SET", "APPEND=%s\n");
}
