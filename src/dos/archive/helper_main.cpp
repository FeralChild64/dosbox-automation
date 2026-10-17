// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//

// dosbox-automation-archive-helper: parses archives for the engine (design
// D15). Request frames on stdin, replies on stdout; exits on EOF (the
// engine is gone), after RequestsPerInstance requests, or on a bad frame.

#include <archive.h>

#include <cerrno>
#include <charconv>
#include <chrono>
#include <clocale>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if defined(WIN32)
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <cstring>
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>
#endif
#if defined(__linux__)
#include <csignal>
#include <sys/prctl.h>
#elif defined(__FreeBSD__)
#include <csignal>
#include <sys/procctl.h>
#endif

#include "dos/archive/archive_reader.h"
#include "dos/archive/descriptor.h"
#include "dos/archive/helper_protocol.h"
#include "dos/archive/shadow_sanitizer.h"

using namespace ArchiveHelper;
using ArchiveMount::ArchiveReader;
using ArchiveMount::Error;

namespace {

constexpr const char* HelperVersion = "1";

// The exit codes the client and the tests read.
constexpr int ExitClean       = 0;
constexpr int ExitProtocol    = 2;
constexpr int ExitNoMemoryCap = 3;

int protocol_out = 1;

#if !defined(WIN32)
bool stdin_is_socket   = false;
int pending_descriptor = -1; // arrived with a frame, taken by Open

// One recvmsg on a socket stdin: the bytes as read() would give them,
// plus a descriptor when one rides along (kept, replacing any earlier).
ssize_t receive_some(uint8_t* out, const size_t size)
{
	if (!stdin_is_socket) {
		return read(0, out, size);
	}
	auto iov       = iovec{out, size};
	auto msg       = msghdr{};
	msg.msg_iov    = &iov;
	msg.msg_iovlen = 1;
	alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))] = {};
	msg.msg_control                                        = control;
	msg.msg_controllen = sizeof(control);
#if defined(MSG_CMSG_CLOEXEC)
	const auto n = recvmsg(0, &msg, MSG_CMSG_CLOEXEC);
#else
	const auto n = recvmsg(0, &msg, 0);
#endif
	if (n <= 0) {
		return n;
	}
	for (auto* c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
		if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS) {
			continue;
		}
		int fd = -1;
		std::memcpy(&fd, CMSG_DATA(c), sizeof(int));
		if (pending_descriptor >= 0) {
			close(pending_descriptor);
		}
		pending_descriptor = fd;
#if !defined(MSG_CMSG_CLOEXEC)
		fcntl(pending_descriptor, F_SETFD, FD_CLOEXEC);
#endif
	}
	return n;
}
#endif

bool read_exact(uint8_t* out, size_t size)
{
	while (size > 0) {
#if defined(WIN32)
		const int n = _read(0, out, static_cast<unsigned int>(size));
#else
		const auto n = receive_some(out, size);
		if (n < 0 && errno == EINTR) {
			continue;
		}
#endif
		if (n <= 0) {
			return false;
		}
		out += n;
		size -= static_cast<size_t>(n);
	}
	return true;
}

bool write_all(const uint8_t* data, size_t size)
{
	while (size > 0) {
#if defined(WIN32)
		const int n = _write(protocol_out,
		                     data,
		                     static_cast<unsigned int>(size));
#else
		const auto n = write(protocol_out, data, size);
		if (n < 0 && errno == EINTR) {
			continue;
		}
#endif
		if (n <= 0) {
			return false;
		}
		data += n;
		size -= static_cast<size_t>(n);
	}
	return true;
}

bool send(const Type type, const std::vector<uint8_t>& payload)
{
	const auto frame = EncodeFrame(type, payload);
	return write_all(frame.data(), frame.size());
}

bool send_status(const Error error, const std::string& detail)
{
	return send(Type::StatusReply, EncodeStatusReply({error, detail}));
}

// Keeps the protocol on a private descriptor and points fd 1 at stderr,
// so a library that prints cannot corrupt a frame.
bool claim_stdout()
{
#if defined(WIN32)
	_setmode(0, _O_BINARY);
	_setmode(1, _O_BINARY);
	protocol_out = _dup(1);
	return protocol_out >= 0 && _dup2(2, 1) == 0;
#else
	protocol_out = dup(1);
	return protocol_out >= 0 && dup2(2, 1) == 1;
#endif
}

