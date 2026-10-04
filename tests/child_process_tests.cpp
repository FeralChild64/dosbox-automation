// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//
#include "misc/child_process.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <thread>

#if !defined(WIN32)
#include <csignal>
#include <sys/types.h>
#include <unistd.h>
#endif

#include "archive_fixtures.h"
#include "dos/archive/descriptor.h"
#include "dos/archive/helper_protocol.h"
#include "test_temp_dir.h"

using namespace std::chrono;
using namespace ArchiveHelper;

namespace {

const std::string HelperPath = DOSBOX_ARCHIVE_HELPER_PATH;

// A socket stdin by default: the transport the engine uses. The pipe
// stays covered by HelloRoundTripOnAPipe.
std::unique_ptr<ChildProcess> StartHelper(
        const ChildProcess::StdinKind kind = ChildProcess::StdinKind::Socket)
{
	auto detail = std::string();
	auto child  = ChildProcess::Start({HelperPath}, &detail, kind);
	EXPECT_TRUE(child) << detail;
	return child;
}

bool ReadReply(ChildProcess& child, Type& type, std::vector<uint8_t>& payload)
{
	const auto deadline = steady_clock::now() + 5000ms;
	std::vector<uint8_t> header(HeaderSize);
	bool timed_out = false;
	if (!child.ReadExact(header.data(), header.size(), deadline, &timed_out)) {
		return false;
	}
	uint32_t length = 0;
	if (!DecodeHeader(header, MaxReplyPayload, type, length)) {
		return false;
	}
	payload.resize(length);
	return length == 0 ||
	       child.ReadExact(payload.data(), length, deadline, &timed_out);
}

#if defined(__linux__) || defined(__FreeBSD__)
// kill(pid, 0) also succeeds on a zombie nobody has reaped yet.
bool ProcessRuns(const pid_t pid)
{
#if defined(__linux__)
	auto* f = std::fopen(("/proc/" + std::to_string(pid) + "/stat").c_str(), "r");
	if (!f) {
		return false;
	}
	char state        = 0;
	const auto fields = std::fscanf(f, "%*d %*s %c", &state);
	std::fclose(f);
	return fields == 1 && state != 'Z';
#else
	return kill(pid, 0) == 0;
#endif
}
#endif

} // namespace

TEST(ChildProcess, HelloRoundTrip)
{
	auto child = StartHelper();
	ASSERT_TRUE(child);
	ASSERT_TRUE(child->Write(EncodeFrame(Type::Hello, {})));
	Type type                    = Type::Quit;
	std::vector<uint8_t> payload = {};
	ASSERT_TRUE(ReadReply(*child, type, payload));
	EXPECT_EQ(type, Type::HelloReply);
	auto hello = HelloReply{};
	ASSERT_TRUE(DecodeHelloReply(payload, hello));
	EXPECT_EQ(hello.version, Version);
	EXPECT_GE(hello.libarchive_version, 3004000u);
	ASSERT_TRUE(child->Write(EncodeFrame(Type::Quit, {})));
	int exit_code = -1;
	EXPECT_TRUE(child->WaitFor(2000ms, &exit_code));
	EXPECT_EQ(exit_code, 0);
}

TEST(ChildProcess, HelloRoundTripOnAPipe)
{
	auto child = StartHelper(ChildProcess::StdinKind::Pipe);
	ASSERT_TRUE(child);
	ASSERT_TRUE(child->Write(EncodeFrame(Type::Hello, {})));
	Type type                    = Type::Quit;
	std::vector<uint8_t> payload = {};
	ASSERT_TRUE(ReadReply(*child, type, payload));
	EXPECT_EQ(type, Type::HelloReply);
	auto hello = HelloReply{};
	ASSERT_TRUE(DecodeHelloReply(payload, hello));
	EXPECT_EQ(hello.version, Version);
	EXPECT_GE(hello.libarchive_version, 3004000u);
	ASSERT_TRUE(child->Write(EncodeFrame(Type::Quit, {})));
	int exit_code = -1;
	EXPECT_TRUE(child->WaitFor(2000ms, &exit_code));
	EXPECT_EQ(exit_code, 0);
}

