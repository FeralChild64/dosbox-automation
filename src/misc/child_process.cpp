// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//
#include "misc/child_process.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cerrno>

#if defined(WIN32)
#include <windows.h>
#else
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>
#endif

#include "utils/checks.h"

CHECK_NARROWING();

using Clock = std::chrono::steady_clock;

#if !defined(WIN32)
// A write to a helper that died answers EPIPE instead of raising SIGPIPE;
// where the flag is missing, SO_NOSIGPIPE on the socket does it.
#if defined(MSG_NOSIGNAL)
constexpr int SendFlags = MSG_NOSIGNAL;
#else
constexpr int SendFlags = 0;
#endif

static bool would_block(const int error)
{
#if EAGAIN == EWOULDBLOCK
	return error == EAGAIN;
#else
	return error == EAGAIN || error == EWOULDBLOCK;
#endif
}
#endif

std::unique_ptr<ChildProcess> ChildProcess::Start(const std::vector<std::string>& argv,
                                                  std::string* detail,
                                                  const StdinKind stdin_kind)
{
	auto fail = [detail](const std::string& why) {
		if (detail) {
			*detail = why;
		}
		return std::unique_ptr<ChildProcess>();
	};
	if (argv.empty()) {
		return fail("no program to start");
	}
	std::vector<const char*> args = {};
	args.reserve(argv.size() + 1);
	for (const auto& arg : argv) {
		args.push_back(arg.c_str());
	}
	args.push_back(nullptr);

	const auto props = SDL_CreateProperties();
	SDL_SetPointerProperty(props,
	                       SDL_PROP_PROCESS_CREATE_ARGS_POINTER,
	                       const_cast<char**>(args.data()));
#if !defined(WIN32)
	int pair[2]             = {-1, -1};
	SDL_IOStream* child_end = nullptr;
	if (stdin_kind == StdinKind::Socket) {
#if defined(SOCK_CLOEXEC)
		if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) != 0) {
#else
		if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0) {
#endif
			SDL_DestroyProperties(props);
			return fail(std::string("socketpair: ") + strerror(errno));
		}
		for (const auto fd : pair) {
			fcntl(fd, F_SETFD, fcntl(fd, F_GETFD) | FD_CLOEXEC);
		}
#if defined(SO_NOSIGPIPE)
		const int one = 1;
		setsockopt(pair[0], SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
		// An interface with no functions: SDL's spawn reads only the
		// descriptor property and dup2s it onto the child's stdin.
		SDL_IOStreamInterface iface = {};
		SDL_INIT_INTERFACE(&iface);
		child_end = SDL_OpenIO(&iface, nullptr);
		if (!child_end) {
			close(pair[0]);
			close(pair[1]);
			SDL_DestroyProperties(props);
			return fail(SDL_GetError());
		}
		SDL_SetNumberProperty(SDL_GetIOProperties(child_end),
		                      SDL_PROP_IOSTREAM_FILE_DESCRIPTOR_NUMBER,
		                      pair[1]);
		SDL_SetNumberProperty(props,
		                      SDL_PROP_PROCESS_CREATE_STDIN_NUMBER,
		                      SDL_PROCESS_STDIO_REDIRECT);
		SDL_SetPointerProperty(props,
		                       SDL_PROP_PROCESS_CREATE_STDIN_POINTER,
		                       child_end);
	} else
#endif
	{
		SDL_SetNumberProperty(props,
		                      SDL_PROP_PROCESS_CREATE_STDIN_NUMBER,
		                      SDL_PROCESS_STDIO_APP);
	}
	SDL_SetNumberProperty(props,
	                      SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER,
	                      SDL_PROCESS_STDIO_APP);
	SDL_SetNumberProperty(props,
	                      SDL_PROP_PROCESS_CREATE_STDERR_NUMBER,
	                      SDL_PROCESS_STDIO_INHERITED);
	auto* process = SDL_CreateProcessWithProperties(props);
	SDL_DestroyProperties(props);
