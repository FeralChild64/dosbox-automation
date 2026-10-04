// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//
#include "dos/archive/descriptor.h"

#include <fcntl.h>
#include <sys/stat.h>

#if defined(WIN32)
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace ArchiveMount {

int DuplicateNoInherit(const int fd)
{
#if defined(WIN32)
	// _dup would make the copy inheritable until a later flag clear;
	// DuplicateHandle never does.
	const auto source = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
	if (source == INVALID_HANDLE_VALUE) {
		return -1;
	}
	HANDLE copy = INVALID_HANDLE_VALUE;
	if (!DuplicateHandle(GetCurrentProcess(),
	                     source,
	                     GetCurrentProcess(),
	                     &copy,
	                     0,
	                     FALSE,
	                     DUPLICATE_SAME_ACCESS)) {
		return -1;
	}
	const auto copy_fd = _open_osfhandle(reinterpret_cast<intptr_t>(copy),
	                                     _O_RDONLY | _O_BINARY | _O_NOINHERIT);
	if (copy_fd < 0) {
		CloseHandle(copy);
	}
	return copy_fd;
#else
	return fcntl(fd, F_DUPFD_CLOEXEC, 0);
#endif
}

int OpenReadOnlyShared(const std::filesystem::path& path)
{
#if defined(WIN32)
	const auto handle = CreateFileW(path.wstring().c_str(),
	                                GENERIC_READ,
	                                FILE_SHARE_READ | FILE_SHARE_WRITE |
	                                        FILE_SHARE_DELETE,
	                                nullptr,
	                                OPEN_EXISTING,
	                                FILE_ATTRIBUTE_NORMAL,
	                                nullptr);
	if (handle == INVALID_HANDLE_VALUE) {
		return -1;
	}
	if (GetFileType(handle) != FILE_TYPE_DISK) {
		CloseHandle(handle);
		return -1;
	}
	const auto fd = _open_osfhandle(reinterpret_cast<intptr_t>(handle),
	                                _O_RDONLY | _O_BINARY | _O_NOINHERIT);
	if (fd < 0) {
		CloseHandle(handle);
	}
	return fd;
#else
	// O_NONBLOCK keeps a FIFO at the path from holding the open; the type
	// is judged on the descriptor, then the flag goes again.
	const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
	if (fd < 0) {
		return -1;
	}
	struct stat st  = {};
	const int flags = fcntl(fd, F_GETFL);
	if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || flags < 0 ||
	    fcntl(fd, F_SETFL, flags & ~O_NONBLOCK) != 0) {
		close(fd);
		return -1;
	}
	return fd;
#endif
}

int OpenRegularFileNoFollow(const std::filesystem::path& path)
{
#if defined(WIN32)
	const auto handle = CreateFileW(path.wstring().c_str(),
	                                GENERIC_READ,
	                                FILE_SHARE_READ | FILE_SHARE_WRITE |
	                                        FILE_SHARE_DELETE,
	                                nullptr,
	                                OPEN_EXISTING,
	                                FILE_FLAG_OPEN_REPARSE_POINT,
	                                nullptr);
	if (handle == INVALID_HANDLE_VALUE) {
		return -1;
	}
	BY_HANDLE_FILE_INFORMATION info = {};
	constexpr DWORD not_plain       = FILE_ATTRIBUTE_DIRECTORY |
	                            FILE_ATTRIBUTE_REPARSE_POINT;
	if (GetFileType(handle) != FILE_TYPE_DISK ||
	    !GetFileInformationByHandle(handle, &info) ||
	    (info.dwFileAttributes & not_plain) != 0) {
		CloseHandle(handle);
		return -1;
	}
	const auto fd = _open_osfhandle(reinterpret_cast<intptr_t>(handle),
	                                _O_RDONLY | _O_BINARY | _O_NOINHERIT);
	if (fd < 0) {
		CloseHandle(handle);
	}
	return fd;
#else
	const auto fd = open(path.c_str(),
	                     O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
	if (fd < 0) {
		return -1;
	}
	struct stat st = {};
	if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
		close(fd);
		return -1;
	}
	return fd;
#endif
}

bool StatDescriptor(const int fd, StatPair& out)
{
#if defined(WIN32)
	struct _stat64 st = {};
	if (_fstat64(fd, &st) != 0) {
		return false;
	}
#else
	struct stat st = {};
	if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
		return false;
	}
#endif
	out.size  = static_cast<int64_t>(st.st_size);
	out.mtime = static_cast<int64_t>(st.st_mtime);
	return true;
}

void CloseDescriptor(const int fd)
{
	if (fd < 0) {
		return;
	}
#if defined(WIN32)
	_close(fd);
#else
	close(fd);
#endif
}

} // namespace ArchiveMount
