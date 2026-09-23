// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//

#include "join.h"

#include "more_output.h"
#include "utils/string_utils.h"

void JOIN::Run()
{
	if (HelpRequested()) {
		MoreOutputStrings output(*this);
		output.AddString(MSG_Get("PROGRAM_JOIN_HELP_LONG"));
		output.Display();
		return;
	}

	std::string tmp = {};
	cmd->GetStringRemain(tmp);
	trim(tmp);

	if (cmd->FindExist("/d", true)) {
		WriteOut(MSG_Get("PROGRAM_JOIN_STUB_DISCONNECT"));
		return;
	}

	if (tmp.empty()) {
		WriteOut(MSG_Get("PROGRAM_JOIN_STUB_LIST"));
		return;
	}

	WriteOut(MSG_Get("PROGRAM_JOIN_STUB_NOT_IMPLEMENTED"));
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
	        "  [color=light-green]join[reset] [color=white]drive1:[reset] [color=light-cyan]drive2:\\path[reset]\n"
	        "  [color=light-green]join[reset] [color=white]drive1:[reset] /d     (disconnect)\n"
	        "  [color=light-green]join[reset]                (list joins)\n"
	        "\n"
	        "  [color=white]drive1:[reset]    The drive to merge into a directory.\n"
	        "  [color=light-cyan]drive2:\\path[reset]  The directory where drive1 appears.\n"
	        "  /d          Disconnect a previous join.\n"
	        "\n"
	        "Notes:\n"
	        "  This is a compatibility stub. It accepts the standard switches\n"
	        "  without error so installers that check for JOIN find it present.\n"
	        "  The drive-merging functionality is not implemented.\n"
	        "\n"
	        "  For mapping a directory to a drive letter, use SUBST instead.\n"
	        "  Contributions to add the full join functionality are welcome.\n");

	MSG_Add("PROGRAM_JOIN_STUB_DISCONNECT", "No joins active.\n");
	MSG_Add("PROGRAM_JOIN_STUB_LIST", "No joins active.\n");
	MSG_Add("PROGRAM_JOIN_STUB_NOT_IMPLEMENTED",
	        "JOIN is not yet implemented. Use SUBST for drive-to-path mapping.\n");
}