#if !defined(WIN32)
	if (child_end) {
		SDL_CloseIO(child_end); // the child holds its own stdin now
		close(pair[1]);
		if (!process) {
			close(pair[0]);
		}
	}
#else
	(void)stdin_kind;
#endif
	if (!process) {
		return fail(SDL_GetError());
	}

	auto child     = std::unique_ptr<ChildProcess>(new ChildProcess());
	child->process = process;
#if !defined(WIN32)
	if (stdin_kind == StdinKind::Socket) {
		child->input_fd = pair[0];
		fcntl(child->input_fd,
		      F_SETFL,
		      fcntl(child->input_fd, F_GETFL) | O_NONBLOCK);
	} else {
		child->input = SDL_GetProcessInput(process);
	}
	auto* output = SDL_GetProcessOutput(process);
	if ((!child->input && child->input_fd < 0) || !output) {
		return fail("the child started without pipes");
	}
#else
	child->input = SDL_GetProcessInput(process);
	auto* output = SDL_GetProcessOutput(process);
	if (!child->input || !output) {
		return fail("the child started without pipes");
	}
#endif
	const auto io_props = SDL_GetIOProperties(output);
#if defined(WIN32)
	child->output_handle = SDL_GetPointerProperty(
	        io_props, SDL_PROP_IOSTREAM_WINDOWS_HANDLE_POINTER, nullptr);
	if (!child->output_handle) {
		return fail("the child's stdout has no handle");
	}
#else
	child->output_fd = static_cast<int>(SDL_GetNumberProperty(
	        io_props, SDL_PROP_IOSTREAM_FILE_DESCRIPTOR_NUMBER, -1));
	if (child->output_fd < 0) {
		return fail("the child's stdout has no descriptor");
	}
#endif
	return child;
}

ChildProcess::~ChildProcess()
{
	if (!process) {
		return;
	}
	if (IsAlive()) {
		Kill();
	}
#if !defined(WIN32)
	if (input_fd >= 0) {
		close(input_fd);
	}
#endif
	// Also closes both streams, which belong to the process.
	SDL_DestroyProcess(process);
}

bool ChildProcess::Write(const std::span<const uint8_t> bytes)
{
	// A full pipe writes short, not-ready or (Windows) partial; only an
	// error or the deadline means a dead child. No flush: on Windows it
	// waits for the reader.
	const auto deadline = Clock::now() + std::chrono::seconds(5);
	size_t done         = 0;
#if !defined(WIN32)
	if (input_fd >= 0) {
		while (done < bytes.size()) {
			const auto n = send(input_fd,
			                    bytes.data() + done,
			                    bytes.size() - done,
			                    SendFlags);
			if (n > 0) {
				done += static_cast<size_t>(n);
				continue;
			}
			if (n < 0 && errno == EINTR) {
				continue;
			}
			if (n < 0 && would_block(errno) && Clock::now() < deadline) {
				SDL_Delay(1);
				continue;
			}
			return false;
		}
		return true;
	}
#endif
	while (done < bytes.size()) {
		const auto n = SDL_WriteIO(input,
		                           bytes.data() + done,
		                           bytes.size() - done);
		done += n;
		if (done == bytes.size()) {
			break;
		}
		const auto status = SDL_GetIOStatus(input);
		if (status == SDL_IO_STATUS_ERROR ||
		    status == SDL_IO_STATUS_EOF || Clock::now() >= deadline) {
			return false;
		}
		if (n == 0) {
			SDL_Delay(1);
		}
	}
	return true;
}

