// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//

#ifndef DOSBOX_PROGRAM_JOIN_H
#define DOSBOX_PROGRAM_JOIN_H

#include "dos/programs.h"

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
