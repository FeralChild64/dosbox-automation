// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//
#include "dos/archive/archive_helper_client.h"

#include <algorithm>
#include <mutex>
#include <optional>
#include <random>
#include <unordered_map>
#include <unordered_set>

#if defined(WIN32)
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "augra/log.h"
#include "dos/archive/descriptor.h"
#include "dos/archive/head_sniff.h"
#include "dos/archive/helper_protocol.h"
#include "misc/child_process.h"
#include "misc/support.h"
#include "utils/checks.h"

CHECK_NARROWING();

namespace ArchiveMount {

static std::filesystem::path helper_path_override = {};

void SetHelperPathOverride(const std::filesystem::path& path)
{
	helper_path_override = path;
}

using namespace ArchiveHelper;
using Clock = std::chrono::steady_clock;

namespace {

constexpr auto LogComponent = "archive";
constexpr auto QuitGrace    = std::chrono::seconds(2);

std::string path_to_utf8(const std::filesystem::path& p)
{
#if defined(WIN32)
	const auto u8 = p.u8string();
	return std::string(u8.begin(), u8.end());
#else
	return p.string();
#endif
}

bool read_whole(const std::filesystem::path& p, const int64_t expected,
                std::vector<uint8_t>& out)
{
	const int fd = OpenReadOnlyShared(p);
	if (fd < 0 || expected < 0) {
		CloseDescriptor(fd);
		return false;
	}
	std::error_code ec = {};
	if (std::filesystem::file_size(p, ec) != static_cast<uintmax_t>(expected) ||
	    ec) {
		CloseDescriptor(fd);
		return false;
	}
	out.resize(static_cast<size_t>(expected));
	size_t done = 0;
	bool ok     = true;
	for (;;) {
		uint8_t spare = 0;
		auto* into    = done < out.size() ? out.data() + done : &spare;
		const auto ask = done < out.size() ? out.size() - done : size_t{1};
#if defined(WIN32)
		const int n = _read(fd,
		                    into,
		                    static_cast<unsigned int>(
		                            std::min<size_t>(ask, 1u << 30)));
#else
		const auto n = read(fd, into, ask);
#endif
		if (n == 0) {
			break;
		}
		if (n < 0 || done == out.size()) {
			ok = false; // an error, or more bytes than declared
			break;
		}
		done += static_cast<size_t>(n);
	}
	CloseDescriptor(fd);
	return ok && done == out.size();
}

bool is_digits(const std::u8string_view text)
{
	return !text.empty() &&
	       std::all_of(text.begin(), text.end(), [](const char8_t c) {
		       return c >= u8'0' && c <= u8'9';
	       });
}

// The reader writes <name>.<32 hex>.part and renames it; a helper killed
// mid-write leaves that file behind. Only that exact shape is removed:
// name equal to only_name when given, else an ordinal (MaterializeAll).
void remove_part_files(const std::filesystem::path& dir,
                       const std::optional<std::u8string>& only_name)
{
	constexpr size_t HexDigits = 32;
	constexpr auto Tail        = std::u8string_view(u8".part");
	std::error_code ec         = {};
	auto it = std::filesystem::directory_iterator(dir, ec);
	for (; !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
		const auto file = it->path().filename().u8string();
		const auto name = std::u8string_view(file);
		if (name.size() < 1 + 1 + HexDigits + Tail.size() ||
		    !name.ends_with(Tail)) {
			continue;
		}
		const auto hex_at = name.size() - Tail.size() - HexDigits;
		const auto hex    = name.substr(hex_at, HexDigits);
		const auto stem   = name.substr(0, hex_at - 1);
		const auto is_hex = std::all_of(hex.begin(), hex.end(), [](const char8_t c) {
			return (c >= u8'0' && c <= u8'9') ||
			       (c >= u8'a' && c <= u8'f');
		});
		if (name[hex_at - 1] != u8'.' || !is_hex ||
		    (only_name ? stem != *only_name : !is_digits(stem))) {
			continue;
		}
		auto remove_ec = std::error_code();
		std::filesystem::remove(it->path(), remove_ec);
	}
}

enum class Scope { Member, All, Test };

struct Refusal {
	Error error        = Error::None;
	std::string detail = {};
};

void set_detail(std::string* detail, const std::string& text)
{
	if (detail) {
		*detail = text;
	}
}

std::chrono::milliseconds budget_of(const std::chrono::milliseconds base,
                                    const int64_t bytes, const int64_t bytes_per_ms)
{
	const auto extra = bytes_per_ms > 0
	                         ? std::max<int64_t>(bytes, 0) / bytes_per_ms
	                         : 0;
	return base + std::chrono::milliseconds(extra);
}

} // namespace

std::chrono::milliseconds OpenBudget(const ArchiveHelperClient::Options& options,
                                     const std::optional<Access> sniffed,
                                     const int64_t on_disk_bytes, const Limits& limits)
{
	if (sniffed == Access::PerMember) {
		return budget_of(options.base_timeout,
		                 on_disk_bytes,
		                 options.bytes_per_ms);
	}
	return budget_of(options.base_timeout,
	                 limits.max_total_bytes,
	                 options.sequential_bytes_per_ms);
}

std::chrono::milliseconds RequestBudget(const ArchiveHelperClient::Options& options,
                                        const Access access,
                                        const int64_t declared_bytes,
                                        const int64_t total_bytes)
{
	if (access == Access::PerMember) {
		return budget_of(options.base_timeout,
		                 declared_bytes,
		                 options.bytes_per_ms);
	}
	return budget_of(options.base_timeout,
	                 total_bytes,
	                 options.sequential_bytes_per_ms);
}

struct ArchiveHelperClient::Impl {
	// Every live client, for KillAll on the engine's exit paths.
	static inline std::mutex registry_mutex          = {};
	static inline std::unordered_set<Impl*> registry = {};

