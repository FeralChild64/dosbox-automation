// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//
#include "dos/programs/mount.h"

#include "archive_fixtures.h"
#include "config/setup.h"
#include "dos/archive/archive_cache.h"
#include "dos/archive/archive_helper_client.h"
#include "dos/archive/archive_reader.h"
#include "dos/drives.h"
#include "dos/programs/mount_common.h"
#include "dos/programs/mount_policy.h"
#include "dosbox_test_fixture.h"
#include "misc/cross.h"
#include "test_temp_dir.h"
#include "utils/env_utils.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using ArchiveFixtures::FileEntry;
using ArchiveFixtures::Kind;
using ArchiveFixtures::WriteArchive;

namespace {

namespace std_fs = std::filesystem;

void SetEnv(const char* name, const char* value)
{
#if defined(WIN32)
	_putenv_s(name, value);
#else
	setenv(name, value, 1);
#endif
}

std::vector<FileEntry> Game()
{
	return {
	        {"game/", "", true},
	        {"game/doom.exe", std::string(3000, 'x')},
	        {"game/data/", "", true},
	        {"game/data/level.wad", std::string(5000, 'w')},
	        {"game/readme.txt", "hello"},
	};
}

std::vector<std::pair<std::string, MountVerdict>> verdicts = {};
std::vector<std::string> constructed                       = {};

void CaptureVerdict(const std_fs::path& raw, const MountVerdict& verdict)
{
	verdicts.emplace_back(raw.string(), verdict);
}
void CaptureConstruction(const char* host_path)
{
	constructed.emplace_back(host_path);
}

class MountArchiveTest : public DOSBoxTestFixture {
protected:
	void SetUp() override
	{
		DOSBoxTestFixture::SetUp();
		// The policy asserts its initialisation (gotcha 2026-08).
		MountPolicy::InitPolicyConfig({});
		tmp = TestTempDir::MakeUnique("mount_archive_");
		ASSERT_FALSE(tmp.empty());
		ArchiveMount::SetHelperPathOverride(DOSBOX_ARCHIVE_HELPER_PATH);
		Set("archive_cache_path", (tmp / "cache").string());
		Set("archive_sidecar_path", (tmp / "saves").string());
		MOUNT::ResetArchiveCacheForTests();
		verdicts.clear();
		constructed.clear();
		MountPolicy::verdict_hook            = &CaptureVerdict;
		MountPolicy::drive_construction_hook = &CaptureConstruction;
		program = std::make_unique<MOUNT>();
	}
	void TearDown() override
	{
		for (char d : {'D', 'E', 'F'}) {
			if (Drives.at(drive_index(d))) {
				UnmountHelper(d);
			}
		}
		MountPolicy::verdict_hook            = nullptr;
		MountPolicy::drive_construction_hook = nullptr;
		ArchiveMount::SetHelperPathOverride({});
		MOUNT::ResetArchiveCacheForTests();
		program.reset();
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
	std_fs::path Built(const std::string& name, const Kind kind,
	                   const std::vector<FileEntry>& entries = Game())
	{
		const auto path = tmp / name;
		auto error      = std::string();
		EXPECT_TRUE(WriteArchive(path, kind, entries, nullptr, &error))
		        << error;
		return path;
	}
	// ~Program deletes cmd, so the line lives on the heap and the old
	// one goes before the next (the mount tests leak theirs instead).
	CommandLine* SetCommand(const std::string& command_params)
	{
		delete program->cmd;
		program->cmd = new CommandLine("MOUNT", command_params);
		return program->cmd;
	}
	std::optional<MountParameters> Mount(const std::string& command_params)
	{
		return program->ProcessArguments(SetCommand(command_params));
	}
	// The archive drive, behind its union when the mount has a sidecar.
	static std::shared_ptr<ArchiveDrive> DriveAt(const char letter)
	{
		return ArchiveDriveOf(Drives.at(drive_index(letter)));
	}
	static std::shared_ptr<UnionDrive> UnionAt(const char letter)
	{
		return std::dynamic_pointer_cast<UnionDrive>(
		        Drives.at(drive_index(letter)));
	}

	std_fs::path tmp               = {};
	std::unique_ptr<MOUNT> program = {};
};

TEST_F(MountArchiveTest, MountsAZipFoundByExtensionAndMagic)
{
	const auto zip    = Built("doom.zip", Kind::ZipStored);
	const auto result = Mount("D " + zip.string());
	ASSERT_TRUE(result.has_value());
	EXPECT_EQ(result->type, MountType::Archive);
	EXPECT_FALSE(result->is_image_mode);
	EXPECT_EQ(result->mediaid, MediaId::HardDisk);
	auto drive = DriveAt('D');
	ASSERT_TRUE(drive);
	// The message stubs of dosbox_tests give every MSG_Get an empty
	// string, so the type is checked by its enum, not its text.
	EXPECT_EQ(drive->GetType(), DosDriveType::Archive);
	EXPECT_EQ(std::string(drive->GetInfo()), std_fs::canonical(zip).string());
	EXPECT_STREQ(drive->GetLabel(), "DOOM");
	// D10: the one top directory became the root.
	EXPECT_TRUE(drive->TestDir("DATA"));
	EXPECT_FALSE(drive->TestDir("GAME"));
	EXPECT_TRUE(drive->FileExists("README.TXT"));
	// The sink invariant: a verdict for the path before the construction.
	ASSERT_EQ(verdicts.size(), 1u);
	EXPECT_TRUE(verdicts[0].second.allowed);
	// Two constructions: the archive, then the sidecar's local drive.
	ASSERT_EQ(constructed.size(), 2u);
	EXPECT_EQ(constructed[0], std_fs::canonical(zip).string());
	EXPECT_TRUE(constructed[1].starts_with((tmp / "saves").string()))
	        << constructed[1];
	EXPECT_EQ(ArchiveMount::ArchiveHelperClient::LiveHelpers(), 1);
	// The cache got its entry and the tag.
	EXPECT_TRUE(std_fs::is_regular_file(tmp / "cache" /
	                                    ArchiveMount::CacheTagName));
	// Unmount ends the helper (once nothing else holds the drive).
	drive.reset();
	UnmountHelper('D');
	EXPECT_FALSE(Drives.at(drive_index('D')));
	EXPECT_EQ(ArchiveMount::ArchiveHelperClient::LiveHelpers(), 0);
}

TEST_F(MountArchiveTest, ExplicitTypeArchiveAndTheRootOption)
{
	const auto zip = Built("doom.zip", Kind::ZipStored);
	auto result = Mount("D " + zip.string() + " -t archive -root game\\data");
	ASSERT_TRUE(result.has_value());
	EXPECT_EQ(result->archive_root, "GAME\\DATA");
	auto drive = DriveAt('D');
	ASSERT_TRUE(drive);
	EXPECT_TRUE(drive->FileExists("LEVEL.WAD"));
	EXPECT_FALSE(drive->TestDir("DATA"));
	drive.reset();
	UnmountHelper('D');
	// A root that is not in the archive refuses the mount, after the
	// helper was started and before any drive exists.
	EXPECT_FALSE(Mount("D " + zip.string() + " -root NOPE").has_value());
	EXPECT_FALSE(Drives.at(drive_index('D')));
	EXPECT_EQ(ArchiveMount::ArchiveHelperClient::LiveHelpers(), 0);
	// Forward slashes and lower case are accepted.
	result = Mount("E " + zip.string() + " -root game/data/");
	ASSERT_TRUE(result.has_value());
	EXPECT_EQ(result->archive_root, "GAME\\DATA");
}

TEST_F(MountArchiveTest, JunkWithAnArchiveExtensionIsRefused)
{
	const auto junk = tmp / "notes.zip";
	{
		std::ofstream out(junk, std::ios::binary);
		out << std::string(600, 'a');
	}
	// Without -t: not an archive by magic, not an image either.
	EXPECT_FALSE(Mount("D " + junk.string()).has_value());
	// With -t archive: the policy says NotAnArchive.
	EXPECT_FALSE(Mount("D " + junk.string() + " -t archive").has_value());
	ASSERT_FALSE(verdicts.empty());
	EXPECT_EQ(verdicts.back().second.reason, DenyReason::NotAnArchive);
	EXPECT_TRUE(constructed.empty());
	// -t archive on a directory.
	EXPECT_FALSE(Mount("D " + tmp.string() + " -t archive").has_value());
}

TEST_F(MountArchiveTest, SequentialIsRefusedByDefaultAndCachedOnRequest)
{
	const auto tgz = Built("doom.tar.gz", Kind::TarGz);
	EXPECT_FALSE(Mount("D " + tgz.string()).has_value());
	ASSERT_FALSE(verdicts.empty());
	EXPECT_EQ(verdicts.back().second.reason, DenyReason::ArchiveSequential);
	EXPECT_EQ(ArchiveMount::ArchiveHelperClient::LiveHelpers(), 0);
	Set("archive_sequential", "cache");
	const auto result = Mount("D " + tgz.string());
	ASSERT_TRUE(result.has_value());
	auto drive = DriveAt('D');
	ASSERT_TRUE(drive);
	auto file = drive->FileOpen("README.TXT", OPEN_READ);
	ASSERT_TRUE(file);
	// The one pass landed in the cache entry.
	const auto entry = ArchiveMount::ArchiveCache::EntryDirectory(
	        tmp / "cache", "doom.tar", drive->Source().IdentityHash());
	EXPECT_TRUE(std_fs::is_regular_file(entry / ArchiveMount::FilesDirName / "1"));
}

TEST_F(MountArchiveTest, NoSidecarParsesAndAnUnavailableCacheRefuses)
{
	const auto zip = Built("doom.zip", Kind::ZipStored);
	auto result    = Mount("D " + zip.string() + " -nosidecar");
	ASSERT_TRUE(result.has_value());
	EXPECT_TRUE(result->no_sidecar);
	UnmountHelper('D');
	{
		std::ofstream out(tmp / "file", std::ios::binary);
		out << "x";
	}
	Set("archive_cache_path", (tmp / "file").string());
	MOUNT::ResetArchiveCacheForTests();
	EXPECT_FALSE(Mount("D " + zip.string()).has_value());
	EXPECT_EQ(ArchiveMount::ArchiveHelperClient::LiveHelpers(), 0);
	EXPECT_FALSE(Drives.at(drive_index('D')));
}

TEST_F(MountArchiveTest, TheCapsFromTheSettingsReachTheHelper)
{
	const auto zip = Built("doom.zip", Kind::ZipStored);
	Set("archive_max_entries", "2");
	EXPECT_FALSE(Mount("D " + zip.string()).has_value());
	ASSERT_FALSE(verdicts.empty());
	// The policy's own verdict was allowed; the refusal came from the
	// index, reported through the same hook with the mapped reason.
	EXPECT_EQ(verdicts.back().second.reason, DenyReason::ArchiveTooLarge);
	EXPECT_EQ(ArchiveMount::ArchiveHelperClient::LiveHelpers(), 0);
	// The scratch directory made for the helper went with the refusal.
	auto scratch_entries = 0;
	std::error_code ec   = {};
	for (const auto& e :
	     std_fs::directory_iterator(tmp / "cache" / "scratch", ec)) {
		(void)e;
		++scratch_entries;
	}
	EXPECT_EQ(scratch_entries, 0);
}

TEST_F(MountArchiveTest, ReconnectCommandOnlyTouchesArchiveDrives)
{
	const auto zip = Built("doom.zip", Kind::ZipStored);
	ASSERT_TRUE(Mount("D " + zip.string()).has_value());
	SetCommand("-reconnect D:");
	EXPECT_TRUE(program->HandleReconnect());
	EXPECT_EQ(DriveAt('D')->State(), ArchiveMountState::Ready);
	SetCommand("-reconnect Z:");
	// Handled: the message says it is no archive.
	EXPECT_TRUE(program->HandleReconnect());
	SetCommand("D " + zip.string());
	EXPECT_FALSE(program->HandleReconnect());
}

TEST_F(MountArchiveTest, TheOverlapCheckRefusesAMountedLetter)
{
	const auto zip = Built("doom.zip", Kind::ZipStored);
	ASSERT_TRUE(Mount("D " + zip.string()).has_value());
	EXPECT_FALSE(Mount("D " + zip.string()).has_value());
	EXPECT_EQ(ArchiveMount::ArchiveHelperClient::LiveHelpers(), 1);
}

TEST_F(MountArchiveTest, AMountGetsASidecarUnderSavesAndWritesLandThere)
{
	const auto zip = Built("doom.zip", Kind::ZipStored);
	auto result    = Mount("D " + zip.string());
	ASSERT_TRUE(result.has_value());
	auto drive = UnionAt('D');
	ASSERT_TRUE(drive);
	const auto hash = ArchiveDriveOf(drive)->Source().IdentityHash();
	EXPECT_EQ(drive->SidecarDirectory(),
	          tmp / "saves" / ("doom-" + hash.substr(0, 8)));
	EXPECT_TRUE(std_fs::is_regular_file(drive->SidecarDirectory() /
	                                    ArchiveMount::SidecarTomlName));
	EXPECT_TRUE(std_fs::is_regular_file(drive->SidecarDirectory() / ".lock"));
	auto file = drive->FileCreate("NEW.TXT", {});
	ASSERT_TRUE(file);
	file->Close();
	EXPECT_TRUE(std_fs::is_regular_file(drive->SidecarDirectory() / "NEW.TXT"));
	EXPECT_EQ(drive->GetType(), DosDriveType::Archive);
	drive.reset();
	UnmountHelper('D');
	// A remount finds the same sidecar by path.
	result = Mount("E " + zip.string());
	ASSERT_TRUE(result.has_value());
	EXPECT_TRUE(UnionAt('E')->FileExists("NEW.TXT"));
}

TEST_F(MountArchiveTest, NoSidecarMountsTheArchiveDriveAlone)
{
	const auto zip = Built("doom.zip", Kind::ZipStored);
	ASSERT_TRUE(Mount("D " + zip.string() + " -nosidecar").has_value());
	EXPECT_TRUE(DriveAt('D'));
	EXPECT_FALSE(UnionAt('D'));
	EXPECT_FALSE(std_fs::exists(tmp / "saves"));
}

TEST_F(MountArchiveTest, ASecondMountOfTheSameArchiveIsSidecarBusy)
{
	const auto zip = Built("doom.zip", Kind::ZipStored);
	ASSERT_TRUE(Mount("D " + zip.string()).has_value());
	EXPECT_FALSE(Mount("E " + zip.string()).has_value());
	ASSERT_FALSE(verdicts.empty());
	EXPECT_EQ(verdicts.back().second.reason, DenyReason::SidecarBusy);
	EXPECT_FALSE(Drives.at(drive_index('E')));
	// With -nosidecar beside it: allowed, it writes nothing.
	EXPECT_TRUE(Mount("E " + zip.string() + " -nosidecar").has_value());
	// Two roots of one archive are still one sidecar: refused too.
	EXPECT_FALSE(Mount("F " + zip.string() + " -root game").has_value());
}

TEST_F(MountArchiveTest, AnExplicitSidecarPathIsUsedAndValidated)
{
	const auto zip  = Built("doom.zip", Kind::ZipStored);
	const auto mine = tmp / "mine";
	std_fs::create_directories(mine);
	ASSERT_TRUE(Mount("D " + zip.string() + " -sidecar " + mine.string()).has_value());
	EXPECT_EQ(UnionAt('D')->SidecarDirectory(), std_fs::canonical(mine));
	UnmountHelper('D');
	EXPECT_FALSE(Mount("D " + zip.string() + " -sidecar " + (tmp / "nope").string())
	                     .has_value());
	EXPECT_EQ(verdicts.back().second.reason, DenyReason::DoesNotResolve);
}

// The fixture has no PSP file table: handles go through FCB mode.
constexpr bool fcb = true;

TEST_F(MountArchiveTest, AnArchiveFileOpenOnTheUnionKeepsTheDriveMounted)
{
	const auto zip = Built("doom.zip", Kind::ZipStored);
	ASSERT_TRUE(Mount("D " + zip.string()).has_value());
	uint16_t handle = 0;
	ASSERT_TRUE(DOS_OpenFile("D:\\README.TXT", OPEN_READ, &handle, fcb));
	UnmountHelper('D');
	EXPECT_TRUE(UnionAt('D'));
	// The sidecar is still this mount's: a second mount of it is refused.
	EXPECT_FALSE(Mount("E " + zip.string()).has_value());
	ASSERT_TRUE(DOS_CloseFile(handle, fcb));
	UnmountHelper('D');
	EXPECT_FALSE(Drives.at(drive_index('D')));
	EXPECT_TRUE(Mount("E " + zip.string()).has_value());
}

TEST_F(MountArchiveTest, ASidecarFileOpenOnTheUnionKeepsTheDriveMounted)
{
	const auto zip = Built("doom.zip", Kind::ZipStored);
	ASSERT_TRUE(Mount("D " + zip.string()).has_value());
	uint16_t handle = 0;
	ASSERT_TRUE(DOS_CreateFile("D:\\SAVE.DAT", {}, &handle, fcb));
	UnmountHelper('D');
	EXPECT_TRUE(UnionAt('D'));
	ASSERT_TRUE(DOS_CloseFile(handle, fcb));
	UnmountHelper('D');
	EXPECT_FALSE(Drives.at(drive_index('D')));
}

TEST_F(MountArchiveTest, AFileOpenOnAPlainArchiveDriveKeepsItMounted)
{
	const auto zip = Built("doom.zip", Kind::ZipStored);
	ASSERT_TRUE(Mount("D " + zip.string() + " -nosidecar").has_value());
	uint16_t handle = 0;
	ASSERT_TRUE(DOS_OpenFile("D:\\README.TXT", OPEN_READ, &handle, fcb));
	UnmountHelper('D');
	EXPECT_TRUE(DriveAt('D'));
	ASSERT_TRUE(DOS_CloseFile(handle, fcb));
	UnmountHelper('D');
	EXPECT_FALSE(Drives.at(drive_index('D')));
}

TEST_F(MountArchiveTest, ATildeInTheSidecarPathIsTheHomeDirectory)
{
	const auto zip  = Built("doom.zip", Kind::ZipStored);
	const auto mine = tmp / "mine";
	std_fs::create_directories(mine);
	const auto old_home = get_env_var("HOME");
	SetEnv("HOME", tmp.string().c_str());
	const auto result = Mount("D " + zip.string() + " -sidecar ~" +
	                          CROSS_FILESPLIT + "mine");
	SetEnv("HOME", old_home.c_str());
	ASSERT_TRUE(result.has_value());
	EXPECT_EQ(UnionAt('D')->SidecarDirectory(), std_fs::canonical(mine));
}

TEST_F(MountArchiveTest, ADosPathAsSidecarPathMapsToItsHostDirectory)
{
	const auto zip  = Built("doom.zip", Kind::ZipStored);
	const auto host = tmp / "host";
	std_fs::create_directories(host / "mine");
	ASSERT_TRUE(Mount("E " + host.string()).has_value());
	ASSERT_TRUE(Mount("D " + zip.string() + " -sidecar E:\\MINE").has_value());
	EXPECT_EQ(UnionAt('D')->SidecarDirectory(), std_fs::canonical(host / "mine"));
}

TEST_F(MountArchiveTest, WithPrARelativeSidecarPathIsRelativeToTheConfig)
{
	Built("doom.zip", Kind::ZipStored);
	std_fs::create_directories(tmp / "mine");
	control->config_files.push_back((tmp / "game.conf").string());
	const auto result = Mount("D doom.zip -pr -sidecar mine");
	control->config_files.pop_back();
	ASSERT_TRUE(result.has_value());
	EXPECT_EQ(UnionAt('D')->SidecarDirectory(), std_fs::canonical(tmp / "mine"));
}

TEST_F(MountArchiveTest, AReplacedArchiveGetsANewSidecarAndTheOldIsNamed)
{
	const auto zip = Built("doom.zip", Kind::ZipStored);
	ASSERT_TRUE(Mount("D " + zip.string()).has_value());
	const auto first = UnionAt('D')->SidecarDirectory();
	UnmountHelper('D');
	// A new release under the same name.
	Built("doom.zip",
	      Kind::ZipStored,
	      {
	              {"game/", "", true},
                      {"game/doom.exe", "v2"}
        });
	ASSERT_TRUE(Mount("D " + zip.string()).has_value());
	const auto second = UnionAt('D')->SidecarDirectory();
	EXPECT_NE(first, second);
	EXPECT_TRUE(std_fs::is_directory(first));
}

TEST_F(MountArchiveTest, ASidecarBesideTheArchiveIsTakenOnlyThroughTheWhitelist)
{
	const auto zip = Built("doom.zip", Kind::ZipStored);
	// Learn the key by mounting once, then move that sidecar beside the
	// archive.
	ASSERT_TRUE(Mount("D " + zip.string()).has_value());
	const auto under_saves = UnionAt('D')->SidecarDirectory();
	UnmountHelper('D');
	const auto beside = tmp / under_saves.filename();
	std_fs::rename(under_saves, beside);
	// The test fixture's policy is owner-trusted with the webserver off:
	// the archive's directory passes, so the sidecar beside it is used.
	ASSERT_TRUE(Mount("D " + zip.string()).has_value());
	EXPECT_EQ(UnionAt('D')->SidecarDirectory(), std_fs::canonical(beside));
	UnmountHelper('D');
	// With injection possible the whitelists apply: the archive is allowed
	// as an image root, its directory is not an allowed base, so the
	// sidecar beside it is skipped and a new one is created under saves.
	const auto conf = tmp / "policy.conf";
	{
		std::ofstream out(conf);
		out << "[webserver]\nmount_allowed_image_roots = " << tmp.string()
		    << "\n";
	}
	MountPolicy::InitPolicyConfig(conf);
	MountPolicy::SetInjectionPossible(true);
	ASSERT_TRUE(Mount("D " + zip.string()).has_value());
	MountPolicy::SetInjectionPossible(false);
	MountPolicy::InitPolicyConfig({});
	ASSERT_TRUE(UnionAt('D'));
	EXPECT_EQ(UnionAt('D')->SidecarDirectory().parent_path(), tmp / "saves");
	EXPECT_TRUE(std_fs::is_directory(beside));
}

TEST_F(MountArchiveTest, StartupCleanupRemovesAnExpiredEntryWithoutAnyMount)
{
	// An entry last used eight days ago, written with the cache's own clock
	// hook; the [dos] default TTL is seven days.
	const auto zip     = Built("doom.zip", Kind::ZipStored);
	auto opened_reader = ArchiveMount::ArchiveReader::Open(zip,
	                                                       ArchiveMount::Limits{});
	ASSERT_EQ(opened_reader.error, ArchiveMount::Error::None)
	        << opened_reader.detail;
	auto source    = std::move(opened_reader.reader);
	const auto now = std::chrono::duration_cast<std::chrono::seconds>(
	                         std::chrono::system_clock::now().time_since_epoch())
	                         .count();
	ArchiveMount::CacheOptions options = {};
	options.now = [now]() { return now - 8 * 24 * 60 * 60; };
	auto cache  = ArchiveMount::ArchiveCache::Open(tmp / "cache", options);
	ASSERT_EQ(cache.status, ArchiveMount::CacheStatus::Ok) << cache.detail;
	const auto entry = ArchiveMount::ArchiveCache::EntryDirectory(
	        tmp / "cache", "doom", source->IdentityHash());
	{
		auto lease = cache.cache->Acquire(*source, zip);
		ASSERT_EQ(lease.status, ArchiveMount::CacheStatus::Ok)
		        << lease.detail;
		std_fs::path member = {};
		auto detail         = std::string();
		int readme          = -1;
		for (const auto& e : source->Entries()) {
			if (e.utf8_path == "game/readme.txt") {
				readme = e.ordinal;
			}
		}
		ASSERT_EQ(lease.lease->MemberFile(readme, member, &detail),
		          ArchiveMount::CacheStatus::Ok)
		        << detail;
	}
	cache.cache.reset();
	ASSERT_TRUE(std_fs::exists(entry));

	MOUNT::ArchiveCacheStartup();

	EXPECT_FALSE(std_fs::exists(entry));
}

TEST_F(MountArchiveTest, ChangingCacheSettingsKeepsTheStoreWhileArchivesAreMounted)
{
	// The lease behind D points at the store; a second mount after a
	// settings change must not replace it under that lease.
	Set("archive_memory_limit", "0");
	const auto a = Built("a.zip", Kind::ZipStored);
	const auto b = Built("b.zip",
	                     Kind::ZipStored,
	                     {
	                             {"x.txt", "x"}
        });
	ASSERT_TRUE(Mount("D " + a.string()));
	Set("archive_cache_max_size", "8192");
	ASSERT_TRUE(Mount("E " + b.string()));
	auto file = DriveAt('D')->FileOpen("README.TXT", OPEN_READ);
	ASSERT_TRUE(file);
	uint8_t buf[16] = {};
	uint16_t n      = sizeof(buf);
	EXPECT_TRUE(file->Read(buf, &n));
	EXPECT_EQ(std::string(reinterpret_cast<char*>(buf), n), "hello");
	file->Close();
}

TEST_F(MountArchiveTest, ReadOnlyMountsTheArchiveDriveWithoutASidecar)
{
	const auto a = Built("a.zip", Kind::ZipStored);
	ASSERT_TRUE(Mount("D " + a.string() + " -ro"));
	EXPECT_TRUE(DriveAt('D'));
	EXPECT_FALSE(UnionAt('D'));
	EXPECT_FALSE(std_fs::exists(tmp / "saves"));
}

TEST_F(MountArchiveTest, ADriveNumberWithAnExplicitArchiveTypeIsRefused)
{
	const auto a = Built("a.zip", Kind::ZipStored);
	EXPECT_FALSE(Mount("0 " + a.string() + " -t archive").has_value());
	EXPECT_FALSE(Mount("D " + a.string() + " -t archive -fs none").has_value());
	EXPECT_FALSE(Drives.at(drive_index('D')));
}

TEST_F(MountArchiveTest, ArchiveOptionsOnADirectoryMountAreRefused)
{
	const auto dir = tmp / "plain";
	std_fs::create_directories(dir);
	EXPECT_FALSE(Mount("D " + dir.string() + " -nosidecar").has_value());
	EXPECT_FALSE(Mount("D " + dir.string() + " -sidecar " + (tmp / "s").string())
	                     .has_value());
	EXPECT_FALSE(Mount("D " + dir.string() + " -root GAME").has_value());
	EXPECT_FALSE(Drives.at(drive_index('D')));
}

} // namespace

TEST_F(MountArchiveTest, AnUnsupportedMethodSaysSoAndAsksForTheArchive)
{
	auto text = std::string(MOUNT::ArchiveUnsupportedMessage);
	std::replace(text.begin(), text.end(), '\n', ' ');
	EXPECT_NE(text.find("not implemented"), std::string::npos) << text;
	EXPECT_NE(text.find("https://github.com/dosbox-automation/dosbox-automation/issues"),
	          std::string::npos)
	        << text;
	EXPECT_NE(text.find("attach the archive"), std::string::npos) << text;
	EXPECT_NE(text.find("link"), std::string::npos) << text;
}