// An OS-enforced cap, so a decompression bomb dies here and not in swap.
bool apply_memory_limit(const int64_t megabytes)
{
	if (ShadowSanitizer) {
		std::fputs("archive helper: sanitizer build, memory cap not applied\n",
		           stderr);
		return true;
	}
	const auto bytes = static_cast<uint64_t>(megabytes) * 1024 * 1024;
#if defined(WIN32)
	auto* job = CreateJobObjectW(nullptr, nullptr);
	if (!job) {
		return false;
	}
	JOBOBJECT_EXTENDED_LIMIT_INFORMATION info = {};
	info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_PROCESS_MEMORY;
	info.ProcessMemoryLimit               = static_cast<SIZE_T>(bytes);
	return SetInformationJobObject(job,
	                               JobObjectExtendedLimitInformation,
	                               &info,
	                               sizeof(info)) &&
	       AssignProcessToJobObject(job, GetCurrentProcess());
#elif defined(__APPLE__)
	// macOS refuses a 1 GiB RLIMIT_AS for a process that already maps
	// ~400 GB (probe P10); only the hang clock bounds the helper there.
	(void)bytes;
	return true;
#else
	struct rlimit limit = {};
	limit.rlim_cur      = static_cast<rlim_t>(bytes);
	limit.rlim_max      = static_cast<rlim_t>(bytes);
	return setrlimit(RLIMIT_AS, &limit) == 0;
#endif
}

// A helper busy inside libarchive would outlive a crashed engine until its
// next read; the kernel ends it with the parent instead. A parent already
// gone before this call shows up as EOF on the first read.
void die_with_parent()
{
#if defined(__linux__)
	prctl(PR_SET_PDEATHSIG, SIGKILL);
#elif defined(__FreeBSD__)
	int signal_number = SIGKILL;
	procctl(P_PID, 0, PROC_PDEATHSIG_CTL, &signal_number);
#endif
}

std::filesystem::path path_from_utf8(const std::string& utf8)
{
#if defined(WIN32)
	return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
#else
	return std::filesystem::path(utf8);
#endif
}

struct Options {
	int64_t memory_limit_mb = 0;
	bool test_hooks         = false;
	bool version            = false;
	bool bad                = false;
};

std::optional<int64_t> parse_megabytes(const std::string_view text)
{
	int64_t value        = 0;
	const auto* end      = text.data() + text.size();
	const auto [ptr, ec] = std::from_chars(text.data(), end, value);
	if (ec != std::errc() || ptr != end || value <= 0 || value > 1024 * 1024) {
		return {};
	}
	return value;
}

Options parse(const int argc, char** argv)
{
	auto o = Options{};
	for (int i = 1; i < argc; ++i) {
		const auto arg = std::string_view(argv[i]);
		if (arg == "--version") {
			o.version = true;
		} else if (arg == "--test-hooks") {
			o.test_hooks = true;
		} else if (arg == "--memory-limit-mb" && i + 1 < argc) {
			const auto mb = parse_megabytes(argv[++i]);
			if (!mb) {
				o.bad = true;
			} else {
				o.memory_limit_mb = *mb;
			}
		} else {
			o.bad = true;
		}
	}
	return o;
}

struct TestHooks {
	std::optional<int> crash_on = {};
	std::optional<int> hang_on  = {};
};

void run_test_hooks(const TestHooks& hooks, const int ordinal)
{
	if (hooks.crash_on && *hooks.crash_on == ordinal) {
		std::abort();
	}
	if (hooks.hang_on && *hooks.hang_on == ordinal) {
		std::this_thread::sleep_for(std::chrono::seconds(600));
	}
}

// Touches every page so the cap decides, not overcommit.
Error allocate_and_touch(const uint32_t megabytes)
{
	const auto bytes = static_cast<size_t>(megabytes) * 1024 * 1024;
	auto block       = std::unique_ptr<uint8_t, decltype(&std::free)>(
                static_cast<uint8_t*>(std::malloc(bytes)), &std::free);
	if (!block) {
		return Error::TooLarge;
	}
	// volatile: an optimizer may drop a block nothing reads, and with it
	// the allocation this hook exists to make.
	volatile uint8_t* const pages = block.get();
	for (size_t i = 0; i < bytes; i += 4096) {
		pages[i] = 1;
	}
	return Error::None;
}

