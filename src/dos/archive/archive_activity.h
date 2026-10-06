// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//
#ifndef DOSBOX_ARCHIVE_ACTIVITY_H
#define DOSBOX_ARCHIVE_ACTIVITY_H

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace ArchiveMount {

// The stem of a reader temp name, <stem>.<32 lowercase hex>.part (the
// reader's write-then-rename), or nothing for any other name.
std::optional<std::u8string> PartFileStem(std::u8string_view name);

// Plan 5 item 5.7: what the archive helper is doing for the engine, so
// GET /api/v1/status can say "preparing game files" while MOUNT blocks
// the emulation thread. Read from any thread.
namespace Activity {

enum class Kind { Identifying, Materializing };

struct Snapshot {
	Kind kind                    = Kind::Identifying;
	std::string archive          = {}; // the name without its last extension
	int64_t bytes_total          = 0;
	std::filesystem::path target = {}; // a member's file or a folder
	bool target_is_folder        = false;
};

// Sets its record for its lifetime; the newest live guard is current.
class Guard {
public:
	explicit Guard(Snapshot snapshot);
	~Guard();
	Guard(const Guard&)            = delete;
	Guard& operator=(const Guard&) = delete;

private:
	uint64_t id = 0;
};

std::optional<Snapshot> Current();

// Sees every guard as it starts; for the tests, never set by the engine.
using Observer = void (*)(const Snapshot& snapshot);
extern Observer observer;

// What is on disk so far: 0 while identifying; a member's newest part
// file, else its finished file; the regular files in MaterializeAll's
// folder. Symlinks count nothing. Never throws.
int64_t BytesDone(const Snapshot& snapshot);

struct Status {
	std::string activity = {}; // "identifying" or "materializing"
	std::string archive  = {};
	int64_t bytes_done   = 0;
	int64_t bytes_total  = 0;
};

std::optional<Status> CurrentStatus();

} // namespace Activity

} // namespace ArchiveMount

#endif
