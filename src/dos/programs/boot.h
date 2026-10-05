// SPDX-FileCopyrightText:  2021-2025 The DOSBox Staging Team
// SPDX-FileCopyrightText:  2002-2021 The DOSBox Team
// SPDX-License-Identifier: GPL-2.0-or-later

#ifndef DOSBOX_PROGRAM_BOOT_H
#define DOSBOX_PROGRAM_BOOT_H

#include "dos/programs.h"
#include "dosbox.h"

#include <memory>
#include <vector>

namespace ArchiveImages {
class Hold;
}

class BOOT final : public Program {
public:
	BOOT()
	{
		AddMessages();
		help_detail = {HELP_Filter::All,
		               HELP_Category::Dosbox,
		               HELP_CmdType::Program,
		               "BOOT"};
	}
	void Run(void) override;

private:
	static void AddMessages();

	FILE* getFSFile_mounted(const char* filename, uint32_t* ksize,
	                        uint32_t* bsize, uint8_t* error);

	FILE* getFSFile(const char* filename, uint32_t* ksize, uint32_t* bsize,
	                bool tryload = false);

	void printError();

	void DisableUmbXmsEms();
	void NotifyBooting();

	// Design D7: what keeps the booted images' host files valid; handed to
	// the registry once the boot is under way.
	std::vector<std::shared_ptr<ArchiveImages::Hold>> member_holds = {};
};

#endif // DOSBOX_PROGRAM_BOOT_H
