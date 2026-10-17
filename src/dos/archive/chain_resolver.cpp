// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//
#include "dos/archive/chain_resolver.h"

#include "augra/log.h"
#include "dos/archive/archive_tree.h"
#include "dos/archive/entry_names.h"
#include "dos/archive/member_path.h"
#include "utils/checks.h"

#include <cstddef>
#include <limits>

CHECK_NARROWING();

namespace ArchiveMount {

namespace {

constexpr auto LogComponent = "mount";

} // namespace

std::string ChainPrefix(const ChainedPath& chain)
{
	auto out = std::string();
	for (const auto& level : chain.levels) {
		if (!out.empty()) {
			out += '/';
		}
		out += level.archive_member;
	}
	return out;
}

std::optional<int64_t> ComposeWindowOffset(const int64_t parent_offset,
                                           const int64_t stored_offset)
{
	if (parent_offset < 0 || stored_offset < 0 ||
	    stored_offset > std::numeric_limits<int64_t>::max() - parent_offset) {
		return {};
	}
	return parent_offset + stored_offset;
}

ChainedPath ChainOfLevels(const std::filesystem::path& host,
                          const std::vector<std::string>& members)
{
	ChainedPath out  = {};
	out.host.archive = host;
	for (const auto& member : members) {
		ChainLevel level     = {};
		level.archive_member = member;
		out.levels.push_back(std::move(level));
	}
	if (!out.levels.empty()) {
		out.host.member = out.levels.front().archive_member;
	}
	return out;
}

ChainResult ResolveChain(const ChainedPath& chain, ArchiveHelperClient& host_client,
                         const std::filesystem::path& host_archive,
                         ArchiveCache& cache, const ChainOptions& options)
{
	ChainResult out   = {};
	out.total_entries = static_cast<int64_t>(host_client.Entries().size());
	out.chain         = chain;
	auto& levels      = out.chain.levels;

	auto* current_client = &host_client;
	auto current_archive = host_archive;

	// N8: a sequential archive is unpacked whole to reach one member, so
	// the setting is asked before each level is extracted from.
	const auto refuse_sequential = [&](const size_t level,
	                                   const std::string& member_name) {
		out.error        = Error::SolidUnsupported;
		out.failed_level = static_cast<int>(level);
		out.detail = member_name + ": sequential archive at level " +
		             std::to_string(level);
	};

	// N4 at the host and at every level (G11, G16): the name the next step
	// looks up, absent from a house archive's own listing, is looked for in
	// its store; a drive enters a house archive it ends on (N3).
	auto entered_store     = std::optional<size_t>();
	const auto enter_store = [&](const std::vector<Entry>& listing,
	                             const size_t at,
	                             const std::string& next,
	                             std::string leaf) {
		if (next.empty() && !options.enter_house_leaf) {
			return true;
		}
		if (!next.empty()) {
			auto rejection = ArchiveNames::Rejection::None;
			if (FindMember(listing, next, &rejection) ||
			    rejection != ArchiveNames::Rejection::None) {
				return true;
			}
		}
		const auto* house = HouseArchiveMember(listing);
		if (!house) {
			return true;
		}
		if (static_cast<int>(levels.size()) >= MaxNestingDepth) {
			out.error        = Error::EntryRejected;
			out.failed_level = static_cast<int>(at);
			out.detail = house->utf8_path + ": nesting depth exceeds " +
			             std::to_string(MaxNestingDepth);
			return false;
		}
		ChainLevel store     = {};
		store.archive_member = house->utf8_path;
		store.leaf_member    = std::move(leaf);
		levels.insert(levels.begin() + static_cast<std::ptrdiff_t>(at),
		              std::move(store));
		if (out.house_member.empty()) {
			out.house_member = house->utf8_path;
		}
		entered_store = at;
		augra::log_info(LogComponent,
		                "nested level %zu enters the store %s (N4)",
		                at,
		                house->utf8_path.c_str());
		return true;
	};

	if (!chain.host.lone) {
		const auto next = levels.empty() ? chain.host.member
		                                 : levels.front().archive_member;
		if (!enter_store(host_client.Entries(),
		                 0,
		                 next,
		                 levels.empty() ? next : std::string())) {
			return out;
		}
	}
	if (levels.empty()) {
		out.leaf_client  = &host_client;
		out.leaf_archive = host_archive;
		return out;
	}

	for (size_t level = 0; level < levels.size(); ++level) {
		const auto member_name = levels[level].archive_member;

		auto rejection    = ArchiveNames::Rejection::None;
		const auto* entry = FindMember(current_client->Entries(),
		                               member_name,
		                               &rejection);
		if (!entry) {
			out.error        = Error::NotFound;
			out.failed_level = static_cast<int>(level);
			if (rejection != ArchiveNames::Rejection::None) {
				out.detail = member_name + ": " +
				             ArchiveNames::RejectionText(rejection);
			} else {
				out.detail = member_name +
				             ": no such member in the archive"
				             " (level " +
				             std::to_string(level) + ")";
			}
			return out;
		}

		// Nested design 2.2 and N6: a stored zip member of a zip opens
		// over its own bytes; everything else goes through the cache.
		const bool windowable = current_client->GetFormat() == Format::Zip &&
		                        entry->stored_offset >= 0 && !entry->is_dir;
		ChainLink link       = {};
		link.resolved_member = member_name;
		if (windowable) {
			const auto parent = current_client->Window();
			const auto offset = ComposeWindowOffset(parent.offset,
			                                        entry->stored_offset);
			if (!offset) {
				out.error        = Error::Corrupt;
				out.failed_level = static_cast<int>(level);
				out.detail       = member_name +
				             ": stored offset past the file";
				return out;
			}
			auto opened = ArchiveHelperClient::OpenWindow(
			        *current_client,
			        *offset,
			        entry->size,
			        current_archive / std::filesystem::path(member_name),
			        options.limits,
			        options.helper_options);
			if (opened.error != Error::None) {
				out.error        = opened.error;
				out.failed_level = static_cast<int>(level);
				out.detail       = opened.detail;
				return out;
			}
			link.window = opened.client->Window();
			link.client = std::move(opened.client);
			current_archive /= std::filesystem::path(member_name);
		} else {
			if (!options.sequential_allowed &&
			    current_client->GetAccess() == Access::Sequential) {
				refuse_sequential(level, member_name);
				return out;
			}

			auto leased = cache.Acquire(*current_client, current_archive);
			if (leased.status != CacheStatus::Ok) {
				out.error        = Error::OpenFailed;
				out.failed_level = static_cast<int>(level);
				out.detail = std::string(CacheStatusText(leased.status)) +
				             ": " + leased.detail;
				return out;
			}

			auto file_path           = std::filesystem::path();
			auto detail              = std::string();
			const auto member_status = leased.lease->ServeMember(
			        entry->ordinal, file_path, &detail);
			if (member_status != CacheStatus::Ok) {
				out.error        = Error::OpenFailed;
				out.failed_level = static_cast<int>(level);
				out.detail = std::string(CacheStatusText(member_status)) +
				             ": " + detail;
				return out;
			}
			// The lease outlives the client it was served from.
			leased.lease->Detach();

			auto opened = ArchiveHelperClient::Open(file_path,
			                                        options.limits,
			                                        options.helper_options);
			if (opened.error != Error::None) {
				out.error        = opened.error;
				out.failed_level = static_cast<int>(level);
				out.detail       = opened.detail;
				return out;
			}
			link.lease       = std::move(leased.lease);
			link.client      = std::move(opened.client);
			link.cached_path = file_path;
			current_archive  = file_path;
		}

		// N7: the entry cap counts every level; a window adds no bytes.
		out.total_entries += static_cast<int64_t>(
		        link.client->Entries().size());
		if (out.total_entries > options.max_total_entries) {
			out.error        = Error::TooManyEntries;
			out.failed_level = static_cast<int>(level);
			out.detail       = "total entries across the chain (" +
			             std::to_string(out.total_entries) +
			             ") exceeds the cap (" +
			             std::to_string(options.max_total_entries) + ")";
			return out;
		}

		augra::log_info(LogComponent,
		                "nested level %zu: %s %s (%zu entries)",
		                level,
		                member_name.c_str(),
		                link.window ? "read in place" : "served from the cache",
		                link.client->Entries().size());

		current_client = link.client.get();
		out.links.push_back(std::move(link));

		// A store the rule entered is not itself searched for another.
		if (entered_store == level) {
			continue;
		}
		const bool last = level + 1 == levels.size();
		const auto next = last ? levels[level].leaf_member
		                       : levels[level + 1].archive_member;
		if (!enter_store(current_client->Entries(),
		                 level + 1,
		                 next,
		                 last ? next : std::string())) {
			return out;
		}
		if (last && entered_store == level + 1) {
			levels[level].leaf_member.clear();
		}
	}

	// The leaf's container is unpacked whole by Resolve: same rule.
	if (!options.sequential_allowed &&
	    current_client->GetAccess() == Access::Sequential) {
		refuse_sequential(levels.size(), levels.back().archive_member);
		return out;
	}

	out.leaf_window = !out.links.empty() && out.links.back().window.has_value();
	out.leaf_client  = current_client;
	out.leaf_archive = current_archive;
	return out;
}

} // namespace ArchiveMount
