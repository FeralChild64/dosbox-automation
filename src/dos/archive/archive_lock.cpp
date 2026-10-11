// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//
#include "dos/archive/archive_lock.h"

#include "utils/checks.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <utility>

#if defined(WIN32)
#include <io.h>
#include <windows.h>
#else
#include <sys/file.h>
#include <unistd.h>
#endif

CHECK_NARROWING();

namespace ArchiveMount {

namespace {

int OpenLockFile(const std::filesystem::path& path, const bool create = true)
{
#if defined(WIN32)
	// _O_NOINHERIT so no helper the engine spawns carries the handle.
	return _wopen(path.wstring().c_str(),
	              _O_RDWR | _O_BINARY | _O_NOINHERIT | (create ? _O_CREAT : 0),
	              _S_IREAD | _S_IWRITE);
#else
	return open(path.c_str(),
	            O_RDWR | O_CLOEXEC | O_NOFOLLOW | (create ? O_CREAT : 0),
	            0600);
#endif
}

bool Lock(const int fd, const FileLock::Mode mode)
{
#if defined(WIN32)
	const auto handle = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
	if (handle == INVALID_HANDLE_VALUE) {
		return false;
	}
	OVERLAPPED overlapped = {};
	DWORD flags           = LOCKFILE_FAIL_IMMEDIATELY;
	if (mode == FileLock::Mode::Exclusive) {
		flags |= LOCKFILE_EXCLUSIVE_LOCK;
	}
	// One byte at offset 0 is the whole lock; the file stays empty.
	return LockFileEx(handle, flags, 0, 1, 0, &overlapped) != 0;
#else
	const int operation = (mode == FileLock::Mode::Exclusive ? LOCK_EX : LOCK_SH) |
	                      LOCK_NB;
	return flock(fd, operation) == 0;
#endif
}

void Unlock(const int fd)
{
#if defined(WIN32)
	const auto handle = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
	if (handle != INVALID_HANDLE_VALUE) {
		OVERLAPPED overlapped = {};
		UnlockFileEx(handle, 0, 1, 0, &overlapped);
	}
	_close(fd);
#else
	// close drops the flock with the last descriptor of the description.
	close(fd);
#endif
}

} // namespace

std::optional<FileLock> FileLock::TryAcquire(const std::filesystem::path& path,
                                             const Mode mode)
{
	const int fd = OpenLockFile(path);
	if (fd < 0) {
		return {};
	}
	if (!Lock(fd, mode)) {
#if defined(WIN32)
		_close(fd);
#else
		close(fd);
#endif
		return {};
	}
	return FileLock(fd, mode, path);
}

bool FileLock::IsHeld(const std::filesystem::path& path)
{
	// Holders create the file, so a missing one is held by nobody. The
	// exclusive try lasts only until Unlock, the same window Cleanup has.
	const int fd = OpenLockFile(path, false);
	if (fd < 0) {
		return false;
	}
	const bool free = Lock(fd, Mode::Exclusive);
	Unlock(fd);
	return !free;
}

FileLock::FileLock(const int fd, const Mode mode, std::filesystem::path path)
        : fd(fd),
          mode(mode),
          path(std::move(path))
{}

FileLock::~FileLock()
{
	Release();
}

FileLock::FileLock(FileLock&& other) noexcept
        : fd(other.fd),
          mode(other.mode),
          path(std::move(other.path))
{
	other.fd = -1;
}

FileLock& FileLock::operator=(FileLock&& other) noexcept
{
	if (this != &other) {
		Release();
		fd       = other.fd;
		mode     = other.mode;
		path     = std::move(other.path);
		other.fd = -1;
	}
	return *this;
}

void FileLock::Release()
{
	if (fd >= 0) {
		Unlock(fd);
		fd = -1;
	}
}

} // namespace ArchiveMount
