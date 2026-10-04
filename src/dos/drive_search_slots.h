// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//
#ifndef DOSBOX_DRIVE_SEARCH_SLOTS_H
#define DOSBOX_DRIVE_SEARCH_SLOTS_H

#include <array>
#include <cstdint>

// Search cursors addressed by the DTA's 16-bit directory id. DOS has no call
// that ends a search, so a slot frees only when its search runs out; when
// all are live, the one unused the longest gives way.
template <typename Cursor, int Capacity>
class DriveSearchSlots {
	static_assert(Capacity > 0 && Capacity <= 0x10000);

public:
	struct Acquired {
		uint16_t id;
		Cursor& cursor;
	};

	Acquired Acquire()
	{
		int pick = 0;
		for (int i = 0; i < Capacity; ++i) {
			if (!slots[i].in_use) {
				pick = i;
				break;
			}
			if (slots[i].last_use < slots[pick].last_use) {
				pick = i;
			}
		}
		auto& slot    = slots[pick];
		slot.cursor   = {};
		slot.in_use   = true;
		slot.last_use = ++clock;
		return {static_cast<uint16_t>(pick), slot.cursor};
	}

	Cursor* Find(const uint16_t id)
	{
		if (id >= Capacity || !slots[id].in_use) {
			return nullptr;
		}
		slots[id].last_use = ++clock;
		return &slots[id].cursor;
	}

	void Release(const uint16_t id)
	{
		if (id >= Capacity) {
			return;
		}
		slots[id].cursor = {};
		slots[id].in_use = false;
	}

private:
	struct Slot {
		Cursor cursor     = {};
		bool in_use       = false;
		uint64_t last_use = 0;
	};
	std::array<Slot, Capacity> slots = {};
	uint64_t clock                   = 0;
};

#endif
