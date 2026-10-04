// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//
#ifndef DOSBOX_TESTS_DRIVE_SEARCH_HELPER_H
#define DOSBOX_TESTS_DRIVE_SEARCH_HELPER_H

#include "dos/dos.h"
#include "dos/drives.h"

#include <cstdint>
#include <string>

// One search through a drive with its own DTA in emulated memory, the way
// a program walking a tree keeps one DTA per level.
class DriveSearch {
public:
	DriveSearch(DOS_Drive& drive, const uint8_t drive_index,
	            const RealPt dta_address)
	        : drive(drive),
	          drive_index(drive_index),
	          dta(dta_address)
	{}

	bool First(const char* dir, FatAttributeFlags attr = Directories())
	{
		char pattern[] = "*.*";
		dta.SetupSearch(drive_index, attr, pattern);
		return drive.FindFirst(dir, dta, false);
	}
	bool Next()
	{
		return drive.FindNext(dta);
	}
	std::string Name() const
	{
		DOS_DTA::Result result = {};
		dta.GetResult(result);
		return result.name;
	}

private:
	static FatAttributeFlags Directories()
	{
		FatAttributeFlags attr = {};
		attr.directory         = true;
		return attr;
	}

	DOS_Drive& drive;
	uint8_t drive_index = 0;
	DOS_DTA dta;
};

#endif
