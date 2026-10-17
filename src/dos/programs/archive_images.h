// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//
#ifndef DOSBOX_ARCHIVE_IMAGES_H
#define DOSBOX_ARCHIVE_IMAGES_H

#include "dos/archive/archive_cache.h"
#include "dos/archive/archive_lock.h"
#include "dos/archive/chain_resolver.h"
#include "dos/archive/member_path.h"
#include "dos/programs/mount_policy.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Design D7 and D17: an image named inside an archive, for MOUNT, BOOT and
// the swap. Runs on the emulation thread, like every mount.
namespace ArchiveImages {

// What keeps a member's host file valid while it is in use: the cache
// lease of a CD image or a read-only disk, the lock of a sidecar image
// copy, and the scratch directory the helper materializes into.
class Hold {
public:
	Hold() = default;
	~Hold();
	Hold(const Hold&)            = delete;
	Hold& operator=(const Hold&) = delete;

	std::unique_ptr<ArchiveMount::ArchiveCache::Lease> lease = {};
	std::optional<ArchiveMount::FileLock> image_lock         = {};
	std::filesystem::path scratch_dir                        = {};
	std::shared_ptr<ArchiveMount::FileLock> scratch_lock     = {};

	// Nested archives: the intermediate leases in opening order, released
	// innermost first by the destructor; their clients close with the host's.
	std::vector<ArchiveMount::ChainLink> chain_links = {};
	std::string chain = {}; // ChainPrefix of the member, "" when not nested
	// The chain as the request spelled it, which leaves out any store the
	// house rule entered (N4), so a swap back by the same path finds this.
	std::string spelled_chain = {};
	// N4: the first store the house rule entered ("" when none).
	std::string house_member = {};

	// The member this hold serves, so its drive can be handed it again,
	// and the listing it was picked from, so a swap back to it needs no
	// helper (a sequential archive would be unpacked once more).
	std::filesystem::path archive   = {};
	std::string key                 = {};
	std::filesystem::path host_file = {};
	std::string member_name         = {};
	int ordinal                     = ArchiveMount::ImpliedOrdinal;
	int64_t size                    = 0;
	std::shared_ptr<const std::vector<ArchiveMount::Entry>> entries = {};
};

enum class Kind { CdImage, CdCompanion, DiskImage };

// By the member's extension: iso, cue and bin are CD images; mds and ccd
// name companion files 4a does not rewrite; anything else is a disk.
Kind KindOf(std::string_view member_name);

struct Request {
	ArchiveMount::MemberPath member = {};
	MountOrigin origin              = MountOrigin::GuestCommand;
	bool read_only                  = false; // -ro or -nosidecar: the cache
	std::string sidecar_path        = {};    // -sidecar, already resolved
	// Phase 6.4: a -scratch drive; the copy lands under this directory's
	// .images instead of a sidecar's. Empty: the sidecar rules apply.
	std::filesystem::path scratch_dir = {};
	// The policy roots the archive is judged under; MOUNT and BOOT pass
	// the policy's own, the swap passes what its caller gave it.
	std::vector<std::filesystem::path> allowed_image_roots = {};
	std::filesystem::path conf_anchor                      = {};
	// The swap's drive: a member that drive already holds is handed back
	// rather than locked a second time.
	std::optional<int> drive_index = {};
};

struct Resolved {
	std::filesystem::path host_file = {}; // what the image code opens
	std::string member_name         = {}; // the entry's UTF-8 path
	int64_t size                    = 0;
	std::shared_ptr<Hold> hold      = {};
	bool read_only = false; // a cache file other engines lease too
};

struct Result {
	std::optional<Resolved> resolved = {};
	MountVerdict verdict             = {}; // the refusal when not resolved
	std::string detail               = {};
};

// The policy on the archive, the helper, the member, then the bytes: the
// cache for CD images and read-only disks, the sidecar's .images copy
// otherwise. The caller checks the mount lock first and reports refusals.
Result Resolve(const Request& request);

// Holds of a mounted drive by drive index, added to what the drive holds
// (a swap leaves the other images of a set open); released together at
// unmount. Raw drive numbers 0 to 3 use the index of A to D.
void Keep(int drive_index, std::vector<std::shared_ptr<Hold>> holds);
void Release(int drive_index);

// How MOUNT mounted the drive's archive: a later request naming the drive
// (the swap) inherits it, so -ro and -nosidecar hold through swaps and a
// named sidecar keeps every disk of the set (ada-ciru items 2 and 4).
struct DriveMode {
	std::filesystem::path archive = {}; // canonical, as Resolve judged it
	bool read_only                = false;
	std::string sidecar_path      = {};
	std::filesystem::path scratch_dir = {}; // every swap of the drive
	                                        // copies here
};
void SetDriveMode(int drive_index, const DriveMode& mode);
std::optional<DriveMode> DriveModeOf(int drive_index);
// BOOT never returns to DOS: its holds live until the engine exits, under
// A and B, where swapInDisks puts the set whatever drive was booted.
void KeepForBoot(std::vector<std::shared_ptr<Hold>> holds);
size_t HeldCountForTests(int drive_index);
void ReleaseAllForTests();

} // namespace ArchiveImages

#endif // DOSBOX_ARCHIVE_IMAGES_H
