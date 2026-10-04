// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//
#ifndef DOSBOX_ARCHIVE_READER_H
#define DOSBOX_ARCHIVE_READER_H

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "dos/archive/archive_source.h"

namespace ArchiveMount {

class ArchiveReader;

struct OpenResult {
	std::unique_ptr<ArchiveReader> reader = {};
	Error error                           = Error::None;
	std::string detail                    = {};
};

class ArchiveReader final : public ArchiveSource {
public:
	static OpenResult Open(const std::filesystem::path& path,
	                       const Limits& limits);
	// Over a duplicate of fd, which the caller keeps and may close at
	// once; name is for log lines only. The duplicate shares the file
	// offset, so the caller must not read through its own descriptor.
	static OpenResult Open(int fd, const std::filesystem::path& name,
	                       const Limits& limits);
	~ArchiveReader() override;
	ArchiveReader(const ArchiveReader&)            = delete;
	ArchiveReader& operator=(const ArchiveReader&) = delete;

	Format GetFormat() const override;
	Access GetAccess() const override;
	const std::vector<Entry>& Entries() const override;
	int64_t TotalBytes() const override;
	int64_t ArchiveLength() const override;
	const std::string& IdentityHash() const override;
	StatPair IndexedStat() const override;

	// One call at a time: every handle shares the archive's file offset
	// (asserted in debug builds).
	Error Materialize(int ordinal, std::vector<uint8_t>& out,
	                  std::string* detail) override;
	Error MaterializeToFile(int ordinal, const std::filesystem::path& target,
	                        std::string* detail) override;
	Error MaterializeAll(const std::filesystem::path& directory,
	                     std::string* detail) override;

private:
	// unique_ptr's deleter needs Impl complete, so the constructor lives
	// in the .cpp and the member carries no initializer (it is null).
	ArchiveReader();
	struct Impl;
	std::unique_ptr<Impl> impl;
};

} // namespace ArchiveMount

#endif // DOSBOX_ARCHIVE_READER_H
