// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//

#include "dos/drive_swap.h"

#include "augra/log.h"

#include "dos/archive/member_path.h"
#include "dos/dos.h"
#include "dos/drives.h"
#include "dos/programs/append.h"
#include "dos/programs/archive_images.h"
#include "dos/programs/mount_policy.h"
#include "ints/bios_disk.h"
#include "misc/cross.h"
#include "misc/logging.h"
#include "utils/checks.h"

#include <cctype>
#include <filesystem>
#include <memory>
#include <string_view>

CHECK_NARROWING();

namespace DriveSwap {

std::filesystem::path ResolveImagePath(const std::filesystem::path& image_path,
                                       const std::filesystem::path& conf_anchor,
                                       const std::vector<std::filesystem::path>& allowed_image_roots)
{
	if (image_path.is_absolute()) {
		return image_path;
	}

	auto candidates = std::vector<std::filesystem::path>{};
	if (!conf_anchor.empty()) {
		candidates.push_back(conf_anchor / image_path);
	}
	for (const auto& root : allowed_image_roots) {
		candidates.push_back(root / image_path);
	}

	for (const auto& candidate : candidates) {
		auto ec = std::error_code();
		if ((std::filesystem::exists(candidate, ec) && !ec) ||
		    ArchiveMount::SplitMemberPath(candidate.string())) {
			return candidate;
		}
	}

	return image_path;
}

Result Swap(char drive_letter, const std::filesystem::path& image_path,
            bool mount_locked, const std::filesystem::path& conf_anchor,
            const std::vector<std::filesystem::path>& allowed_image_roots)
{
	auto result = Result{};

	// Once mounts are locked, the configuration is frozen for everyone,
	// the guest commands (BOOT, IMGMOUNT, MOUNT) and the API alike.
	if (mount_locked) {
		result.error = "mount is locked";
		LOG_WARNING("DRIVE-SWAP: Blocked - locked");
		return result;
	}

	if (!std::isalpha(static_cast<unsigned char>(drive_letter))) {
		result.error = "Invalid drive letter";
		return result;
	}

	const auto drv_idx = static_cast<uint8_t>(
	        std::toupper(static_cast<unsigned char>(drive_letter)) - 'A');

	if (drv_idx >= DOS_DRIVES) {
		result.error = "Invalid drive letter";
		return result;
	}

	// Both callers carry API-origin trust: the REST request arrives over
	// HTTP and the Lua script was loaded through the same webserver.
	const auto resolved_input = ResolveImagePath(image_path,
	                                             conf_anchor,
	                                             allowed_image_roots);

	// The drive's own -ro (every image mount records a mode) holds through
	// swaps to host images too; archive members add the cache rule below.
	const auto drive_mode = ArchiveImages::DriveModeOf(drv_idx);
	std::shared_ptr<ArchiveImages::Hold> hold = {};
	auto read_only     = drive_mode && drive_mode->read_only;
	auto resolved_path = std::filesystem::path();
	auto probe         = std::error_code();
	const auto member  = std::filesystem::exists(resolved_input, probe)
	                           ? ArchiveMount::LoneImageMember(resolved_input)
	                           : ArchiveMount::SplitMemberPath(
                                            resolved_input.string());
	if (member) {
		// Design D7: the archive is judged inside Resolve with API
		// trust; the file swapped in is the engine's own sidecar copy.
		ArchiveImages::Request request = {};
		request.member                 = *member;
		request.origin                 = MountOrigin::Api;
		request.allowed_image_roots    = allowed_image_roots;
		request.conf_anchor            = conf_anchor;
		request.drive_index            = drv_idx;
		auto got = ArchiveImages::Resolve(request);
		if (!got.resolved) {
			const auto* why = MountPolicy::DenyReasonText(
			        got.verdict.reason);
			result.error = std::string("Blocked by mount policy: ") + why;
			LOG_WARNING("DRIVE-SWAP: Blocked - %s", why);
			return result;
		}
		if (!MountPolicy::ValidateDiskImageStructure(got.resolved->host_file)) {
			result.error = "Blocked by mount policy: not a recognized disk image";
			return result;
		}
		resolved_path = got.resolved->host_file;
		hold          = got.resolved->hold;
		read_only     = read_only || got.resolved->read_only;
	} else {
		// A -scratch drive keeps writes for the mount only; a host
		// image would be written in place.
		if (const auto mode = ArchiveImages::DriveModeOf(drv_idx);
		    mode && !mode->scratch_dir.empty()) {
			result.error = "Blocked: a -scratch drive takes disks from archives only";
			LOG_WARNING("DRIVE-SWAP: Blocked - host image onto a -scratch drive");
			return result;
		}
		const auto verdict = MountPolicy::ValidateImagePath(resolved_input,
		                                                    MountOrigin::Api,
		                                                    allowed_image_roots,
		                                                    conf_anchor);
		if (!verdict.allowed) {
			result.error = "Blocked by mount policy";
			LOG_WARNING("DRIVE-SWAP: Blocked - policy violation");
			return result;
		}
		// Use the canonical path from validation, not the raw request
		// string: the object mounted is the object validated.
		resolved_path = verdict.resolved;
	}
	const auto resolved = resolved_path.string();

	// std::filesystem rather than stat + S_ISREG: the POSIX macro does
	// not exist on MSVC, which kept this whole file from building on
	// Windows.
	auto ec = std::error_code();
	if (!std::filesystem::is_regular_file(resolved_path, ec) || ec) {
		result.error = "File not found";
		return result;
	}

	const auto size_bytes = std::filesystem::file_size(resolved_path, ec);
	if (ec) {
		result.error = "File not found";
		return result;
	}
	const auto file_size_kb = static_cast<uint32_t>(size_bytes / 1024);
	bool is_floppy          = false;
	for (const auto& geom : BIOS_GetDiskGeometryList()) {
		if (geom.ksize == file_size_kb) {
			is_floppy = true;
			break;
		}
	}

	// A booted or '-fs none' floppy has no DOS drive, only the BIOS disk;
	// swap that disk the way BOOT loads images (ada-plh7)
	const bool is_raw_slot = drv_idx < 2 && !Drives[drv_idx] &&
	                         imageDiskList[drv_idx];
	if (is_raw_slot) {
		if (!is_floppy) {
			result.error = "Not a floppy image";
			return result;
		}
		// BOOT keeps every image of its set open in the Ctrl+F4 list; a
		// swap to one of them takes that object, so the file is not
		// open twice with two buffers (ada-ciru item 1).
		std::shared_ptr<imageDisk> listed = {};
		for (const auto& slot : diskSwap) {
			if (slot && std::string_view(slot->diskname) == resolved) {
				listed = slot;
				break;
			}
		}
		const auto old_disk = imageDiskList[drv_idx];
		// BOOT opens its set writable; a read-only drive opens its own.
		if (listed && !read_only) {
			imageDiskList[drv_idx] = listed;
		} else {
			bool is_readonly = read_only;
			auto* file = fopen_wrap_ro_fallback(resolved, is_readonly);
			if (file == nullptr) {
				result.error = "File not found";
				return result;
			}
			imageDiskList[drv_idx] = std::make_shared<imageDisk>(
			        file, resolved.c_str(), file_size_kb, false);

			// BOOT's Ctrl+F4 list would otherwise bring the old
			// image back
			for (auto& slot : diskSwap) {
				if (slot == old_disk) {
					slot = imageDiskList[drv_idx];
				}
			}
		}
		augra::log_info("drive-swap",
		                "swapped the raw disk in %c: for '%s'",
		                'A' + drv_idx,
		                resolved.c_str());
		if (hold) {
			ArchiveImages::Keep(drv_idx, {hold});
		}
		result.ok = true;
		return result;
	}

	// A file the drive's Ctrl+F4 list holds is swapped to through that
	// list's object: a second object would write with its own FAT cache
	// (ada-99u7). A read-only request never takes a writable one.
	std::shared_ptr<fatDrive> new_drive = {};
	const auto& listed = DriveManager::GetFilesystemImages(drv_idx);
	for (size_t i = 0; i < listed.size(); ++i) {
		auto fat = std::dynamic_pointer_cast<fatDrive>(listed[i]);
		if (fat && fat->loadedDisk &&
		    std::string_view(fat->loadedDisk->diskname) == resolved &&
		    (!read_only || fat->IsReadOnly())) {
			new_drive = std::move(fat);
			DriveManager::SelectDisk(drv_idx, i);
			break;
		}
	}
	if (!new_drive) {
		// Build the new drive before releasing the old one so a
		// construction failure does not leave the slot empty.
		new_drive = std::make_shared<fatDrive>(
		        resolved.c_str(), 512, 0, 0, 0, is_floppy ? 0xF0 : 0xF8, read_only);
		if (!new_drive->created_successfully) {
			result.error = "Failed to mount image";
			return result;
		}
	}

	Drives[drv_idx].reset();

	Drives[drv_idx] = new_drive;

	if (drv_idx < MAX_DISK_IMAGES) {
		imageDiskList[drv_idx] = new_drive->loadedDisk;
	}

	// The volume label reaches the drive's cache through a directory
	// search, as MOUNT does after construction; without it the swapped
	// disk shows no label.
	const RealPt saved_dta = dos.dta();
	dos.dta(dos.tables.tempdta);
	AppendCommand::Suspend no_append_search;
	const char root[7] = {
	        static_cast<char>('A' + drv_idx), ':', '\\', '*', '.', '*', 0};
	if (!DOS_FindFirst(root, FatAttributeFlags::Volume)) {
		LOG_WARNING("DRIVE-SWAP: Unable to find %c drive's volume label",
		            'A' + drv_idx);
	}
	dos.dta(saved_dta);

	if (hold) {
		ArchiveImages::Keep(drv_idx, {hold});
	}
	result.ok = true;
	return result;
}

} // namespace DriveSwap