	std::filesystem::path archive = {};
	int archive_fd  = -1; // the descriptor the policy validated
	Limits limits   = {};
	Options options = {};
	std::unique_ptr<ChildProcess> child = {};
	int served                          = 0; // requests sent to this helper
	int respawns                        = 0;
	IndexReply index                    = {};
	std::unordered_map<int, Refusal> refused_members = {};
	std::optional<Refusal> refused_all               = {};
	std::optional<int> crash_hook                    = {};
	std::optional<int> hang_hook                     = {};
	bool hooks_persistent                            = false;
	bool closed                                      = false;
	// Set when an attempt of the last request ended without a reply: that
	// helper may have left a .part file even if a retry then succeeded.
	bool lost_a_helper = false;

	Impl()
	{
		std::lock_guard<std::mutex> lock(registry_mutex);
		registry.insert(this);
	}
	~Impl()
	{
		CloseDescriptor(archive_fd);
		std::lock_guard<std::mutex> lock(registry_mutex);
		registry.erase(this);
	}
	Impl(const Impl&)            = delete;
	Impl& operator=(const Impl&) = delete;

	// One request, one reply; false on any transport or framing failure.
	bool Exchange(const Type type, const std::vector<uint8_t>& payload,
	              const std::chrono::milliseconds budget, Type& reply_type,
	              std::vector<uint8_t>& reply, bool& timed_out)
	{
		timed_out = false;
		if (!child) {
			return false;
		}
		++served;
		if (!child->Write(EncodeFrame(type, payload))) {
			return false;
		}
		return Receive(budget, reply_type, reply, timed_out);
	}

	bool Receive(const std::chrono::milliseconds budget, Type& reply_type,
	             std::vector<uint8_t>& reply, bool& timed_out)
	{
		timed_out           = false;
		const auto deadline = Clock::now() + budget;
		std::vector<uint8_t> header(HeaderSize);
		if (!child->ReadExact(header.data(), header.size(), deadline, &timed_out)) {
			return false;
		}
		uint32_t length = 0;
		if (!DecodeHeader(header, MaxReplyPayload, reply_type, length)) {
			return false;
		}
		reply.resize(length);
		return length == 0 ||
		       child->ReadExact(reply.data(), length, deadline, &timed_out);
	}