// libarchive converts UTF-16 names (Joliet, 7z, RAR4) to the locale's charset,
// NULL for non-ASCII in "C". UTF-8 is "C.UTF-8" on glibc and FreeBSD,
// "UTF-8" on macOS, ".UTF-8" on the Windows UCRT.
bool use_utf8_ctype()
{
	for (const char* name :
	     {"C.UTF-8", "C.utf8", "UTF-8", ".UTF-8", "en_US.UTF-8"}) {
		if (std::setlocale(LC_CTYPE, name)) {
			return true;
		}
	}
	return false;
}

} // namespace

int main(int argc, char** argv)
{
	const auto options = parse(argc, argv);
	if (!use_utf8_ctype()) {
		std::fputs(
		        "archive helper: no UTF-8 locale, names outside ASCII "
		        "are refused\n",
		        stderr);
	}
	if (options.bad) {
		std::fputs(
		        "usage: dosbox-automation-archive-helper "
		        "[--memory-limit-mb N] [--test-hooks] [--version]\n",
		        stderr);
		return ExitProtocol;
	}
	if (options.version) {
		std::printf("dosbox-automation-archive-helper %s (%s)\n",
		            HelperVersion,
		            archive_version_details());
		return ExitClean;
	}
	if (options.memory_limit_mb > 0 &&
	    !apply_memory_limit(options.memory_limit_mb)) {
		std::fputs("archive helper: cannot apply the memory cap\n", stderr);
		return ExitNoMemoryCap;
	}
	die_with_parent();
	if (!claim_stdout()) {
		std::fputs("archive helper: cannot set up the protocol stream\n",
		           stderr);
		return ExitProtocol;
	}

#if !defined(WIN32)
	struct stat stdin_stat = {};
	stdin_is_socket = fstat(0, &stdin_stat) == 0 && S_ISSOCK(stdin_stat.st_mode);
#endif

	std::unique_ptr<ArchiveReader> reader = {};
	auto hooks                            = TestHooks{};
	std::vector<uint8_t> header(HeaderSize);
	std::vector<uint8_t> payload = {};

	for (int served = 0; served < RequestsPerInstance; ++served) {
		if (!read_exact(header.data(), header.size())) {
			return ExitClean;
		}
		Type type       = Type::Quit;
		uint32_t length = 0;
		if (!DecodeHeader(header, MaxRequestPayload, type, length)) {
			send_status(Error::HelperFailed, "malformed request frame");
			return ExitProtocol;
		}
		payload.resize(length);
		if (length > 0 && !read_exact(payload.data(), length)) {
			return ExitClean;
		}

		switch (type) {
		case Type::Hello: {
			auto reply               = HelloReply{};
			reply.version            = Version;
			reply.libarchive_version = static_cast<uint32_t>(
			        archive_version_number());
			reply.libarchive_string = archive_version_details();
			if (!send(Type::HelloReply, EncodeHelloReply(reply))) {
				return ExitClean;
			}
			break;
		}
		case Type::Open: {
			auto request = OpenRequest{};
			if (!DecodeOpenRequest(payload, request)) {
				send_status(Error::HelperFailed,
				            "malformed open request");
				return ExitProtocol;
			}
			reader.reset();
			int archive_fd = -1;
			if (request.descriptor_follows) {
#if defined(WIN32)
				if (!send_status(Error::HelperFailed,
				                 "descriptor passing is not available on this host")) {
					return ExitClean;
				}
				break;
#else
				if (pending_descriptor < 0) {
					if (!send_status(Error::HelperFailed,
					                 "the open request announced a descriptor "
					                 "that did not arrive")) {
						return ExitClean;
					}
					break;
				}
				archive_fd         = pending_descriptor;
				pending_descriptor = -1;
#endif
			} else if (request.handle != 0) {
#if defined(WIN32)
				const auto handle = reinterpret_cast<HANDLE>(
				        static_cast<uintptr_t>(request.handle));
				if (GetFileType(handle) != FILE_TYPE_DISK) {
					if (!send_status(Error::HelperFailed,
					                 "the archive handle did not arrive")) {
						return ExitClean;
					}
					break;
				}
				archive_fd = _open_osfhandle(
				        reinterpret_cast<intptr_t>(handle),
				        _O_RDONLY | _O_BINARY | _O_NOINHERIT);
#else
				if (!send_status(Error::HelperFailed,
				                 "a Windows handle on a POSIX host")) {
					return ExitClean;
				}
				break;
#endif
			}
			auto window = std::optional<ArchiveMount::ByteWindow>{};
			if (request.window_length > 0) {
				window = ArchiveMount::ByteWindow{request.window_offset,
				                                  request.window_length};
			}
			if (window && archive_fd < 0) {
				if (!send_status(Error::OpenFailed,
				                 "an archive window needs a descriptor")) {
					return ExitClean;
				}
				break;
			}
			auto opened = archive_fd >= 0
			                    ? ArchiveReader::Open(archive_fd,
			                                          path_from_utf8(
			                                                  request.path),
			                                          request.limits,
			                                          window)
			                    : ArchiveReader::Open(
			                              path_from_utf8(request.path),
			                              request.limits);
			// The reader holds its own duplicate.
			ArchiveMount::CloseDescriptor(archive_fd);
			if (opened.error != Error::None) {
				if (!send_status(opened.error, opened.detail)) {
					return ExitClean;
				}
				break;
			}
			auto reply           = IndexReply{};
			reply.format         = opened.reader->GetFormat();
			reply.access         = opened.reader->GetAccess();
			reply.total_bytes    = opened.reader->TotalBytes();
			reply.archive_length = opened.reader->ArchiveLength();
			reply.identity       = opened.reader->IdentityHash();
			reply.stat           = opened.reader->IndexedStat();
			reply.entries        = opened.reader->Entries();
			const auto bytes     = EncodeIndexReply(reply);
			if (bytes.size() > MaxReplyPayload) {
				if (!send_status(Error::TooManyEntries,
				                 "the index exceeds the helper's reply cap")) {
					return ExitClean;
				}
				break;
			}
			reader = std::move(opened.reader);
			if (!send(Type::IndexReply, bytes)) {
				return ExitClean;
			}
			break;
		}
		case Type::Materialize: {
			auto request = MaterializeRequest{};
			if (!DecodeMaterializeRequest(payload, request)) {
				send_status(Error::HelperFailed,
				            "malformed materialize request");
				return ExitProtocol;
			}
			if (!reader) {
				if (!send_status(Error::OpenFailed,
				                 "no archive is open")) {
					return ExitClean;
				}
				break;
			}
			run_test_hooks(hooks, request.ordinal);
			auto detail      = std::string();
			const auto error = reader->MaterializeToFile(
			        request.ordinal, path_from_utf8(request.target), &detail);
			if (!send_status(error, detail)) {
				return ExitClean;
			}
			break;
		}
		case Type::MaterializeAll: {
			auto request = MaterializeAllRequest{};
			if (!DecodeMaterializeAllRequest(payload, request)) {
				send_status(Error::HelperFailed,
				            "malformed materialize-all request");
				return ExitProtocol;
			}
			if (!reader) {
				if (!send_status(Error::OpenFailed,
				                 "no archive is open")) {
					return ExitClean;
				}
				break;
			}
			run_test_hooks(hooks, -1);
			auto detail      = std::string();
			const auto error = reader->MaterializeAll(
			        path_from_utf8(request.directory), &detail);
			if (!send_status(error, detail)) {
				return ExitClean;
			}
			break;
		}
		case Type::Quit: return ExitClean;
		case Type::TestCrashOn:
		case Type::TestHangOn:
		case Type::TestAllocate: {
			if (!options.test_hooks) {
				send_status(Error::HelperFailed,
				            "test hook without --test-hooks");
				return ExitProtocol;
			}
			auto r     = Reader(payload);
			uint32_t v = 0;
			if (!r.U32(v) || !r.AtEnd()) {
				send_status(Error::HelperFailed,
				            "malformed test hook");
				return ExitProtocol;
			}
			auto error = Error::None;
			if (type == Type::TestCrashOn) {
				hooks.crash_on = static_cast<int>(v);
			} else if (type == Type::TestHangOn) {
				hooks.hang_on = static_cast<int>(v);
			} else {
				error = allocate_and_touch(v);
			}
			if (!send_status(error, "")) {
				return ExitClean;
			}
			break;
		}
		case Type::HelloReply:
		case Type::IndexReply:
		case Type::StatusReply:
			send_status(Error::HelperFailed,
			            "a reply type sent as a request");
			return ExitProtocol;
		}
	}
	return ExitClean;
}
