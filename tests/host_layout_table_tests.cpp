// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//

#include "misc/host_locale.h"

#include <gtest/gtest.h>

#include <map>
#include <set>
#include <string>

#include "dos/dos_locale.h"

namespace {

std::set<std::string> KnownDosLayouts()
{
	std::set<std::string> codes = {};
	for (const auto& entry : LocaleData::KeyboardLayoutInfo) {
		codes.insert(entry.layout_codes.begin(), entry.layout_codes.end());
	}
	return codes;
}

// What each table's source can get wrong without an outside list of host
// layout names; the names are audited against xkb, kbd and kbdlayout.info
// in the developer docs (keyboard-layout-table-audit.md).
TEST(HostLayoutTables, EveryRowOnThisPlatformIsWellFormed)
{
	const auto tables = GetHostLayoutTables();
	ASSERT_FALSE(tables.empty());

	const auto known = KnownDosLayouts();

	for (const auto& table : tables) {
		SCOPED_TRACE(table.name);
		ASSERT_FALSE(table.rows.empty());

		std::map<std::string, int> seen = {};
		for (const auto& [key, target] : table.rows) {
			++seen[key];

			EXPECT_FALSE(key.empty());
			EXPECT_EQ(key.find_first_of(" \t\r\n"), std::string::npos)
			        << "whitespace in key '" << key << "'";
			EXPECT_TRUE(known.contains(target.keyboard_layout))
			        << "'" << key << "' maps to unknown DOS layout '"
			        << target.keyboard_layout << "'";
		}

		for (const auto& [key, count] : seen) {
			EXPECT_EQ(count, 1) << "duplicate key '" << key << "'";
		}
	}
}

} // namespace