	bool ExpectStatus(const Type type, const std::vector<uint8_t>& payload,
	                  const std::chrono::milliseconds budget,
	                  StatusReply& status, bool& timed_out)
	{
		Type reply_type = Type::Quit;
		std::vector<uint8_t> reply;
		if (!Exchange(type, payload, budget, reply_type, reply, timed_out)) {
			return false;
		}
		return reply_type == Type::StatusReply &&
		       DecodeStatusReply(reply, status);
	}

	// Spawn, Hello, Open. A respawned helper must report the identity of
	// the first open, or the archive changed under us.
	Error Spawn(const bool first, std::string* detail)
	{
		child.reset();
		served    = 0;
		auto argv = std::vector<std::string>{
		        path_to_utf8(options.helper_path),
		        "--memory-limit-mb",
		        std::to_string(options.memory_limit_mb)};
		if (options.test_hooks) {
			argv.emplace_back("--test-hooks");
		}
		auto request   = OpenRequest{};
		request.path   = path_to_utf8(archive);
		request.limits = limits;
#if defined(WIN32)
		// SDL spawns with inheritance on, so the child sees this value.
		// Another spawn in the window before CreateProcess inherits it
		// too (read-only); ada-05fj's handle list ends that.
		HANDLE inheritable = INVALID_HANDLE_VALUE;
		if (!DuplicateHandle(GetCurrentProcess(),
		                     reinterpret_cast<HANDLE>(_get_osfhandle(archive_fd)),
		                     GetCurrentProcess(),
		                     &inheritable,
		                     0,
		                     TRUE,
		                     DUPLICATE_SAME_ACCESS)) {
			set_detail(detail,
			           "cannot duplicate the archive handle for the helper");
			return Error::HelperFailed;
		}
		request.handle = static_cast<uint64_t>(
		        reinterpret_cast<uintptr_t>(inheritable));
#else
		request.descriptor_follows = true;
#endif
		auto start_detail = std::string();
		child             = ChildProcess::Start(argv,
                                            &start_detail,
                                            ChildProcess::StdinKind::Socket);
#if defined(WIN32)
		CloseHandle(inheritable); // the child has its copy, or never
		                          // started
#endif
		if (!child) {
			set_detail(detail,
			           "cannot start the archive helper: " + start_detail);
			return Error::HelperFailed;
		}

		Type reply_type = Type::Quit;
		std::vector<uint8_t> reply;
		bool timed_out = false;
		auto hello     = HelloReply{};
		if (!Exchange(Type::Hello, {}, options.base_timeout, reply_type, reply, timed_out) ||
		    reply_type != Type::HelloReply ||
		    !DecodeHelloReply(reply, hello) || hello.version != Version) {
			child.reset();
			set_detail(detail,
			           timed_out ? "the archive helper did not answer hello"
			                     : "the archive helper failed at hello");
			return timed_out ? Error::Timeout : Error::HelperFailed;
		}
		if (first) {
			augra::log_info(LogComponent,
			                "archive helper %s, %s",
			                path_to_utf8(options.helper_path).c_str(),
			                hello.libarchive_string.c_str());
		}

		auto on_disk = StatPair{};
		StatDescriptor(archive_fd, on_disk);
		// The sniff reads through the shared file position; it must be
		// done before the helper holds the same open file description.
		const auto budget = OpenBudget(options,
		                               SniffFile(archive_fd).access,
		                               on_disk.size,
		                               limits);
		++served;
#if defined(WIN32)
		const auto sent = child->Write(
		        EncodeFrame(Type::Open, EncodeOpenRequest(request)));
#else
		const auto sent = child->WriteWithDescriptor(
		        archive_fd,
		        EncodeFrame(Type::Open, EncodeOpenRequest(request)));
#endif
		if (!sent) {
			child.reset();
			set_detail(detail, "the archive helper died before open");
			return Error::HelperFailed;
		}
		if (!Receive(budget, reply_type, reply, timed_out)) {
			child.reset();
			set_detail(detail,
			           timed_out ? "the archive helper hung on open"
			                     : "the archive helper died on open");
			return timed_out ? Error::Timeout : Error::HelperFailed;
		}
		if (reply_type == Type::StatusReply) {
			auto status = StatusReply{};
			if (!DecodeStatusReply(reply, status) ||
			    status.error == Error::None) {
				child.reset();
				set_detail(detail,
				           "malformed status from the archive helper");
				return Error::HelperFailed;
			}
			// A helper without an open archive would answer every
			// later request with OpenFailed until its request cap.
			child.reset();
			set_detail(detail, status.detail);
			return status.error;
		}
		auto opened = IndexReply{};
		if (reply_type != Type::IndexReply ||
		    !DecodeIndexReply(reply, opened)) {
			child.reset();
			set_detail(detail, "malformed index from the archive helper");
			return Error::HelperFailed;
		}
		if (const auto broken = CheckIndex(opened, limits)) {
			child.reset();
			set_detail(detail, "the archive helper sent " + *broken);
			return Error::HelperFailed;
		}
		if (first) {
			index = std::move(opened);
		} else if (opened.identity != index.identity ||
		           opened.entries.size() != index.entries.size() ||
		           opened.total_bytes != index.total_bytes) {
			child.reset();
			set_detail(detail,
			           "the archive changed while the helper was restarted");
			return Error::Changed;
		}
		// A respawned helper's index is only compared: Entries() keeps
		// pointing at the first one.
		if (!first) {
			++respawns;
			if (hooks_persistent) {
				RearmHooks();
			}
		}
		return Error::None;
	}

