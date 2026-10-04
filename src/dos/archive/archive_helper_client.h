// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//
#ifndef DOSBOX_ARCHIVE_HELPER_CLIENT_H
#define DOSBOX_ARCHIVE_HELPER_CLIENT_H

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "dos/archive/archive_source.h"

namespace ArchiveMount {

// Where the tests put the helper; the engine never sets it. Empty
// restores the default (beside the executable).
void SetHelperPathOverride(const std::filesystem::path& path);

// ArchiveSource over the helper process (design D15), one helper per
// archive. A crash is retried once in a fresh helper; a second crash or a
// hang refuses that member for the mount's life.
class ArchiveHelperClient final : public ArchiveSource {
public:
	struct Options {
		std::filesystem::path helper_path = {}; // empty: beside the engine
		std::filesystem::path scratch_dir = {}; // Materialize lands here
		int64_t memory_limit_mb = 1024;
		// Per request: base plus 1 ms per bytes_per_ms of declared
		// size, so a 2 GiB member gets about 160 s.
		std::chrono::milliseconds base_timeout = std::chrono::seconds(30);
		int64_t bytes_per_ms = 16 * 1024;
		// Sequential formats inflate the whole stream to index or reach
		// a member, and bzip2 runs at 27 MB/s (2026-09-30): 8 KiB per ms
		// over the uncompressed bound, the cap at Open, the total after.
		int64_t sequential_bytes_per_ms = 8 * 1024;
		bool test_hooks = false; // never set by the engine
	};

	struct OpenResult {
		std::unique_ptr<ArchiveHelperClient> client = {};
		Error error                                 = Error::None;
		std::string detail                          = {};
	};

	// Call Open and every later request from a thread that lives as long
	// as the mount: on Linux the helper dies with the thread that started it.
	static OpenResult Open(const std::filesystem::path& path,
	                       const Limits& limits, const Options& options);
	static std::filesystem::path DefaultHelperPath();
	~ArchiveHelperClient() override;
	ArchiveHelperClient(const ArchiveHelperClient&)            = delete;
	ArchiveHelperClient& operator=(const ArchiveHelperClient&) = delete;

	Format GetFormat() const override;
	Access GetAccess() const override;
	const std::vector<Entry>& Entries() const override;
	int64_t TotalBytes() const override;
	int64_t ArchiveLength() const override;
	const std::string& IdentityHash() const override;
	StatPair IndexedStat() const override;

	Error Materialize(int ordinal, std::vector<uint8_t>& out,
	                  std::string* detail) override;
	Error MaterializeToFile(int ordinal, const std::filesystem::path& target,
	                        std::string* detail) override;
	Error MaterializeAll(const std::filesystem::path& directory,
	                     std::string* detail) override;

	// Sends Quit and waits two seconds; false when the helper had to be
	// killed. Every later request is refused.
	bool Close();
	int RespawnCount() const;

	// Kills every live helper outright, for the engine's exit paths; only
	// once no thread drives a client any more (the registry guards the
	// set, not each client's child).
	static void KillAll();
	static int LiveHelpers();

	// Honoured only with options.test_hooks. Persistent hooks are re-armed
	// in every respawned helper.
	bool ArmTestCrash(int ordinal, bool persistent);
	bool ArmTestHang(int ordinal, bool persistent);
	Error TestAllocate(int megabytes);

private:
	// unique_ptr's deleter needs Impl complete: constructor in the .cpp.
	ArchiveHelperClient();
	struct Impl;
	std::unique_ptr<Impl> impl;
};

// The Open clock: sequential (or unknown) heads walk the whole stream,
// bounded by limits.max_total_bytes; per-member heads by the file's size.
std::chrono::milliseconds OpenBudget(const ArchiveHelperClient::Options& options,
                                     std::optional<Access> sniffed,
                                     int64_t on_disk_bytes, const Limits& limits);

// A request's clock from the archive's class: the member's declared
// size for per-member formats, the declared total for sequential ones.
std::chrono::milliseconds RequestBudget(const ArchiveHelperClient::Options& options,
                                        Access access, int64_t declared_bytes,
                                        int64_t total_bytes);

} // namespace ArchiveMount

#endif // DOSBOX_ARCHIVE_HELPER_CLIENT_H
