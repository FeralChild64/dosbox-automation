// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//
#include "dos/archive/archive_activity.h"

#include <algorithm>
#include <map>
#include <mutex>
#include <system_error>
#include <utility>
#include <vector>

namespace ArchiveMount {

std::optional<std::u8string> PartFileStem(const std::u8string_view name)
{
	constexpr size_t HexDigits = 32;
	constexpr auto Tail        = std::u8string_view(u8".part");
	if (name.size() < 1 + 1 + HexDigits + Tail.size() || !name.ends_with(Tail)) {
		return {};
	}
	const auto hex_at = name.size() - Tail.size() - HexDigits;
	const auto hex    = name.substr(hex_at, HexDigits);
	const auto is_hex = std::all_of(hex.begin(), hex.end(), [](const char8_t c) {
		return (c >= u8'0' && c <= u8'9') || (c >= u8'a' && c <= u8'f');
	});
	if (name[hex_at - 1] != u8'.' || !is_hex) {
		return {};
	}
	return std::u8string(name.substr(0, hex_at - 1));
}

namespace Activity {

Observer observer = nullptr;

namespace {

std::mutex records_mutex                           = {};
std::vector<std::pair<uint64_t, Snapshot>> records = {};
uint64_t next_id                                   = 1;

// Without following links: a symlink in a scratch folder is not ours.
std::optional<int64_t> RegularFileSize(const std::filesystem::path& path)
{
	std::error_code ec = {};
	const auto status  = std::filesystem::symlink_status(path, ec);
	if (ec || !std::filesystem::is_regular_file(status)) {
		return {};
	}
	const auto size = std::filesystem::file_size(path, ec);
	if (ec) {
		return {};
	}
	return static_cast<int64_t>(size);
}

// A member counts once, by its larger file: a failed pass leaves finished
// members, and the next pass writes each again beside them.
int64_t FolderBytes(const std::filesystem::path& folder)
{
	std::map<std::u8string, int64_t> per_member = {};
	std::error_code ec                          = {};
	auto it = std::filesystem::directory_iterator(folder, ec);
	for (; !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
		const auto size = RegularFileSize(it->path());
		if (!size) {
			continue;
		}
		const auto name = it->path().filename().u8string();
		auto& counted   = per_member[PartFileStem(name).value_or(name)];
		counted         = std::max(counted, *size);
	}
	int64_t total = 0;
	for (const auto& [member, bytes] : per_member) {
		total += bytes;
	}
	return total;
}

int64_t MemberBytes(const std::filesystem::path& target)
{
	const auto name = target.filename().u8string();
	auto newest     = std::filesystem::file_time_type::min();
	std::optional<int64_t> part_bytes = {};
	std::error_code ec                = {};
	auto it = std::filesystem::directory_iterator(target.parent_path(), ec);
	for (; !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
		if (PartFileStem(it->path().filename().u8string()) != name) {
			continue;
		}
		const auto size         = RegularFileSize(it->path());
		std::error_code time_ec = {};
		const auto time = std::filesystem::last_write_time(it->path(),
		                                                   time_ec);
		if (size && !time_ec && (!part_bytes || time > newest)) {
			newest     = time;
			part_bytes = size;
		}
	}
	if (part_bytes) {
		return *part_bytes;
	}
	// After the rename the finished file holds the bytes until the guard ends.
	return RegularFileSize(target).value_or(0);
}

const char* KindName(const Kind kind)
{
	return kind == Kind::Identifying ? "identifying" : "materializing";
}

} // namespace

Guard::Guard(Snapshot snapshot)
{
	if (observer) {
		observer(snapshot);
	}
	std::lock_guard<std::mutex> lock(records_mutex);
	id = next_id++;
	records.emplace_back(id, std::move(snapshot));
}

Guard::~Guard()
{
	std::lock_guard<std::mutex> lock(records_mutex);
	std::erase_if(records,
	              [this](const auto& record) { return record.first == id; });
}

std::optional<Snapshot> Current()
{
	std::lock_guard<std::mutex> lock(records_mutex);
	if (records.empty()) {
		return {};
	}
	return records.back().second;
}

int64_t BytesDone(const Snapshot& snapshot)
{
	if (snapshot.kind == Kind::Identifying || snapshot.target.empty()) {
		return 0;
	}
	return snapshot.target_is_folder ? FolderBytes(snapshot.target)
	                                 : MemberBytes(snapshot.target);
}

std::optional<Status> CurrentStatus()
{
	// The folder walk runs outside the lock, on the copy.
	const auto snapshot = Current();
	if (!snapshot) {
		return {};
	}
	Status status      = {};
	status.activity    = KindName(snapshot->kind);
	status.archive     = snapshot->archive;
	status.bytes_done  = BytesDone(*snapshot);
	status.bytes_total = snapshot->bytes_total;
	return status;
}

} // namespace Activity

} // namespace ArchiveMount