	bool SendHook(const Type type, const uint32_t value)
	{
		auto w = Writer{};
		w.U32(value);
		auto status    = StatusReply{};
		bool timed_out = false;
		return ExpectStatus(type, w.Take(), options.base_timeout, status, timed_out) &&
		       status.error == Error::None;
	}

	void RearmHooks()
	{
		if (crash_hook) {
			SendHook(Type::TestCrashOn,
			         static_cast<uint32_t>(*crash_hook));
		}
		if (hang_hook) {
			SendHook(Type::TestHangOn, static_cast<uint32_t>(*hang_hook));
		}
	}

	Error CheckUnchanged(std::string* detail) const
	{
		auto now = StatPair{};
		if (!StatDescriptor(archive_fd, now) ||
		    now.size != index.stat.size || now.mtime != index.stat.mtime) {
			set_detail(detail,
			           "archive size or mtime changed since it was indexed");
			return Error::Changed;
		}
		return Error::None;
	}

	Error EnsureHelper(std::string* detail)
	{
		if (child && child->IsAlive() && served < RequestsPerInstance) {
			return Error::None;
		}
		return Spawn(false, detail);
	}

	const Refusal* RefusalFor(const Scope scope, const int ordinal) const
	{
		if (scope == Scope::All) {
			return refused_all ? &*refused_all : nullptr;
		}
		if (scope == Scope::Member) {
			const auto it = refused_members.find(ordinal);
			return it == refused_members.end() ? nullptr : &it->second;
		}
		return nullptr;
	}

	void Refuse(const Scope scope, const int ordinal, const Refusal& refusal)
	{
		if (scope == Scope::All) {
			refused_all = refusal;
		} else if (scope == Scope::Member) {
			refused_members[ordinal] = refusal;
		}
	}

