// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//
#ifndef DOSBOX_CHILD_PROCESS_H
#define DOSBOX_CHILD_PROCESS_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

struct SDL_Process;
struct SDL_IOStream;

// A child with piped stdin and stdout over SDL3's process API, for the
// archive helper (design D15). A read that passes its deadline kills the
// child: that is the engine's answer to a hang.
class ChildProcess {
public:
	// Socket: the child's stdin is a Unix socket pair end, so a
	// descriptor rides with a frame (SDL closes every other descriptor
	// in the child). On Windows both kinds are the same pipe.
	enum class StdinKind { Pipe, Socket };
	static std::unique_ptr<ChildProcess> Start(
	        const std::vector<std::string>& argv, std::string* detail,
	        StdinKind stdin_kind = StdinKind::Pipe);
	~ChildProcess(); // kills the child if it still runs

	ChildProcess(const ChildProcess&)            = delete;
	ChildProcess& operator=(const ChildProcess&) = delete;

	bool Write(std::span<const uint8_t> bytes);
	// The frame's first bytes carry fd to the child; false on a pipe
	// stdin or on Windows. The caller keeps fd.
	bool WriteWithDescriptor(int fd, std::span<const uint8_t> bytes);
	// Exactly size bytes, or false on EOF or at the deadline; timed_out
	// says which, and after a timeout the child is dead.
	bool ReadExact(uint8_t* out, size_t size,
	               std::chrono::steady_clock::time_point deadline,
	               bool* timed_out);
	bool IsAlive();
	// Waits up to budget for the child to exit on its own.
	bool WaitFor(std::chrono::milliseconds budget, int* exit_code);
	void Kill();

private:
	ChildProcess() = default;

	enum class ReadState { Data, NotReady, Closed };
	ReadState ReadSome(uint8_t* out, size_t size, size_t& got);
	void WaitReadable(std::chrono::milliseconds budget);

	SDL_Process* process = nullptr;
	SDL_IOStream* input  = nullptr; // the child's stdin
	// The child's stdout, read past SDL: SDL 3.4.8's Windows stream drops
	// read-ahead bytes when a later ReadFile finds the pipe empty.
#if defined(WIN32)
	void* output_handle = nullptr;
#else
	int output_fd = -1;
	int input_fd  = -1; // POSIX socket stdin: our end
#endif
};

#endif // DOSBOX_CHILD_PROCESS_H
