// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//
#ifndef DOSBOX_ARCHIVE_LOCK_H
#define DOSBOX_ARCHIVE_LOCK_H

#include <filesystem>
#include <optional>

namespace ArchiveMount {

// An advisory lock on a file of its own, never a directory descriptor
// (flock on a directory is fragile on NFS and FUSE; design 2.1). flock
// on POSIX, LockFileEx on Windows, non-blocking, released on death.
class FileLock {
public:
	enum class Mode { Shared, Exclusive };

	// Creates the file (0600) when missing, never follows a link at the
	// path. Nothing when another holder conflicts or on any error.
	static std::optional<FileLock> TryAcquire(const std::filesystem::path& path,
	                                          Mode mode);
	// Someone holds the lock in either mode; creates nothing, keeps nothing.
	static bool IsHeld(const std::filesystem::path& path);

	~FileLock();
	FileLock(FileLock&& other) noexcept;
	FileLock& operator=(FileLock&& other) noexcept;
	FileLock(const FileLock&)            = delete;
	FileLock& operator=(const FileLock&) = delete;

	Mode GetMode() const
	{
		return mode;
	}
	const std::filesystem::path& Path() const
	{
		return path;
	}

private:
	FileLock(int fd, Mode mode, std::filesystem::path path);
	void Release();

	int fd                     = -1;
	Mode mode                  = Mode::Shared;
	std::filesystem::path path = {};
};

} // namespace ArchiveMount

#endif // DOSBOX_ARCHIVE_LOCK_H