	// A crash is retried once in a fresh helper, then the member is
	// refused; a hang is refused at once, a second wait would cost the
	// same clock again.
	Error StatusRequest(const Type type, const std::vector<uint8_t>& payload,
	                    const std::chrono::milliseconds budget,
	                    const Scope scope, const int ordinal, std::string* detail)
	{
		lost_a_helper = false;
		if (closed) {
			set_detail(detail, "the archive helper client is closed");
			return Error::HelperFailed;
		}
		if (const auto changed = CheckUnchanged(detail);
		    changed != Error::None) {
			return changed;
		}
		if (const auto* refusal = RefusalFor(scope, ordinal)) {
			set_detail(detail, refusal->detail);
			return refusal->error;
		}
		for (int attempt = 1; attempt <= 2; ++attempt) {
			if (const auto error = EnsureHelper(detail);
			    error != Error::None) {
				return error;
			}
			auto status    = StatusReply{};
			bool timed_out = false;
			if (ExpectStatus(type, payload, budget, status, timed_out)) {
				set_detail(detail, status.detail);
				return status.error;
			}
			child.reset();
			lost_a_helper = true;
			if (timed_out) {
				const auto refusal = Refusal{
				        Error::Timeout,
				        "the archive helper hung on this member and was killed"};
				Refuse(scope, ordinal, refusal);
				set_detail(detail, refusal.detail);
				return refusal.error;
			}
			augra::log_warn(LogComponent,
			                "archive helper died on a request, attempt %d",
			                attempt);
		}
		const auto refusal = Refusal{Error::HelperFailed,
		                             "the archive helper crashed twice on this member"};
		Refuse(scope, ordinal, refusal);
		set_detail(detail, refusal.detail);
		return refusal.error;
	}

	std::filesystem::path ScratchName(const int ordinal) const
	{
		std::random_device rd = {};
		auto dist = std::uniform_int_distribution<uint64_t>();
		return options.scratch_dir / ("m" + std::to_string(ordinal) +
		                              "-" + std::to_string(dist(rd)));
	}

	const Entry* Find(const int ordinal) const
	{
		for (const auto& e : index.entries) {
			if (e.ordinal == ordinal) {
				return &e;
			}
		}
		return nullptr;
	}

