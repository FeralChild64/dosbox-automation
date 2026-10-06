// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//
#include "shell/autoexec_archive.h"

#include "dos/archive/head_sniff.h"
#include "dos/archive/member_path.h"
#include "dos/programs/archive_images.h"
#include "dos/programs/mount_run.h"
#include "utils/checks.h"

#include <algorithm>
#include <cstdint>
#include <system_error>

CHECK_NARROWING();

namespace AutoexecArchive {

namespace {

// AUTOEXEC.BAT goes through the batch expansion, where a lone % ends the
// line and %1 or %VAR% are replaced.
std::string AsBatchText(const std::string_view text)
{
	std::string escaped = {};
	for (const char c : text) {
		escaped += c;
		if (c == '%') {
			escaped += '%';
		}
	}
	return escaped;
}

} // namespace

std::optional<Argument> Classify(const std::filesystem::path& path)
{
	std::error_code ec = {};
	if (!std::filesystem::is_regular_file(path, ec)) {
		return {};
	}
	if (ArchiveMount::IsArchiveFile(path)) {
		return Argument::Archive;
	}
	if (!ArchiveMount::IsSingleImageContainer(path)) {
		return {};
	}
	switch (*ArchiveMount::ContainerKindByName(path)) {
	case ArchiveMount::HeadKind::Isz: return Argument::CdContainer;
	case ArchiveMount::HeadKind::Zip: return Argument::DiskContainer;
	default: break;
	}
	// A compressed image is the kind of the name inside its filter.
	const auto inner = ArchiveMount::PathToUtf8(path.stem());
	return ArchiveImages::KindOf(inner) == ArchiveImages::Kind::CdImage
	             ? Argument::CdContainer
	             : Argument::DiskContainer;
}

std::optional<std::vector<std::string>> ArchiveLines(const std::string& path,
                                                     const std::string_view run)
{
	const auto unquotable = [](const char c) {
		const auto byte = static_cast<uint8_t>(c);
		return c == '"' || byte < 0x20 || byte == 0x7f;
	};
	if (path.empty() || std::any_of(path.begin(), path.end(), unquotable)) {
		return {};
	}
	auto mount = "@Z:\\MOUNT.COM C \"" + AsBatchText(path) + "\"";
	if (run.empty()) {
		mount += " -run -exit";
	} else if (MountRun::IsProgramName(run)) {
		mount += " -run " + AsBatchText(run) + " -exit";
	}
	// Reached only when MOUNT queued nothing: the prompt stands at C: as
	// for a directory argument.
	return std::vector<std::string>{"@Z:\\MOUNT.COM -u C >NUL", mount, "@C:"};
}

} // namespace AutoexecArchive
