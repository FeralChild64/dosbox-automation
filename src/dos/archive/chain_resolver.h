// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//
#ifndef DOSBOX_ARCHIVE_CHAIN_RESOLVER_H
#define DOSBOX_ARCHIVE_CHAIN_RESOLVER_H

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "dos/archive/archive_cache.h"
#include "dos/archive/archive_helper_client.h"
#include "dos/archive/archive_source.h"
#include "dos/archive/member_path.h"

namespace ArchiveMount {

// A resolved intermediate archive in the chain: the lease keeps the
// cached copy alive, the client reads it.
struct ChainLink {
	std::unique_ptr<ArchiveCache::Lease> lease  = {};
	std::unique_ptr<ArchiveHelperClient> client = {};
	std::filesystem::path cached_path           = {};
	std::string resolved_member                 = {};
};

struct ChainResult {
	// Intermediate archives in opening order; the hold releases them
	// innermost first.
	std::vector<ChainLink> links = {};
	// The final archive's client, from which the caller picks the leaf
	// member. This is the last link's client when nested, or the host's
	// client when not nested.
	ArchiveHelperClient* leaf_client = nullptr;
	// The archive path the leaf client reads (cached copy or host).
	std::filesystem::path leaf_archive = {};
	// N7: total entries across all levels.
	int64_t total_entries = 0;

	Error error        = Error::None;
	std::string detail = {};
	int failed_level   = -1;
};

struct ChainOptions {
	ArchiveHelperClient::Options helper_options = {};
	Limits limits                               = {};
	// N7: the total entries across all levels.
	int64_t max_total_entries = 65536;
	// N8: D5 per level. When false, a sequential intermediate archive
	// is refused.
	bool sequential_allowed = false;
};

// The levels' archive members joined with '/': "" when not nested.
std::string ChainPrefix(const ChainedPath& chain);

// Walks the levels: per intermediate archive, find the member, lease it
// from the cache, open a helper on the cached copy. The host client is
// the caller's, already opened for the policy check.
ChainResult ResolveChain(const ChainedPath& chain, ArchiveHelperClient& host_client,
                         const std::filesystem::path& host_archive,
                         ArchiveCache& cache, const ChainOptions& options);

} // namespace ArchiveMount

#endif // DOSBOX_ARCHIVE_CHAIN_RESOLVER_H