// Replies that arrive back to back and in pieces come out whole and in
// order, which a reader with its own read-ahead can get wrong.
TEST(ChildProcess, PipelinedRepliesArriveWholeAndInOrder)
{
	auto child = StartHelper();
	ASSERT_TRUE(child);
	constexpr int Count = 40;
	auto frames         = std::vector<uint8_t>();
	for (int i = 0; i < Count; ++i) {
		const auto one = EncodeFrame(Type::Hello, {});
		frames.insert(frames.end(), one.begin(), one.end());
	}
	ASSERT_TRUE(child->Write(frames));
	for (int i = 0; i < Count; ++i) {
		Type type                    = Type::Quit;
		std::vector<uint8_t> payload = {};
		ASSERT_TRUE(ReadReply(*child, type, payload)) << "reply " << i;
		EXPECT_EQ(type, Type::HelloReply) << "reply " << i;
		auto hello = HelloReply{};
		EXPECT_TRUE(DecodeHelloReply(payload, hello)) << "reply " << i;
	}
}

TEST(ChildProcess, AMalformedFrameEndsTheHelperWithTwo)
{
	auto child = StartHelper();
	ASSERT_TRUE(child);
	auto junk = EncodeFrame(Type::Hello, {});
	junk[0]   = 'X';
	ASSERT_TRUE(child->Write(junk));
	int exit_code = -1;
	EXPECT_TRUE(child->WaitFor(2000ms, &exit_code));
	EXPECT_EQ(exit_code, 2);
}

TEST(ChildProcess, ReadPastTheDeadlineKillsTheChild)
{
	auto child = StartHelper();
	ASSERT_TRUE(child);
	uint8_t byte   = 0;
	bool timed_out = false;
	const auto t0  = steady_clock::now();
	EXPECT_FALSE(child->ReadExact(&byte, 1, t0 + 300ms, &timed_out));
	EXPECT_TRUE(timed_out);
	EXPECT_GE(steady_clock::now() - t0, 300ms);
	EXPECT_LT(steady_clock::now() - t0, 3000ms);
	EXPECT_FALSE(child->IsAlive());
}

TEST(ChildProcess, EofIsNotATimeout)
{
	auto child = StartHelper();
	ASSERT_TRUE(child);
	ASSERT_TRUE(child->Write(EncodeFrame(Type::Quit, {})));
	uint8_t byte   = 0;
	bool timed_out = true;
	EXPECT_FALSE(child->ReadExact(&byte, 1, steady_clock::now() + 5000ms, &timed_out));
	EXPECT_FALSE(timed_out);
}

TEST(ChildProcess, AMissingBinaryFailsToStart)
{
	auto detail = std::string();
	auto child = ChildProcess::Start({HelperPath + ".does-not-exist"}, &detail);
	EXPECT_FALSE(child);
	EXPECT_FALSE(detail.empty());
}

TEST(ChildProcess, AnEmptyArgvIsRefused)
{
	auto detail = std::string();
	EXPECT_FALSE(ChildProcess::Start({}, &detail));
	EXPECT_EQ(detail, "no program to start");
}

#if !defined(WIN32)
// More than a pipe holds, to a child that reads late: the write fills the
// pipe, gets partial and not-ready answers, and must still complete.
TEST(ChildProcess, AWriteLargerThanThePipeCompletes)
{
	auto detail = std::string();
	auto child = ChildProcess::Start({"/bin/sh", "-c", "sleep 0.3; cat > /dev/null"},
	                                 &detail);
	ASSERT_TRUE(child) << detail;
	const auto big = std::vector<uint8_t>(1024 * 1024, 0x5a);
	EXPECT_TRUE(child->Write(big));
	child->Kill();
	EXPECT_FALSE(child->IsAlive());
}
#endif

#if !defined(WIN32)
// The engine must outlive a helper that died, whatever SIGPIPE's
// disposition is at that moment; SDL only ignores it as a side effect.
TEST(ChildProcess, AWriteToADeadHelperFailsWithoutSigpipe)
{
	auto child = StartHelper();
	ASSERT_TRUE(child);
	ASSERT_TRUE(child->Write(EncodeFrame(Type::Quit, {})));
	uint8_t byte   = 0;
	bool timed_out = true;
	ASSERT_FALSE(child->ReadExact(&byte, 1, steady_clock::now() + 5000ms, &timed_out));
	ASSERT_FALSE(timed_out);
	// Exit releases descriptors in no set order: EOF on the protocol stream
	// can come before the stdin socket closes, so wait for the reap.
	int exit_code = -1;
	ASSERT_TRUE(child->WaitFor(5000ms, &exit_code));

	struct sigaction default_action = {};
	default_action.sa_handler       = SIG_DFL;
	struct sigaction previous       = {};
	ASSERT_EQ(sigaction(SIGPIPE, &default_action, &previous), 0);
	const auto frame = EncodeFrame(Type::Quit, {});
	EXPECT_FALSE(child->Write(frame));
	EXPECT_FALSE(child->WriteWithDescriptor(STDERR_FILENO, frame));
	sigaction(SIGPIPE, &previous, nullptr);
}
#endif

