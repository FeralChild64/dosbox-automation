// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//

#ifndef DOSBOX_PROGRAM_MOUNT_POLICY_H
#define DOSBOX_PROGRAM_MOUNT_POLICY_H

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "dos/archive/archive_source.h"
#include "dos/archive/head_sniff.h"

enum class DenyReason {
	None,
	DoesNotResolve,
	NotRegularFile,
	SymlinkComponent,
	SystemPath,
	OutsideWhitelist,
	NotADiskImage,
	NotADirectory,
	ReservedDeviceName,
	// Archive mounts (archive mount design 2.3)
	NotAnArchive,
	ArchiveTooLarge,
	ArchiveSequential,
	ArchiveSolidUnsupported,
	ArchiveEncrypted,
	ArchiveMultiVolume,
	ArchiveUnsupported,
	ArchiveEntryRejected,
	ArchiveCorrupt,
	ArchiveChanged,
	ArchiveHelperFailed,
	ArchiveRootNotFound,
	CacheUnavailable,
	// Phase 3: the sidecar (3.2)
	SidecarBusy,
	SidecarUnavailable,
	// Phase 4a: member paths (design D7, D17)
	ArchiveMemberNotFound,
	ArchiveCueRejected,
	// G12: a second archive on one MOUNT
	ArchiveOnePerDrive,
};

struct MountVerdict {
	bool allowed                   = false;
	DenyReason reason              = DenyReason::None;
	std::filesystem::path resolved = {};
};

enum class MountOrigin { GuestCommand, Api };

// OwnerTrusted: human at the keyboard, webserver off, no injector possible.
// WhitelistEnforced: webserver on, or autoexec-driven mount.
enum class DirMountPolicy { WhitelistEnforced, OwnerTrusted };

namespace MountPolicy {

// Primitives

std::optional<std::filesystem::path> CanonicalizeExisting(
        const std::filesystem::path& host_path);

bool HasSymlinkComponent(const std::filesystem::path& canonical_path);

bool IsUnderSystemPath(const std::filesystem::path& canonical_path);

bool IsUnderAnyRoot(const std::filesystem::path& canonical_path,
                    const std::vector<std::filesystem::path>& roots);

bool ValidateDiskImageStructure(const std::filesystem::path& host_path);

// Windows reserved-name and denylist construction (aug-4tvb). Pure
// string logic compiled on every platform so the tests run everywhere;
// only the validator call sites are Windows-gated. The reserved-name
// check runs BEFORE canonicalize: stat'ing COM1 can block on a real
// serial port, so a late check would itself be the denial of service.
bool IsWindowsReservedDeviceName(const std::string& path_component);
bool HasWindowsReservedComponent(const std::filesystem::path& raw_path);
std::vector<std::filesystem::path> BuildWindowsSystemPaths(
        const std::function<std::string(const char*)>& get_env);

// Policy entry points

MountVerdict ValidateDirectoryMount(const std::filesystem::path& raw_path,
                                    const std::filesystem::path& conf_anchor,
                                    const std::vector<std::filesystem::path>& allowed_bases,
                                    DirMountPolicy policy);

// conf_anchor counts as an allowed root wherever the whitelist applies,
// matching ValidateDirectoryMount. Defaults to empty, which only ever
// blocks more. Guest commands are whitelisted too once an injector exists,
// so the caller states where the command came from, not whether to trust it.
MountVerdict ValidateImagePath(const std::filesystem::path& raw_path,
                               MountOrigin origin,
                               const std::vector<std::filesystem::path>& allowed_image_roots,
                               const std::filesystem::path& conf_anchor = {});

struct ArchiveVerdict {
	MountVerdict verdict          = {};
	ArchiveMount::HeadSniff sniff = {};
};

// ValidateImagePath's six host-path steps, then the head sniff as the
// structure gate (no signature: NotAnArchive; sequential: refused unless
// allowed). The index verdict is the helper's Open, see the mapping below.
ArchiveVerdict ValidateArchivePath(
        const std::filesystem::path& raw_path, MountOrigin origin,
        const std::vector<std::filesystem::path>& allowed_image_roots,
        const std::filesystem::path& conf_anchor, bool sequential_allowed);

DenyReason DenyReasonForArchiveError(ArchiveMount::Error error);

#if defined(WIN32)
bool IsDeviceNamespacePath(const std::string& path);
#endif

// One-way latch. Once locked, all mounts are refused: directory mounts,
// image mounts (MOUNT/IMGMOUNT), and BOOT. Cannot be unlocked.
void Lock();
bool IsLocked();

// Whether anything other than a human at the keyboard can drive the guest.
// A guest MOUNT or BOOT is only trusted while this is false.
void SetInjectionPossible(bool possible);
bool IsInjectionPossible();

// Read allowed_bases and allowed_image_roots from the primary config
// file. Called once at startup. Values are read-only after init.
void InitPolicyConfig(const std::filesystem::path& primary_config_path);
const std::vector<std::filesystem::path>& AllowedBases();
const std::vector<std::filesystem::path>& AllowedImageRoots();

// Parent directory of the last -conf file on the command line, canonical.
// Empty when no -conf was loaded. The third policy root, alongside the two
// accessors above.
std::filesystem::path ConfAnchor();

// Test-only observation hooks (aug-2yza), null in production. The unit
// tests arm both to assert the sink invariant: no drive is constructed
// from a host path without a preceding allowed verdict for that path.
using VerdictHookFn = void (*)(const std::filesystem::path& raw_path,
                               const MountVerdict& verdict);
extern VerdictHookFn verdict_hook;

using DriveConstructionHookFn = void (*)(const char* host_path);
extern DriveConstructionHookFn drive_construction_hook;

struct PolicyPaths {
	std::vector<std::filesystem::path> allowed_bases       = {};
	std::vector<std::filesystem::path> allowed_image_roots = {};
};
PolicyPaths ParsePolicyConfig(const std::filesystem::path& config_path);

// A folder that exists must never be reported as missing.
const char* DenyReasonText(DenyReason reason);
const char* DenyMessageId(DenyReason reason);

// A path argument as the host's UTF-8: arguments arrive in the DOS code
// page, typed or from AUTOEXEC, which autoexec.cpp converts from the conf
// (G13).
std::string HostPathFromDos(const std::string& dos_path);

} // namespace MountPolicy

#endif // DOSBOX_PROGRAM_MOUNT_POLICY_H
