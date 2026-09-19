// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//

#include "dos/dos.h"

#include <gtest/gtest.h>

#include "dosbox_test_fixture.h"

namespace {

class DOSRuntimeSettingsTest : public DOSBoxTestFixture {};

// Regression test for the callback table exhaustion bug.
// Before the fix, every runtime [dos] setting change rebuilt EMS and
// XMS unconditionally. EMS leaked one bare callback slot per cycle
// (call_int67 was never deallocated). With CB_MAX=128 and many slots
// pre-allocated at boot, the table exhausted after ~50 cycles,
// crashing with E_Exit("CALLBACK: Can't allocate handler.").
//
// The fix routes notify_dos_setting_updated by prop_name and makes
// the EMS/XMS pool allocations static. This test cycles 120 EMS
// reinits - enough to exhaust the old table twice over.
TEST_F(DOSRuntimeSettingsTest, EmsReinitDoesNotExhaustCallbackTable)
{
	for (int i = 0; i < 120; ++i) {
		DOS_NotifySettingUpdated("ems");
	}
}

// Verify that changing "ver" does not trigger EMS/XMS rebuilds.
// DOS_GetMemory is a bump allocator: each call returns the current
// watermark and advances it. Calling it once before and once after
// the test loop measures whether the loop consumed any paragraphs.
TEST_F(DOSRuntimeSettingsTest, VerChangeDoesNotRebuildEmsXms)
{
	const uint16_t before = DOS_GetMemory(1);

	for (int i = 0; i < 20; ++i) {
		DOS_NotifySettingUpdated("ver");
	}

	const uint16_t after = DOS_GetMemory(1);

	// before consumed 1 paragraph, after consumed 1. If the loop
	// allocated nothing, after == before + 1.
	EXPECT_EQ(after - before, 1);
}

// Same check for "country" - locale only, no EMS/XMS.
TEST_F(DOSRuntimeSettingsTest, CountryChangeDoesNotRebuildEmsXms)
{
	const uint16_t before = DOS_GetMemory(1);

	for (int i = 0; i < 20; ++i) {
		DOS_NotifySettingUpdated("country");
	}

	const uint16_t after = DOS_GetMemory(1);
	EXPECT_EQ(after - before, 1);
}

} // namespace