#if !defined(WIN32)
TEST(ChildProcess, ADescriptorTravelsWithTheOpenFrame)
{
	const auto tmp = TestTempDir::MakeUnique("child_fd_");
	ASSERT_FALSE(tmp.empty());
	const auto path = tmp / "d.zip";
	auto error      = std::string();
	ASSERT_TRUE(ArchiveFixtures::WriteArchive(path,
	                                          ArchiveFixtures::Kind::ZipStored,
	                                          {
	                                                  {"a.txt",  "a"},
	                                                  {"b.txt", "bb"}
        },
	                                          nullptr,
	                                          &error))
	        << error;
	const auto fd = ArchiveMount::OpenReadOnlyShared(path);
	ASSERT_GE(fd, 0);
	auto child = StartHelper();
	ASSERT_TRUE(child);
	auto request = OpenRequest{};
	request.path = "not-the-file.zip"; // a descriptor came, the path is a
	                                   // label
	request.descriptor_follows = true;
	ASSERT_TRUE(child->WriteWithDescriptor(
	        fd, EncodeFrame(Type::Open, EncodeOpenRequest(request))));
	ArchiveMount::CloseDescriptor(fd);
	Type type                    = Type::Quit;
	std::vector<uint8_t> payload = {};
	ASSERT_TRUE(ReadReply(*child, type, payload));
	ASSERT_EQ(type, Type::IndexReply);
	auto index = IndexReply{};
	ASSERT_TRUE(DecodeIndexReply(payload, index));
	EXPECT_EQ(index.entries.size(), 2u);
	std::error_code ec = {};
	std::filesystem::remove_all(tmp, ec);
}

TEST(ChildProcess, AnAnnouncedDescriptorThatNeverArrivesIsRefused)
{
	auto child = StartHelper();
	ASSERT_TRUE(child);
	auto request               = OpenRequest{};
	request.path               = "x.zip";
	request.descriptor_follows = true;
	ASSERT_TRUE(child->Write(EncodeFrame(Type::Open, EncodeOpenRequest(request))));
	Type type                    = Type::Quit;
	std::vector<uint8_t> payload = {};
	ASSERT_TRUE(ReadReply(*child, type, payload));
	ASSERT_EQ(type, Type::StatusReply);
	auto status = StatusReply{};
	ASSERT_TRUE(DecodeStatusReply(payload, status));
	EXPECT_EQ(status.error, ArchiveMount::Error::HelperFailed);
	EXPECT_NE(status.detail.find("did not arrive"), std::string::npos)
	        << status.detail;
}
#endif

#if defined(__linux__) || defined(__FreeBSD__)
// The helper's stdin is our pipe, saved on fd 3 because sh gives a
// background job /dev/null first; only the parent-death signal ends it.
TEST(ChildProcess, TheHelperDiesWithItsParent)
{
	auto detail = std::string();
	auto shell  = ChildProcess::Start({"/bin/sh",
	                                   "-c",
	                                   "exec 3<&0; \"$0\" <&3 3<&- & echo $!; wait",
	                                   HelperPath},
                                         &detail);
	ASSERT_TRUE(shell) << detail;
	auto pid_text       = std::string();
	bool timed_out      = false;
	const auto deadline = steady_clock::now() + 5000ms;
	for (uint8_t c = 0; c != '\n';) {
		ASSERT_TRUE(shell->ReadExact(&c, 1, deadline, &timed_out));
		pid_text.push_back(static_cast<char>(c));
	}
	const auto helper_pid = static_cast<pid_t>(std::stol(pid_text));
	ASSERT_GT(helper_pid, 0);
	// A reply means the helper is past its prctl; killed before that, it
	// would never get the signal (the engine's pipe closing covers that).
	ASSERT_TRUE(shell->Write(EncodeFrame(Type::Hello, {})));
	Type type                    = Type::Quit;
	std::vector<uint8_t> payload = {};
	ASSERT_TRUE(ReadReply(*shell, type, payload));
	shell->Kill();
	auto gone = false;
	for (int i = 0; i < 200 && !gone; ++i) {
		gone = !ProcessRuns(helper_pid);
		std::this_thread::sleep_for(10ms);
	}
	EXPECT_TRUE(gone);
	if (!gone) {
		kill(helper_pid, SIGKILL);
	}
}
#endif