	bool Quit()
	{
		if (!child) {
			return true;
		}
		auto exited = true;
		if (child->IsAlive()) {
			child->Write(EncodeFrame(Type::Quit, {}));
			int exit_code = 0;
			exited        = child->WaitFor(QuitGrace, &exit_code);
			if (!exited) {
				child->Kill();
			}
		}
		child.reset();
		return exited;
	}
};

ArchiveHelperClient::ArchiveHelperClient() = default;

ArchiveHelperClient::~ArchiveHelperClient()
{
	if (impl && !impl->closed) {
		impl->Quit();
	}
}

ArchiveHelperClient::OpenResult ArchiveHelperClient::Open(
        const std::filesystem::path& path, const Limits& limits, const Options& options)
{
	auto result = OpenResult{};
	auto client = std::unique_ptr<ArchiveHelperClient>(new ArchiveHelperClient());
	client->impl          = std::make_unique<Impl>();
	client->impl->archive = path;
	client->impl->limits  = limits;
	client->impl->options = options;
	if (client->impl->options.helper_path.empty()) {
		client->impl->options.helper_path = DefaultHelperPath();
	}
	client->impl->archive_fd = OpenReadOnlyShared(path);
	if (client->impl->archive_fd < 0) {
		result.error  = Error::OpenFailed;
		result.detail = "open failed";
		return result;
	}
	result.error = client->impl->Spawn(true, &result.detail);
	if (result.error != Error::None) {
		return result; // the client's destructor quits the helper
	}
	result.client = std::move(client);
	return result;
}

std::filesystem::path ArchiveHelperClient::DefaultHelperPath()
{
	if (!helper_path_override.empty()) {
		return helper_path_override;
	}
#if defined(WIN32)
	return get_executable_path() / "dosbox-automation-archive-helper.exe";
#else
	return get_executable_path() / "dosbox-automation-archive-helper";
#endif
}

Format ArchiveHelperClient::GetFormat() const
{
	return impl->index.format;
}
Access ArchiveHelperClient::GetAccess() const
{
	return impl->index.access;
}
const std::vector<Entry>& ArchiveHelperClient::Entries() const
{
	return impl->index.entries;
}
int64_t ArchiveHelperClient::TotalBytes() const
{
	return impl->index.total_bytes;
}
int64_t ArchiveHelperClient::ArchiveLength() const
{
	return impl->index.archive_length;
}
const std::string& ArchiveHelperClient::IdentityHash() const
{
	return impl->index.identity;
}
StatPair ArchiveHelperClient::IndexedStat() const
{
	return impl->index.stat;
}
int ArchiveHelperClient::RespawnCount() const
{
	return impl->respawns;
}

Error ArchiveHelperClient::Materialize(const int ordinal, std::vector<uint8_t>& out,
                                       std::string* detail)
{
	out.clear();
	if (impl->options.scratch_dir.empty()) {
		set_detail(detail, "no scratch directory for the archive helper");
		return Error::HelperFailed;
	}
	const auto scratch = impl->ScratchName(ordinal);
	const auto error   = MaterializeToFile(ordinal, scratch, detail);
	if (error != Error::None) {
		return error;
	}
	const auto* entry  = impl->Find(ordinal);
	const auto ok      = entry && read_whole(scratch, entry->size, out);
	std::error_code ec = {};
	std::filesystem::remove(scratch, ec);
	if (!ok) {
		out.clear();
		set_detail(detail,
		           "the helper's file does not match the declared size");
		return Error::Corrupt;
	}
	return Error::None;
}

Error ArchiveHelperClient::MaterializeToFile(const int ordinal,
                                             const std::filesystem::path& target,
                                             std::string* detail)
{
	const auto* entry = ordinal >= 0 ? impl->Find(ordinal) : nullptr;
	if (!entry) {
		set_detail(detail,
		           "no entry with ordinal " + std::to_string(ordinal));
		return Error::NotFound;
	}
	auto request     = MaterializeRequest{};
	request.ordinal  = ordinal;
	request.target   = path_to_utf8(target);
	const auto error = impl->StatusRequest(Type::Materialize,
	                                       EncodeMaterializeRequest(request),
	                                       RequestBudget(impl->options,
	                                                     impl->index.access,
	                                                     entry->size,
	                                                     impl->index.total_bytes),
	                                       Scope::Member,
	                                       ordinal,
	                                       detail);
	if (impl->lost_a_helper) {
		remove_part_files(target.parent_path(),
		                  target.filename().u8string());
	}
	return error;
}

Error ArchiveHelperClient::MaterializeAll(const std::filesystem::path& directory,
                                          std::string* detail)
{
	auto request      = MaterializeAllRequest{};
	request.directory = path_to_utf8(directory);
	const auto error  = impl->StatusRequest(Type::MaterializeAll,
                                               EncodeMaterializeAllRequest(request),
                                               RequestBudget(impl->options,
                                                             impl->index.access,
                                                             impl->index.total_bytes,
                                                             impl->index.total_bytes),
                                               Scope::All,
                                               -1,
                                               detail);
	if (impl->lost_a_helper) {
		remove_part_files(directory, std::nullopt);
	}
	return error;
}

bool ArchiveHelperClient::Close()
{
	impl->closed = true;
	return impl->Quit();
}

void ArchiveHelperClient::KillAll()
{
	std::lock_guard<std::mutex> lock(Impl::registry_mutex);
	for (auto* client : Impl::registry) {
		if (client->child) {
			client->child->Kill();
		}
	}
}

int ArchiveHelperClient::LiveHelpers()
{
	std::lock_guard<std::mutex> lock(Impl::registry_mutex);
	int live = 0;
	for (auto* client : Impl::registry) {
		live += (client->child && client->child->IsAlive()) ? 1 : 0;
	}
	return live;
}

bool ArchiveHelperClient::ArmTestCrash(const int ordinal, const bool persistent)
{
	impl->crash_hook       = ordinal;
	impl->hooks_persistent = persistent;
	return impl->SendHook(Type::TestCrashOn, static_cast<uint32_t>(ordinal));
}

bool ArchiveHelperClient::ArmTestHang(const int ordinal, const bool persistent)
{
	impl->hang_hook        = ordinal;
	impl->hooks_persistent = persistent;
	return impl->SendHook(Type::TestHangOn, static_cast<uint32_t>(ordinal));
}

Error ArchiveHelperClient::TestAllocate(const int megabytes)
{
	auto w = Writer{};
	w.U32(static_cast<uint32_t>(megabytes));
	return impl->StatusRequest(Type::TestAllocate,
	                           w.Take(),
	                           impl->options.base_timeout,
	                           Scope::Test,
	                           -1,
	                           nullptr);
}

} // namespace ArchiveMount
