// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//
#include "dos/drive_search_slots.h"

#include <gtest/gtest.h>

#include <set>

namespace {

struct Cursor {
	int value = 0;
};

TEST(DriveSearchSlots, AnAcquiredCursorIsFreshAndFoundByItsId)
{
	DriveSearchSlots<Cursor, 3> slots;
	auto first         = slots.Acquire();
	first.cursor.value = 7;
	ASSERT_NE(slots.Find(first.id), nullptr);
	EXPECT_EQ(slots.Find(first.id)->value, 7);
	slots.Release(first.id);
	EXPECT_EQ(slots.Find(first.id), nullptr);
	const auto again = slots.Acquire();
	EXPECT_EQ(again.cursor.value, 0);
}

TEST(DriveSearchSlots, AReleasedSlotIsTakenBeforeAnyLiveOne)
{
	DriveSearchSlots<Cursor, 3> slots;
	const auto a = slots.Acquire();
	const auto b = slots.Acquire();
	for (int i = 0; i < 100; ++i) {
		const auto child = slots.Acquire();
		EXPECT_NE(child.id, a.id);
		EXPECT_NE(child.id, b.id);
		slots.Release(child.id);
	}
	EXPECT_NE(slots.Find(a.id), nullptr);
	EXPECT_NE(slots.Find(b.id), nullptr);
}

TEST(DriveSearchSlots, WhenAllAreLiveTheLeastRecentlyUsedGivesWay)
{
	DriveSearchSlots<Cursor, 3> slots;
	const auto a = slots.Acquire();
	const auto b = slots.Acquire();
	const auto c = slots.Acquire();
	ASSERT_EQ((std::set<uint16_t>{a.id, b.id, c.id}).size(), 3u);
	// a was opened first but read last, so b is the oldest in use.
	ASSERT_NE(slots.Find(a.id), nullptr);
	const auto d = slots.Acquire();
	EXPECT_EQ(d.id, b.id);
	EXPECT_NE(slots.Find(a.id), nullptr);
	EXPECT_NE(slots.Find(c.id), nullptr);
}

TEST(DriveSearchSlots, AnIdOutOfRangeFindsNothingAndReleasesNothing)
{
	DriveSearchSlots<Cursor, 3> slots;
	const auto a = slots.Acquire();
	EXPECT_EQ(slots.Find(3), nullptr);
	EXPECT_EQ(slots.Find(0xffff), nullptr);
	slots.Release(0xffff);
	EXPECT_NE(slots.Find(a.id), nullptr);
}

TEST(DriveSearchSlots, ASingleSlotIsReusedEveryTime)
{
	DriveSearchSlots<Cursor, 1> slots;
	const auto a = slots.Acquire();
	const auto b = slots.Acquire();
	EXPECT_EQ(a.id, 0);
	EXPECT_EQ(b.id, 0);
}

} // namespace
