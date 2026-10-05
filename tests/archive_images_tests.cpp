// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//
#include "dos/programs/archive_images.h"

#include "archive_fixtures.h"
#include "config/setup.h"
#include "dos/archive/archive_helper_client.h"
#include "dos/cdrom.h"
#include "dos/programs/mount.h"
#include "dos/programs/mount_policy.h"
#include "dosbox_test_fixture.h"
#include "test_temp_dir.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using ArchiveFixtures::FileEntry;
using ArchiveFixtures::Filter;
using ArchiveFixtures::IszChunk;
using ArchiveFixtures::Kind;
using ArchiveFixtures::MinimalIso;
using ArchiveFixtures::MtimeOf;
using ArchiveFixtures::WriteArchive;
using ArchiveFixtures::WriteCompressed;
using ArchiveFixtures::WriteIsz;

namespace {

namespace std_fs = std::filesystem;

// A 320K DOS 1.x floppy: fatDrive synthesizes the BPB from the media byte.
std::string FloppyBytes()
{
	std::string img(327680, '\0');
	img[512] = '\xFF';
	return img;
}

std::string Slurp(const std_fs::path& path)
{
	std::ifstream in(path, std::ios::binary);
	std::stringstream s;
	s << in.rdbuf();
	return s.str();
}

class ArchiveImagesTest : public DOSBoxTestFixture {
protected:
	void SetUp() override
	{
		DOSBoxTestFixture::SetUp();
		MountPolicy::InitPolicyConfig({});
		tmp = TestTempDir::MakeUnique("archive_images_");
		ASSERT_FALSE(tmp.empty());
		ArchiveMount::SetHelperPathOverride(DOSBOX_ARCHIVE_HELPER_PATH);
		Set("archive_cache_path", (tmp / "cache").string());
		Set("archive_sidecar_path", (tmp / "saves").string());
		MOUNT::ResetArchiveCacheForTests();
		auto error = std::string();
		ASSERT_TRUE(WriteArchive(
		        tmp / "game.zip",
		        Kind::ZipDeflate,
		        {
		                {"disks/", "", true},
		                {"disks/disk1.img", FloppyBytes(), false, false, 700000000},
		                {"cd/", "", true},
		                {"cd/game.iso", MinimalIso()},
		                {"cd/track.bin", MinimalIso()},
		                {"cd/game.cue",
		                 "FILE \"track.bin\" BINARY\n  TRACK 01 MODE1/2048\n    INDEX 01 00:00:00\n"},
		                {"cd/evil.cue", "file \"/etc/passwd\" BINARY\n"},
		                {"cd/game.mds", "MEDIA DESCRIPTOR"}
                },
		        nullptr,
		        &error))
		        << error;
	}
	void TearDown() override
	{
		ArchiveImages::ReleaseAllForTests();
		ArchiveMount::SetHelperPathOverride({});
		MOUNT::ResetArchiveCacheForTests();
		std::error_code ec = {};
		std_fs::remove_all(tmp, ec);
		DOSBoxTestFixture::TearDown();
	}
	static void Set(const char* name, const std::string& value)
	{
		auto section = get_section("dos");
		ASSERT_NE(section, nullptr);
		auto property = section->GetProperty(name);
		ASSERT_NE(property, nullptr) << name;
		ASSERT_TRUE(property->SetValue(value)) << name << " = " << value;
	}
	ArchiveImages::Result Resolve(const std::string& member,
	                              const bool read_only = false)
	{
		ArchiveImages::Request request = {};
		request.member.archive         = tmp / "game.zip";
		request.member.member          = member;
		request.read_only              = read_only;
		request.allowed_image_roots = MountPolicy::AllowedImageRoots();
		request.conf_anchor         = MountPolicy::ConfAnchor();
		return ArchiveImages::Resolve(request);
	}
	// Plan 4b: the container itself is the member path.
	ArchiveImages::Result ResolveLone(const std_fs::path& container,
	                                  const bool read_only = false)
	{
		const auto lone = ArchiveMount::LoneImageMember(container);
		EXPECT_TRUE(lone) << container;
		ArchiveImages::Request request = {};
		request.member    = lone.value_or(ArchiveMount::MemberPath{});
		request.read_only = read_only;
		request.allowed_image_roots = MountPolicy::AllowedImageRoots();
		request.conf_anchor         = MountPolicy::ConfAnchor();
		return ArchiveImages::Resolve(request);
	}
	std_fs::path tmp = {};
};

TEST_F(ArchiveImagesTest, ALoneMemberIsTheOnlyFileOfItsContainer)
{
	ASSERT_TRUE(WriteArchive(tmp / "game.imz",
	                         Kind::ZipDeflate,
	                         {
	                                 {"DISK1.IMA", FloppyBytes()}
        }));
	ASSERT_TRUE(WriteCompressed(tmp / "disk2.img.xz", Filter::Xz, FloppyBytes()));
	for (const auto* name : {"game.imz", "disk2.img.xz"}) {
		const auto result = ResolveLone(tmp / name);
		ASSERT_TRUE(result.resolved) << name << ": " << result.detail;
		const auto& r = *result.resolved;
		EXPECT_EQ(r.host_file.parent_path().filename(), ".images") << name;
		EXPECT_EQ(Slurp(r.host_file), FloppyBytes()) << name;
		// A zip member keeps its own date; a raw image takes the
		// container's (plan 4b, D13.3).
		const auto expected = std::string(name).ends_with(".imz")
		                            ? int64_t{631152000}
		                            : MtimeOf(tmp / name);
		EXPECT_EQ(MtimeOf(r.host_file), expected) << name;
	}
}

TEST_F(ArchiveImagesTest, AnIszIsServedFromTheCacheAsAnIso)
{
	const auto iso = MinimalIso() + std::string(64 * 1024, 'z');
	ASSERT_TRUE(WriteIsz(tmp / "game.isz", iso, IszChunk::Zlib));
	const auto result = ResolveLone(tmp / "game.isz");
	ASSERT_TRUE(result.resolved) << result.detail;
	const auto& r = *result.resolved;
	EXPECT_EQ(r.member_name, "game.iso");
	EXPECT_EQ(Slurp(r.host_file), iso);
	EXPECT_NE(r.host_file.string().find((tmp / "cache").string()),
	          std::string::npos);
	EXPECT_FALSE(std_fs::exists(tmp / "saves"));
}

TEST_F(ArchiveImagesTest, AContainerWithTwoFilesHasNoLoneMember)
{
	ASSERT_TRUE(WriteArchive(tmp / "two.imz",
	                         Kind::ZipDeflate,
	                         {
	                                 {"DISK1.IMA", FloppyBytes()},
	                                 {"DISK2.IMA", FloppyBytes()}
        }));
	const auto result = ResolveLone(tmp / "two.imz");
	EXPECT_FALSE(result.resolved);
	EXPECT_EQ(result.verdict.reason, DenyReason::ArchiveMemberNotFound);
	EXPECT_NE(result.detail.find("2 files"), std::string::npos) << result.detail;
}

TEST_F(ArchiveImagesTest, KindIsDecidedByTheExtension)
{
	using ArchiveImages::Kind;
	EXPECT_EQ(ArchiveImages::KindOf("cd/GAME.ISO"), Kind::CdImage);
	EXPECT_EQ(ArchiveImages::KindOf("a.cue"), Kind::CdImage);
	EXPECT_EQ(ArchiveImages::KindOf("a.bin"), Kind::CdImage);
	EXPECT_EQ(ArchiveImages::KindOf("a.mds"), Kind::CdCompanion);
	EXPECT_EQ(ArchiveImages::KindOf("a.CCD"), Kind::CdCompanion);
	EXPECT_EQ(ArchiveImages::KindOf("disk1.img"), Kind::DiskImage);
	EXPECT_EQ(ArchiveImages::KindOf("noext"), Kind::DiskImage);
}

TEST_F(ArchiveImagesTest, AnIsoIsServedFromTheCache)
{
	const auto result = Resolve("cd/game.iso");
	ASSERT_TRUE(result.resolved) << result.detail;
	const auto& r = *result.resolved;
	EXPECT_EQ(Slurp(r.host_file), MinimalIso());
	EXPECT_EQ(r.member_name, "cd/game.iso");
	EXPECT_NE(r.host_file.string().find((tmp / "cache").string()),
	          std::string::npos);
	ASSERT_TRUE(r.hold && r.hold->lease);
	EXPECT_FALSE(std_fs::exists(tmp / "saves"));
}

TEST_F(ArchiveImagesTest, ACueIsRewrittenToTheCachedTrack)
{
	const auto result = Resolve("cd/game.cue");
	ASSERT_TRUE(result.resolved) << result.detail;
	const auto& r = *result.resolved;
	EXPECT_EQ(r.host_file.extension(), ".cue");
	// The sheet sits beside its tracks: the loader takes nothing else
	// (ada-it53).
	EXPECT_EQ(r.host_file.parent_path(), r.hold->lease->FilesDirectory());
	const auto text = Slurp(r.host_file);
	EXPECT_EQ(text.find("track.bin"), std::string::npos) << text;
	const auto first = text.find('"') + 1;
	const auto name  = text.substr(first, text.find('"', first) - first);
	EXPECT_EQ(name.find_first_of("/\\:"), std::string::npos) << name;
	EXPECT_EQ(Slurp(r.host_file.parent_path() / name), MinimalIso());
	CDROM_Interface_Image cd = {};
	EXPECT_TRUE(cd.SetDevice(r.host_file.string().c_str()));
}

TEST_F(ArchiveImagesTest, ACueNamingAHostFileIsRefused)
{
	const auto result = Resolve("cd/evil.cue");
	EXPECT_FALSE(result.resolved);
	EXPECT_EQ(result.verdict.reason, DenyReason::ArchiveCueRejected);
	EXPECT_NE(result.detail.find("/etc/passwd"), std::string::npos)
	        << result.detail;
}

TEST_F(ArchiveImagesTest, AnMdsIsRefusedAsACompanionFormat)
{
	const auto result = Resolve("cd/game.mds");
	EXPECT_FALSE(result.resolved);
	EXPECT_EQ(result.verdict.reason, DenyReason::ArchiveCueRejected);
}

TEST_F(ArchiveImagesTest, AMissingMemberIsRefused)
{
	const auto result = Resolve("disks/disk9.img");
	EXPECT_FALSE(result.resolved);
	EXPECT_EQ(result.verdict.reason, DenyReason::ArchiveMemberNotFound);
}

TEST_F(ArchiveImagesTest, ADotComponentIsRefusedAsARejectedName)
{
	const auto result = Resolve("cd/../disks/disk1.img");
	EXPECT_FALSE(result.resolved);
	EXPECT_EQ(result.verdict.reason, DenyReason::ArchiveEntryRejected);
}

TEST_F(ArchiveImagesTest, ADiskIsCopiedOnceIntoTheSidecarWithItsDate)
{
	auto first = Resolve("disks/disk1.img");
	ASSERT_TRUE(first.resolved) << first.detail;
	const auto copy = first.resolved->host_file;
	EXPECT_EQ(copy.filename(), "DISK1.IMG");
	EXPECT_EQ(copy.parent_path().filename(), "DISKS");
	EXPECT_EQ(copy.parent_path().parent_path().filename(), ".images");
	EXPECT_EQ(Slurp(copy), FloppyBytes());
	// MSVC's file_clock has no to_sys (Windows, 2026-10-04): read the mtime
	// through stat as archive_reader_tests' MtimeOf does.
	EXPECT_EQ(MtimeOf(copy), 700000000);
	ASSERT_TRUE(first.resolved->hold->image_lock);
	EXPECT_FALSE(first.resolved->hold->lease);

	// The user's writes stay; a second resolve never re-extracts.
	{
		std::fstream out(copy, std::ios::in | std::ios::out | std::ios::binary);
		out.seekp(1024);
		out << "SAVED";
	}
	const auto held_path = copy;
	first.resolved->hold.reset();
	const auto second = Resolve("disks/disk1.img");
	ASSERT_TRUE(second.resolved) << second.detail;
	EXPECT_EQ(second.resolved->host_file, held_path);
	EXPECT_EQ(Slurp(held_path).substr(1024, 5), "SAVED");
}

TEST_F(ArchiveImagesTest, ASecondHolderOfTheSameCopyIsRefusedAsBusy)
{
	const auto first = Resolve("disks/disk1.img");
	ASSERT_TRUE(first.resolved) << first.detail;
	const auto second = Resolve("disks/disk1.img");
	EXPECT_FALSE(second.resolved);
	EXPECT_EQ(second.verdict.reason, DenyReason::SidecarBusy);
	EXPECT_NE(second.detail.find("DISK1.IMG"), std::string::npos)
	        << second.detail;
}

TEST_F(ArchiveImagesTest, AReadOnlyDiskComesFromTheCacheAndMakesNoCopy)
{
	const auto result = Resolve("disks/disk1.img", true);
	ASSERT_TRUE(result.resolved) << result.detail;
	EXPECT_TRUE(result.resolved->hold->lease);
	EXPECT_FALSE(std_fs::exists(tmp / "saves"));
}

TEST_F(ArchiveImagesTest, APlantedLinkInTheImagesDirectoryIsRefused)
{
	auto first = Resolve("disks/disk1.img");
	ASSERT_TRUE(first.resolved) << first.detail;
	const auto images = first.resolved->host_file.parent_path().parent_path();
	first.resolved->hold.reset();
	std::error_code ec = {};
	std_fs::remove_all(images / "DISKS", ec);
	std_fs::create_directory_symlink(tmp, images / "DISKS", ec);
	if (ec) {
		GTEST_SKIP() << "no symlinks here: " << ec.message();
	}
	const auto second = Resolve("disks/disk1.img");
	EXPECT_FALSE(second.resolved);
	EXPECT_EQ(second.verdict.reason, DenyReason::SidecarUnavailable);
	EXPECT_FALSE(std_fs::exists(tmp / "DISK1.IMG"));
}

TEST_F(ArchiveImagesTest, APlantedLinkInPlaceOfTheCopyIsRefused)
{
	auto first = Resolve("disks/disk1.img");
	ASSERT_TRUE(first.resolved) << first.detail;
	const auto copy = first.resolved->host_file;
	first.resolved->hold.reset();
	std::error_code ec = {};
	std_fs::remove(copy, ec);
	std::ofstream(tmp / "victim") << "keep";
	std_fs::create_symlink(tmp / "victim", copy, ec);
	if (ec) {
		GTEST_SKIP() << "no symlinks here: " << ec.message();
	}
	const auto second = Resolve("disks/disk1.img");
	EXPECT_FALSE(second.resolved);
	EXPECT_EQ(second.verdict.reason, DenyReason::SidecarUnavailable);
	EXPECT_EQ(Slurp(tmp / "victim"), "keep");
}

TEST_F(ArchiveImagesTest, TheRegistryReleasesByDrive)
{
	auto a = Resolve("disks/disk1.img");
	ASSERT_TRUE(a.resolved) << a.detail;
	ArchiveImages::Keep(0, {a.resolved->hold});
	a.resolved->hold.reset();
	EXPECT_EQ(ArchiveImages::HeldCountForTests(0), 1u);
	EXPECT_FALSE(Resolve("disks/disk1.img").resolved); // still held
	ArchiveImages::Release(0);
	EXPECT_EQ(ArchiveImages::HeldCountForTests(0), 0u);
	EXPECT_TRUE(Resolve("disks/disk1.img").resolved);
}

TEST_F(ArchiveImagesTest, ReleasingAHoldRemovesItsScratchDirectory)
{
	auto result = Resolve("cd/game.cue");
	ASSERT_TRUE(result.resolved) << result.detail;
	const auto scratch = result.resolved->hold->scratch_dir;
	ASSERT_TRUE(std_fs::exists(scratch));
	result.resolved.reset();
	EXPECT_FALSE(std_fs::exists(scratch));
}

TEST_F(ArchiveImagesTest, TheHelperIsGoneAfterResolve)
{
	const auto result = Resolve("cd/game.iso");
	ASSERT_TRUE(result.resolved) << result.detail;
	EXPECT_EQ(ArchiveMount::ArchiveHelperClient::LiveHelpers(), 0);
}

} // namespace