bool ChildProcess::WriteWithDescriptor(const int fd,
                                       const std::span<const uint8_t> bytes)
{
#if defined(WIN32)
	(void)fd;
	(void)bytes;
	return false;
#else
	if (input_fd < 0 || fd < 0 || bytes.empty()) {
		return false;
	}
	auto iov    = iovec{const_cast<uint8_t*>(bytes.data()), bytes.size()};
	auto msg    = msghdr{};
	msg.msg_iov = &iov;
	msg.msg_iovlen                                         = 1;
	alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))] = {};
	msg.msg_control                                        = control;
	msg.msg_controllen = sizeof(control);
	auto* cmsg         = CMSG_FIRSTHDR(&msg);
	cmsg->cmsg_level   = SOL_SOCKET;
	cmsg->cmsg_type    = SCM_RIGHTS;
	cmsg->cmsg_len     = CMSG_LEN(sizeof(int));
	std::memcpy(CMSG_DATA(cmsg), &fd, sizeof(int));
	const auto deadline = Clock::now() + std::chrono::seconds(5);
	ssize_t sent        = -1;
	while (sent < 0) {
		sent = sendmsg(input_fd, &msg, SendFlags);
		if (sent >= 0) {
			break;
		}
		if (errno == EINTR) {
			continue;
		}
		if (would_block(errno) && Clock::now() < deadline) {
			SDL_Delay(1);
			continue;
		}
		return false;
	}
	// The descriptor rides with the first bytes; a short send leaves
	// the rest as ordinary data.
	return Write(bytes.subspan(static_cast<size_t>(sent)));
#endif
}

ChildProcess::ReadState ChildProcess::ReadSome(uint8_t* out, const size_t size,
                                               size_t& got)
{
	got = 0;
#if defined(WIN32)
	DWORD n        = 0;
	const auto ask = static_cast<DWORD>(std::min<size_t>(size, 1u << 30));
	if (!ReadFile(static_cast<HANDLE>(output_handle), out, ask, &n, nullptr)) {
		return GetLastError() == ERROR_NO_DATA ? ReadState::NotReady
		                                       : ReadState::Closed;
	}
	if (n == 0) {
		// A PIPE_NOWAIT pipe with no data may also succeed with nothing.
		return ReadState::NotReady;
	}
	got = n;
	return ReadState::Data;
#else
	for (;;) {
		const auto n = read(output_fd, out, size);
		if (n > 0) {
			got = static_cast<size_t>(n);
			return ReadState::Data;
		}
		if (n == 0) {
			return ReadState::Closed;
		}
		if (errno == EINTR) {
			continue;
		}
		return would_block(errno) ? ReadState::NotReady : ReadState::Closed;
	}
#endif
}

void ChildProcess::WaitReadable(const std::chrono::milliseconds budget)
{
#if defined(WIN32)
	// Anonymous pipes cannot be waited on; poll every millisecond.
	(void)budget;
	SDL_Delay(1);
#else
	auto pfd           = pollfd{};
	pfd.fd             = output_fd;
	pfd.events         = POLLIN;
	const auto wait_ms = static_cast<int>(
	        std::clamp<int64_t>(budget.count(), 0, 1000));
	poll(&pfd, 1, wait_ms);
#endif
}

bool ChildProcess::ReadExact(uint8_t* out, const size_t size,
                             const Clock::time_point deadline, bool* timed_out)
{
	*timed_out  = false;
	size_t done = 0;
	while (done < size) {
		size_t got       = 0;
		const auto state = ReadSome(out + done, size - done, got);
		if (state == ReadState::Data) {
			done += got;
			continue;
		}
		if (state == ReadState::Closed) {
			return false;
		}
		const auto now = Clock::now();
		if (now >= deadline) {
			Kill();
			*timed_out = true;
			return false;
		}
		WaitReadable(std::chrono::duration_cast<std::chrono::milliseconds>(
		        deadline - now));
	}
	return true;
}

bool ChildProcess::IsAlive()
{
	int exit_code = 0;
	return !SDL_WaitProcess(process, false, &exit_code);
}

bool ChildProcess::WaitFor(const std::chrono::milliseconds budget, int* exit_code)
{
	const auto deadline = Clock::now() + budget;
	while (Clock::now() < deadline) {
		if (SDL_WaitProcess(process, false, exit_code)) {
			return true;
		}
		SDL_Delay(10);
	}
	return SDL_WaitProcess(process, false, exit_code);
}

void ChildProcess::Kill()
{
	SDL_KillProcess(process, true);
	SDL_WaitProcess(process, true, nullptr);
}
