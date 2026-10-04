// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//
#ifndef DOSBOX_ARCHIVE_DESCRIPTOR_H
#define DOSBOX_ARCHIVE_DESCRIPTOR_H

#include <filesystem>

#include "dos/archive/archive_source.h"

namespace ArchiveMount {

// Duplicates a descriptor so that no child process the engine spawns
// inherits it, with no window in which it could: F_DUPFD_CLOEXEC on
// POSIX, DuplicateHandle without inheritance on Windows. -1 on failure.
int DuplicateNoInherit(int fd);

// The one way the archive code opens an archive: read only, never
// inherited, and on Windows with share-delete (design D16(a)) so a
// rename or delete of the mounted file behaves as on POSIX. A regular
// file only: a FIFO or a device answers -1 without waiting.
int OpenReadOnlyShared(const std::filesystem::path& path);

// Opens a regular file read only without following a link at the path and
// without blocking on a FIFO; the type is judged on the open file, so a
// swap after a check by path cannot slip through. -1 for anything else.
int OpenRegularFileNoFollow(const std::filesystem::path& path);

// The size and mtime of an open file, the pair the index records (1.6).
bool StatDescriptor(int fd, StatPair& out);

void CloseDescriptor(int fd);

} // namespace ArchiveMount

#endif // DOSBOX_ARCHIVE_DESCRIPTOR_H
