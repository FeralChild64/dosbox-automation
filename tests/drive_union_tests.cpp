// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//
#include "dos/drives.h"

#include "archive_fixtures.h"
#include "dos/archive/archive_reader.h"
#include "dos/archive/sidecar_record.h"
#include "dos/dos.h"
#include "dosbox_test_fixture.h"
#include "drive_search_helper.h"
#include "hardware/memory.h"
#include "test_temp_dir.h"
#include "utils/fs_utils.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using ArchiveFixtures::FileEntry;
using ArchiveFixtures::Kind;
using ArchiveFixtures::WriteArchive;
using namespace ArchiveMount;

namespace {

constexpr bool fcb = true;

std::vector<FileEntry> Game()
{
	return {
	        {"game/", "", true},
	        {"game/doom.exe", std::string(3000, 'x')},
	        {"game/data/", "", true},
	        {"game/data/level.wad", std::string(5000, 'w')},
	        {"game/readme.txt", "hello"},
	        {"game/save.dat", "old save"},
	        {"game/skiega", "map data"},
	};
}

class UnionDriveTest : public DOSBoxTestFixture {
protected:
	void SetUp() override
	{
		DOSBoxTestFixture::SetUp();
		tmp = TestTempDir::MakeUnique("drive_union_");
		ASSERT_FALSE(tmp.empty());
		sidecar = tmp / "sidecar";
		std_fs::create_directories(sidecar);
		archive    = tmp / "doom.zip";
		auto error = std::string();
		ASSERT_TRUE(WriteArchive(archive, Kind::ZipStored, Game(), nullptr, &error))
		        << error;
		previous = Drives.at(25);
		ASSERT_TRUE(Mount());
	}
	void TearDown() override
	{
		Drives.at(25) = previous;
		union_drive.reset();
		std::error_code ec = {};
		std_fs::remove_all(tmp, ec);
		DOSBoxTestFixture::TearDown();
	}
	// A fresh union over the same archive and sidecar: what a remount does.
	// MOUNT seeds the record's identity (Task 4); here it is a fixed one.
	bool Mount()
	{
		Drives.at(25) = nullptr;
		union_drive.reset();
		auto opened = ArchiveReader::Open(archive, Limits{});
		EXPECT_EQ(opened.error, Error::None) << opened.detail;
		if (opened.error != Error::None) {
			return false;
		}
		auto lower = ArchiveDrive::Create(std::move(opened.reader),
		                                  {},
		                                  "GAME",
		                                  "DOOM",
		                                  archive.string(),
		                                  ArchiveDrive::Options{});
		EXPECT_TRUE(lower.drive) << lower.error;
		if (!lower.drive) {
			return false;
		}
		SidecarRecord identity = {};
		identity.archive_path  = archive.string();
		identity.identity_hash = std::string(64, 'a');
		identity.entry_count   = 7;
		identity.total_bytes   = 8021;
		auto made              = UnionDrive::Create(
                        lower.drive, sidecar, 'Z', identity, &record_error);
		EXPECT_TRUE(made) << record_error;
		if (!made) {
			return false;
		}
		union_drive   = made;
		Drives.at(25) = made;
		return true;
	}
	// FreeBSD's local drive has no attribute store (no sys/xattr.h), so
	// every SetFileAttr fails there; the tests that need the store skip.
	bool HostStoresDosAttributes()
	{
		const auto probe = (tmp / "attr.prb").string();
		std::ofstream(probe) << "p";
		FatAttributeFlags hidden = {};
		hidden.hidden            = true;
		const bool stored = local_drive_set_attributes(probe.c_str(),
		                                               hidden) == DOSERR_NONE;
		FatAttributeFlags back = {};
		local_drive_get_attributes(probe.c_str(), back);
		std::error_code ec = {};
		std_fs::remove(probe, ec);
		return stored && back.hidden;
	}
	std::string ReadDos(const char* name)
	{
		uint16_t handle = 0;
		if (!DOS_OpenFile(name, OPEN_READ, &handle, fcb)) {
			return "<open failed " + std::to_string(dos.errorcode) + ">";
		}
		std::string out = {};
		uint8_t buf[512];
		for (;;) {
			uint16_t n = sizeof(buf);
			if (!DOS_ReadFile(handle, buf, &n, fcb) || n == 0) {
				break;
			}
			out.append(reinterpret_cast<const char*>(buf), n);
		}
		DOS_CloseFile(handle, fcb);
		return out;
	}
	bool WriteDos(const char* name, const std::string& data, const bool create)
	{
		uint16_t handle = 0;
		if (create) {
			if (!DOS_CreateFile(name, {}, &handle, fcb)) {
				return false;
			}
		} else if (!DOS_OpenFile(name, OPEN_READWRITE, &handle, fcb)) {
			return false;
		}
		auto n        = static_cast<uint16_t>(data.size());
		const bool ok = DOS_WriteFile(handle,
		                              reinterpret_cast<uint8_t*>(
		                                      const_cast<char*>(data.data())),
		                              &n,
		                              fcb) &&
		                n == data.size();
		DOS_CloseFile(handle, fcb);
		return ok;
	}
	std::vector<std::string> Dir(const char* search)
	{
		std::vector<std::string> names = {};
		FatAttributeFlags attr         = {};
		attr.directory                 = true;
		if (!DOS_FindFirst(search, attr, false)) {
			return names;
		}
		for (;;) {
			DOS_DTA dta(dos.dta());
			DOS_DTA::Result result = {};
			dta.GetResult(result);
			names.push_back(result.name);
			if (!DOS_FindNext()) {
				break;
			}
		}
		return names;
	}
	std::string Host(const std::string& relative)
	{
		std::ifstream in(sidecar / relative, std::ios::binary);
		return std::string(std::istreambuf_iterator<char>(in), {});
	}

	std_fs::path tmp = {}, sidecar = {}, archive = {};
	std::shared_ptr<DOS_Drive> previous     = {};
	std::shared_ptr<UnionDrive> union_drive = {};
	std::string record_error                = {};
};

TEST_F(UnionDriveTest, ReadsFallThroughAndTheListingMergesBothLayers)
{
	EXPECT_EQ(ReadDos("Z:\\README.TXT"), "hello");
	EXPECT_EQ(Dir("Z:\\*.*"),
	          (std::vector<std::string>{"DOOM.EXE", "DATA", "README.TXT", "SAVE.DAT", "SKIEGA"}));
	ASSERT_TRUE(WriteDos("Z:\\NEW.TXT", "new", true));
	EXPECT_EQ(Host("NEW.TXT"), "new");
	EXPECT_EQ(ReadDos("Z:\\NEW.TXT"), "new");
	auto listing = Dir("Z:\\*.*");
	std::sort(listing.begin(), listing.end());
	EXPECT_EQ(listing,
	          (std::vector<std::string>{
	                  "DATA", "DOOM.EXE", "NEW.TXT", "README.TXT", "SAVE.DAT", "SKIEGA"}));
	EXPECT_TRUE(DOS_FileExists("Z:\\NEW.TXT"));
	EXPECT_FALSE(union_drive->IsReadOnly());
	uint16_t bs = 0, tc = 0, fc = 0;
	uint8_t sc = 0;
	ASSERT_TRUE(union_drive->AllocationInfo(&bs, &sc, &tc, &fc));
	EXPECT_GT(fc, 0); // the sidecar's free space, not the archive's zero
}

TEST_F(UnionDriveTest, AWriteCopiesTheMemberUpOnTheFirstWriteNotOnOpen)
{
	uint16_t handle = 0;
	ASSERT_TRUE(DOS_OpenFile("Z:\\SAVE.DAT", OPEN_READWRITE, &handle, fcb));
	EXPECT_FALSE(std_fs::exists(sidecar / "SAVE.DAT"));
	uint8_t buf[8] = {};
	uint16_t n     = 3;
	ASSERT_TRUE(DOS_ReadFile(handle, buf, &n, fcb));
	EXPECT_EQ(std::string(reinterpret_cast<char*>(buf), n), "old");
	n = 3;
	ASSERT_TRUE(DOS_WriteFile(
	        handle, reinterpret_cast<uint8_t*>(const_cast<char*>("NEW")), &n, fcb));
	EXPECT_TRUE(std_fs::exists(sidecar / "SAVE.DAT"));
	ASSERT_TRUE(DOS_CloseFile(handle, fcb));
	EXPECT_EQ(Host("SAVE.DAT"), "oldNEWve");
	EXPECT_EQ(ReadDos("Z:\\SAVE.DAT"), "oldNEWve");
	// The archive's own bytes are untouched.
	auto opened = ArchiveReader::Open(archive, Limits{});
	ASSERT_EQ(opened.error, Error::None);
	std::vector<uint8_t> out = {};
	ASSERT_EQ(opened.reader->Materialize(5, out, nullptr), Error::None);
	EXPECT_EQ(std::string(out.begin(), out.end()), "old save");
}

TEST_F(UnionDriveTest, ACopiedUpFileKeepsTheMembersDateAndLosesReadOnly)
{
	ASSERT_TRUE(WriteDos("Z:\\README.TXT", "X", false));
	FatAttributeFlags attr = {};
	ASSERT_TRUE(DOS_GetFileAttr("Z:\\README.TXT", &attr));
	EXPECT_FALSE(attr.read_only);
	// A plain copy-up (the rename) carries the member's 1990-01-01 stamp;
	// the written README above carries the write time, as DOS would.
	ASSERT_TRUE(DOS_Rename("Z:\\SAVE.DAT", "Z:\\SAVE.BAK"));
	ASSERT_TRUE(DOS_GetFileAttr("Z:\\SAVE.BAK", &attr));
	EXPECT_FALSE(attr.read_only);
	const auto stamp = std_fs::last_write_time(sidecar / "SAVE.BAK");
	const auto unix  = std::chrono::duration_cast<std::chrono::seconds>(
                                  stamp - std_fs::file_time_type::clock::now() +
                                  std::chrono::system_clock::now().time_since_epoch())
	                          .count();
	EXPECT_NEAR(static_cast<double>(unix), 631152000.0, 86400.0 * 2);
}

TEST_F(UnionDriveTest, DeleteAndRenameOfLowerEntriesBecomeChanges)
{
	ASSERT_TRUE(DOS_UnlinkFile("Z:\\README.TXT"));
	EXPECT_FALSE(DOS_FileExists("Z:\\README.TXT"));
	EXPECT_EQ(ReadDos("Z:\\README.TXT"), "<open failed 2>");
	ASSERT_TRUE(DOS_Rename("Z:\\SAVE.DAT", "Z:\\SAVE.BAK"));
	EXPECT_FALSE(DOS_FileExists("Z:\\SAVE.DAT"));
	EXPECT_EQ(ReadDos("Z:\\SAVE.BAK"), "old save");
	EXPECT_EQ(Host("SAVE.BAK"), "old save");
	// Not written yet: the two-second window.
	EXPECT_FALSE(std_fs::exists(sidecar / SidecarTomlName) &&
	             SidecarRecord::Load(sidecar / SidecarTomlName, nullptr)
	                             ->changes.size() == 2);
	ASSERT_TRUE(union_drive->Flush(&record_error)) << record_error;
	const auto record = SidecarRecord::Load(sidecar / SidecarTomlName,
	                                        &record_error);
	ASSERT_TRUE(record) << record_error;
	ASSERT_EQ(record->changes.size(), 2u);
	EXPECT_EQ(record->changes[0].path, "README.TXT");
	EXPECT_EQ(record->changes[1].op, SidecarChange::Op::Rename);
	EXPECT_EQ(record->changes[1].to, "SAVE.BAK");
	// A remount reads the record back: README gone, SAVE.BAK there.
	ASSERT_TRUE(Mount());
	EXPECT_EQ(union_drive->Record().identity_hash, std::string(64, 'a'));
	EXPECT_FALSE(DOS_FileExists("Z:\\README.TXT"));
	EXPECT_FALSE(DOS_FileExists("Z:\\SAVE.DAT"));
	EXPECT_EQ(ReadDos("Z:\\SAVE.BAK"), "old save");
	auto listing = Dir("Z:\\*.*");
	std::sort(listing.begin(), listing.end());
	EXPECT_EQ(listing,
	          (std::vector<std::string>{"DATA", "DOOM.EXE", "SAVE.BAK", "SKIEGA"}));
}

TEST_F(UnionDriveTest, DeleteThenCreateOfOneNameInOneSessionSurvivesAReload)
{
	ASSERT_TRUE(DOS_UnlinkFile("Z:\\SAVE.DAT"));
	ASSERT_TRUE(WriteDos("Z:\\SAVE.DAT", "fresh", true));
	EXPECT_EQ(ReadDos("Z:\\SAVE.DAT"), "fresh");
	ASSERT_TRUE(union_drive->Flush(&record_error)) << record_error;
	const auto record = SidecarRecord::Load(sidecar / SidecarTomlName,
	                                        &record_error);
	ASSERT_TRUE(record) << record_error;
	EXPECT_TRUE(record->changes.empty());
	ASSERT_TRUE(Mount());
	EXPECT_EQ(ReadDos("Z:\\SAVE.DAT"), "fresh");
}

TEST_F(UnionDriveTest, DirectoriesAcrossTheLayers)
{
	ASSERT_TRUE(DOS_MakeDir("Z:\\SAVES"));
	EXPECT_TRUE(std_fs::is_directory(sidecar / "SAVES"));
	ASSERT_TRUE(WriteDos("Z:\\SAVES\\A.SAV", "a", true));
	// A file below a lower-only directory: the parent is made in the sidecar.
	ASSERT_TRUE(WriteDos("Z:\\DATA\\EXTRA.WAD", "e", true));
	EXPECT_EQ(Host("DATA/EXTRA.WAD"), "e");
	auto listing = Dir("Z:\\DATA\\*.*");
	std::sort(listing.begin(), listing.end());
	EXPECT_EQ(listing,
	          (std::vector<std::string>{".", "..", "EXTRA.WAD", "LEVEL.WAD"}));
	// A directory both layers hold cannot be renamed (D3): the lower half
	// would stay behind under the old name.
	EXPECT_FALSE(DOS_Rename("Z:\\DATA", "Z:\\OLDDATA"));
	EXPECT_EQ(dos.errorcode, DOSERR_ACCESS_DENIED);
	EXPECT_TRUE(union_drive->TestDir("DATA"));
	// Removing a lower directory that is not empty on either side is refused.
	EXPECT_FALSE(DOS_RemoveDir("Z:\\DATA"));
	EXPECT_EQ(dos.errorcode, DOSERR_ACCESS_DENIED);
	// Empty it on both sides, then it goes as a change.
	ASSERT_TRUE(DOS_UnlinkFile("Z:\\DATA\\EXTRA.WAD"));
	ASSERT_TRUE(DOS_UnlinkFile("Z:\\DATA\\LEVEL.WAD"));
	ASSERT_TRUE(DOS_RemoveDir("Z:\\DATA"));
	EXPECT_FALSE(union_drive->TestDir("DATA"));
	// A sidecar-only directory renames within the sidecar.
	ASSERT_TRUE(DOS_Rename("Z:\\SAVES", "Z:\\OLDSAVES"));
	EXPECT_TRUE(std_fs::is_directory(sidecar / "OLDSAVES"));
	EXPECT_FALSE(std_fs::exists(sidecar / "SAVES"));
	ASSERT_TRUE(union_drive->Flush(&record_error)) << record_error;
	ASSERT_TRUE(Mount());
	EXPECT_FALSE(union_drive->TestDir("DATA"));
	EXPECT_TRUE(union_drive->TestDir("OLDSAVES"));
	// Recreating the directory drops its delete; the files the game deleted
	// below it stay deleted, so the directory comes back empty.
	ASSERT_TRUE(DOS_MakeDir("Z:\\DATA"));
	EXPECT_TRUE(union_drive->TestDir("DATA"));
	EXPECT_FALSE(DOS_FileExists("Z:\\DATA\\LEVEL.WAD"));
	listing = Dir("Z:\\DATA\\*.*");
	std::sort(listing.begin(), listing.end());
	EXPECT_EQ(listing, (std::vector<std::string>{".", ".."}));
}

TEST_F(UnionDriveTest, AMissingParentIsPathNotFoundInEveryLayer)
{
	EXPECT_FALSE(DOS_MakeDir("Z:\\NOSUCH\\SUB"));
	EXPECT_EQ(dos.errorcode, DOSERR_PATH_NOT_FOUND);
	EXPECT_FALSE(WriteDos("Z:\\NOSUCH\\SAVE.DAT", "s", true));
	EXPECT_EQ(dos.errorcode, DOSERR_PATH_NOT_FOUND);
	EXPECT_FALSE(union_drive->Rename("SAVE.DAT", "NOSUCH\\SAVE.DAT"));
	EXPECT_EQ(dos.errorcode, DOSERR_PATH_NOT_FOUND);
	// DOS_Rename answers access denied for any drive's failed rename.
	EXPECT_FALSE(DOS_Rename("Z:\\SAVE.DAT", "Z:\\NOSUCH\\SAVE.DAT"));
	EXPECT_FALSE(std_fs::exists(sidecar / "NOSUCH"));
	EXPECT_TRUE(DOS_FileExists("Z:\\SAVE.DAT"));
	// A lower directory the game removed does not come back through a child.
	ASSERT_TRUE(DOS_UnlinkFile("Z:\\DATA\\LEVEL.WAD"));
	ASSERT_TRUE(DOS_RemoveDir("Z:\\DATA"));
	EXPECT_FALSE(WriteDos("Z:\\DATA\\NEW.WAD", "n", true));
	EXPECT_EQ(dos.errorcode, DOSERR_PATH_NOT_FOUND);
	EXPECT_FALSE(DOS_MakeDir("Z:\\DATA\\SUB"));
	EXPECT_EQ(dos.errorcode, DOSERR_PATH_NOT_FOUND);
	EXPECT_FALSE(union_drive->TestDir("DATA"));
	EXPECT_FALSE(std_fs::exists(sidecar / "DATA"));
}

TEST_F(UnionDriveTest, AFileCannotShadowALowerDirectory)
{
	EXPECT_FALSE(WriteDos("Z:\\DATA", "x", true));
	EXPECT_EQ(dos.errorcode, DOSERR_ACCESS_DENIED);
	EXPECT_FALSE(std_fs::exists(sidecar / "DATA"));
	EXPECT_FALSE(union_drive->Rename("SAVE.DAT", "DATA"));
	EXPECT_EQ(dos.errorcode, DOSERR_ACCESS_DENIED);
	EXPECT_TRUE(union_drive->TestDir("DATA"));
	EXPECT_TRUE(DOS_FileExists("Z:\\SAVE.DAT"));
	// A lower directory the game removed can be shadowed by a file, and
	// creating that file does not bring the directory back.
	ASSERT_TRUE(DOS_UnlinkFile("Z:\\DATA\\LEVEL.WAD"));
	ASSERT_TRUE(DOS_RemoveDir("Z:\\DATA"));
	EXPECT_TRUE(WriteDos("Z:\\DATA", "x", true));
	EXPECT_FALSE(union_drive->TestDir("DATA"));
	EXPECT_TRUE(DOS_FileExists("Z:\\DATA"));
	ASSERT_TRUE(Mount());
	EXPECT_FALSE(union_drive->TestDir("DATA"));
	EXPECT_EQ(ReadDos("Z:\\DATA"), "x");
}

TEST_F(UnionDriveTest, SetAttrCopiesUpAndRenamingAShadowingFileHidesTheLowerOne)
{
	if (!HostStoresDosAttributes()) {
		GTEST_SKIP() << "this host stores no DOS attributes";
	}
	FatAttributeFlags hidden = {};
	hidden.hidden            = true;
	ASSERT_TRUE(DOS_SetFileAttr("Z:\\README.TXT", hidden));
	EXPECT_TRUE(std_fs::exists(sidecar / "README.TXT"));
	FatAttributeFlags attr = {};
	ASSERT_TRUE(DOS_GetFileAttr("Z:\\README.TXT", &attr));
	EXPECT_TRUE(attr.hidden);
	// README.TXT now lives in the sidecar over the lower one; renaming it
	// away must not let the lower copy reappear.
	ASSERT_TRUE(DOS_Rename("Z:\\README.TXT", "Z:\\README.OLD"));
	EXPECT_FALSE(DOS_FileExists("Z:\\README.TXT"));
	ASSERT_TRUE(union_drive->Flush(&record_error)) << record_error;
	ASSERT_TRUE(Mount());
	EXPECT_FALSE(DOS_FileExists("Z:\\README.TXT"));
	EXPECT_TRUE(DOS_FileExists("Z:\\README.OLD"));
}

TEST_F(UnionDriveTest, LowerEntriesAreWritableThroughTheUnion)
{
	// The archive drive marks its members read-only; through the union a
	// member is writable by copy-up, and DEL refuses a read-only file.
	FatAttributeFlags attr = {};
	ASSERT_TRUE(DOS_GetFileAttr("Z:\\README.TXT", &attr));
	EXPECT_FALSE(attr.read_only);
	ASSERT_TRUE(DOS_GetFileAttr("Z:\\DATA", &attr));
	EXPECT_TRUE(attr.directory);
	EXPECT_FALSE(attr.read_only);
	FatAttributeFlags all = {};
	all.directory         = true;
	ASSERT_TRUE(DOS_FindFirst("Z:\\*.*", all, false));
	for (;;) {
		DOS_DTA dta(dos.dta());
		DOS_DTA::Result result = {};
		dta.GetResult(result);
		EXPECT_FALSE(result.attr.read_only) << result.name;
		if (!DOS_FindNext()) {
			break;
		}
	}
}

TEST_F(UnionDriveTest, AReadOnlyBitSetByTheGameIsKeptInTheSidecar)
{
	if (!HostStoresDosAttributes()) {
		GTEST_SKIP() << "this host stores no DOS attributes";
	}
	FatAttributeFlags ro = {};
	ro.read_only         = true;
	ASSERT_TRUE(DOS_SetFileAttr("Z:\\README.TXT", ro));
	EXPECT_TRUE(std_fs::exists(sidecar / "README.TXT"));
	FatAttributeFlags attr = {};
	ASSERT_TRUE(DOS_GetFileAttr("Z:\\README.TXT", &attr));
	EXPECT_TRUE(attr.read_only);
}

TEST_F(UnionDriveTest, AFailedAttributeSetLeavesNoCopyInTheSidecar)
{
	if (HostStoresDosAttributes()) {
		GTEST_SKIP() << "this host stores DOS attributes";
	}
	FatAttributeFlags hidden = {};
	hidden.hidden            = true;
	EXPECT_FALSE(DOS_SetFileAttr("Z:\\README.TXT", hidden));
	EXPECT_EQ(dos.errorcode, DOSERR_ACCESS_DENIED);
	EXPECT_FALSE(std_fs::exists(sidecar / "README.TXT"));
	EXPECT_EQ(ReadDos("Z:\\README.TXT"), "hello");
	EXPECT_FALSE(DOS_SetFileAttr("Z:\\DATA", hidden));
	EXPECT_FALSE(std_fs::exists(sidecar / "DATA"));
	EXPECT_TRUE(DOS_FileExists("Z:\\DATA\\LEVEL.WAD"));
}

TEST_F(UnionDriveTest, DeletingACopiedUpFileKeepsTheLowerOneHidden)
{
	ASSERT_TRUE(WriteDos("Z:\\README.TXT", "X", false));
	EXPECT_TRUE(std_fs::exists(sidecar / "README.TXT"));
	ASSERT_TRUE(DOS_UnlinkFile("Z:\\README.TXT"));
	EXPECT_FALSE(std_fs::exists(sidecar / "README.TXT"));
	EXPECT_FALSE(DOS_FileExists("Z:\\README.TXT"));
	ASSERT_TRUE(union_drive->Flush(&record_error)) << record_error;
	ASSERT_TRUE(Mount());
	// Without a recorded delete the archive's README.TXT would resurface.
	EXPECT_FALSE(DOS_FileExists("Z:\\README.TXT"));
	EXPECT_EQ(union_drive->Record().changes.size(), 1u);
}

#if !defined(WIN32)
TEST_F(UnionDriveTest, APlantedLinkInTheSidecarIsNeverWrittenThrough)
{
	const auto victim = tmp / "victim.txt";
	{
		std::ofstream out(victim);
		out << "keep";
	}
	std_fs::create_symlink(victim, sidecar / "SAVE.DAT");
	// The upper "has" SAVE.DAT, but it is a link: refused, the lower stays.
	EXPECT_FALSE(WriteDos("Z:\\SAVE.DAT", "X", false));
	EXPECT_EQ(dos.errorcode, DOSERR_ACCESS_DENIED);
	EXPECT_EQ(Host("../victim.txt"), "keep");
	std_fs::remove(sidecar / "SAVE.DAT");
	std_fs::create_directory_symlink(tmp, sidecar / "DATA");
	EXPECT_FALSE(WriteDos("Z:\\DATA\\NEW.WAD", "n", true));
	EXPECT_FALSE(std_fs::exists(tmp / "NEW.WAD"));
}
#endif

TEST_F(UnionDriveTest, TheRecordIsWrittenOnUnmountAndByFlushAll)
{
	ASSERT_TRUE(DOS_UnlinkFile("Z:\\README.TXT"));
	UnionDrive::FlushAll();
	auto record = SidecarRecord::Load(sidecar / SidecarTomlName, &record_error);
	ASSERT_TRUE(record) << record_error;
	EXPECT_EQ(record->changes.size(), 1u);
	ASSERT_TRUE(DOS_UnlinkFile("Z:\\SAVE.DAT"));
	Drives.at(25) = nullptr;
	union_drive.reset(); // the destructor writes
	record = SidecarRecord::Load(sidecar / SidecarTomlName, &record_error);
	ASSERT_TRUE(record) << record_error;
	EXPECT_EQ(record->changes.size(), 2u);
	EXPECT_EQ(UnionDrive::LiveUnions(), 0);
}

TEST_F(UnionDriveTest, RemoveDirInsideAFindNextLoopKeepsTheCallersSearch)
{
	// A DELTREE-style walk: RMDIR on each empty directory as the
	// enumeration reaches it, and the enumeration still reaches the end.
	ASSERT_TRUE(DOS_MakeDir("Z:\\EMPTY1"));
	ASSERT_TRUE(DOS_MakeDir("Z:\\EMPTY2"));
	FatAttributeFlags all = {};
	all.directory         = true;
	ASSERT_TRUE(DOS_FindFirst("Z:\\*.*", all, false));
	std::vector<std::string> seen = {};
	for (;;) {
		DOS_DTA dta(dos.dta());
		DOS_DTA::Result r = {};
		dta.GetResult(r);
		seen.push_back(r.name);
		if (r.name == "EMPTY1" || r.name == "EMPTY2") {
			EXPECT_TRUE(DOS_RemoveDir(("Z:\\" + r.name).c_str()));
		}
		if (!DOS_FindNext()) {
			break;
		}
	}
	for (const char* name :
	     {"EMPTY1", "EMPTY2", "DOOM.EXE", "DATA", "README.TXT", "SAVE.DAT"}) {
		EXPECT_NE(std::find(seen.begin(), seen.end(), name), seen.end())
		        << name;
	}
	EXPECT_FALSE(DOS_FileExists("Z:\\EMPTY1"));
}

TEST_F(UnionDriveTest, AChangedArchiveMakesEveryEntryPointNotReady)
{
	// D16 through the union: nothing falls through to the sidecar alone,
	// and nothing created there shadows a member the archive still has.
	ASSERT_TRUE(WriteDos("Z:\\MINE.TXT", "mine", true));
	FatAttributeFlags all = {};
	all.directory         = true;
	ASSERT_TRUE(DOS_FindFirst("Z:\\*.*", all, false));
	std_fs::last_write_time(archive,
	                        std_fs::last_write_time(archive) +
	                                std::chrono::hours(1));
	uint16_t handle = 0;
	EXPECT_FALSE(DOS_OpenFile("Z:\\README.TXT", OPEN_READ, &handle, fcb));
	EXPECT_EQ(dos.errorcode, DOSERR_DRIVE_NOT_READY);
	ASSERT_EQ(ArchiveDriveOf(union_drive)->State(), ArchiveMountState::Changed);

	const auto refused = [](const bool ok) {
		const bool not_ready = dos.errorcode == DOSERR_DRIVE_NOT_READY;
		dos.errorcode        = 0;
		return !ok && not_ready;
	};
	dos.errorcode = 0;
	EXPECT_TRUE(refused(DOS_FindNext()));
	EXPECT_TRUE(refused(DOS_FindFirst("Z:\\*.*", all, false)));
	EXPECT_TRUE(refused(union_drive->FileOpen("MINE.TXT", OPEN_READ) != nullptr));
	EXPECT_TRUE(refused(union_drive->FileCreate("README.TXT", {}) != nullptr));
	EXPECT_TRUE(refused(union_drive->FileUnlink("SAVE.DAT")));
	EXPECT_TRUE(refused(union_drive->MakeDir("NEWDIR")));
	EXPECT_TRUE(refused(union_drive->RemoveDir("DATA")));
	EXPECT_TRUE(refused(union_drive->Rename("SAVE.DAT", "SAVE.OLD")));
	EXPECT_TRUE(refused(union_drive->SetFileAttr("SAVE.DAT", {})));
	EXPECT_TRUE(refused(union_drive->TestDir("DATA")));
	EXPECT_TRUE(refused(union_drive->FileExists("MINE.TXT")));
	FatAttributeFlags attr = {};
	EXPECT_TRUE(refused(union_drive->GetFileAttr("MINE.TXT", &attr)));
	EXPECT_FALSE(std_fs::exists(sidecar / "README.TXT"));
	EXPECT_FALSE(std_fs::exists(sidecar / "NEWDIR"));
	EXPECT_TRUE(union_drive->Record().changes.empty());
}

TEST_F(UnionDriveTest, AChangedArchiveAnswersNotReadyThroughDos)
{
	ASSERT_TRUE(WriteDos("Z:\\MINE.TXT", "mine", true));
	std_fs::last_write_time(archive,
	                        std_fs::last_write_time(archive) +
	                                std::chrono::hours(1));
	uint16_t handle = 0;
	DOS_OpenFile("Z:\\README.TXT", OPEN_READ, &handle, fcb);
	ASSERT_EQ(ArchiveDriveOf(union_drive)->State(), ArchiveMountState::Changed);

	const auto refused = [](const bool ok) {
		const auto error = dos.errorcode;
		dos.errorcode    = 0;
		return ok ? -1 : static_cast<int>(error);
	};
	constexpr int not_ready = DOSERR_DRIVE_NOT_READY;
	FatAttributeFlags attr  = {};
	EXPECT_EQ(refused(DOS_OpenFile("Z:\\MINE.TXT", OPEN_READ, &handle, fcb)),
	          not_ready);
	EXPECT_EQ(refused(DOS_CreateFile("Z:\\NEW.TXT", {}, &handle, fcb)), not_ready);
	EXPECT_EQ(refused(DOS_UnlinkFile("Z:\\SAVE.DAT")), not_ready);
	EXPECT_EQ(refused(DOS_MakeDir("Z:\\NEWDIR")), not_ready);
	EXPECT_EQ(refused(DOS_RemoveDir("Z:\\DATA")), not_ready);
	EXPECT_EQ(refused(DOS_Rename("Z:\\SAVE.DAT", "Z:\\SAVE.OLD")), not_ready);
	EXPECT_EQ(refused(DOS_ChangeDir("Z:\\DATA")), not_ready);
	EXPECT_EQ(refused(DOS_GetFileAttr("Z:\\MINE.TXT", &attr)), not_ready);
	EXPECT_EQ(refused(DOS_SetFileAttr("Z:\\MINE.TXT", {})), not_ready);
}

TEST_F(UnionDriveTest, DeviceNamesStillOpenWhileTheDefaultDriveIsNotReady)
{
	ASSERT_TRUE(WriteDos("Z:\\MINE.TXT", "mine", true));
	std_fs::last_write_time(archive,
	                        std_fs::last_write_time(archive) +
	                                std::chrono::hours(1));
	uint16_t handle = 0;
	DOS_OpenFile("Z:\\README.TXT", OPEN_READ, &handle, fcb);
	ASSERT_EQ(ArchiveDriveOf(union_drive)->State(), ArchiveMountState::Changed);

	const auto previous_default = DOS_GetDefaultDrive();
	DOS_SetDefaultDrive(25);
	EXPECT_NE(DOS_FindDevice("NUL"), DOS_DEVICES);
	EXPECT_TRUE(DOS_OpenFile("NUL", OPEN_READWRITE, &handle, fcb));
	DOS_CloseFile(handle, fcb);
	EXPECT_TRUE(DOS_OpenFile("Z:\\NUL.TXT", OPEN_READWRITE, &handle, fcb));
	DOS_CloseFile(handle, fcb);
	EXPECT_FALSE(DOS_OpenFile("MINE.TXT", OPEN_READ, &handle, fcb));
	EXPECT_EQ(dos.errorcode, DOSERR_DRIVE_NOT_READY);
	DOS_SetDefaultDrive(previous_default);
}

TEST_F(UnionDriveTest, ChildSearchesThatRunToTheEndLeaveTheParentSearchAlone)
{
	const auto expected = Dir("Z:\\*.*");
	ASSERT_EQ(expected.size(), 5u);
	DriveSearch parent(*union_drive, 25, dos.dta());
	DriveSearch child(*union_drive, 25, dos.tables.tempdta);
	ASSERT_TRUE(parent.First(""));
	std::vector<std::string> names = {parent.Name()};
	for (int i = 0; i < MAX_OPENDIRS + 10; ++i) {
		ASSERT_TRUE(child.First("DATA")) << i;
		while (child.Next()) {
		}
	}
	while (parent.Next()) {
		names.push_back(parent.Name());
	}
	EXPECT_EQ(names, expected);
}

TEST_F(UnionDriveTest, AVolumeSearchEndsWithoutTouchingAnotherSearch)
{
	const auto expected = Dir("Z:\\*.*");
	DriveSearch parent(*union_drive, 25, dos.dta());
	DriveSearch volume(*union_drive, 25, dos.tables.tempdta);
	ASSERT_TRUE(parent.First(""));
	std::vector<std::string> names = {parent.Name()};
	ASSERT_TRUE(volume.First("", FatAttributeFlags::Volume));
	EXPECT_EQ(volume.Name(), "DOOM");
	dos.errorcode = 0;
	EXPECT_FALSE(volume.Next());
	EXPECT_EQ(dos.errorcode, DOSERR_NO_MORE_FILES);
	while (names.size() <= expected.size() && parent.Next()) {
		names.push_back(parent.Name());
	}
	EXPECT_EQ(names, expected);
}

TEST_F(UnionDriveTest, AbandonedSearchesAreEvictedBeforeTheSearchStillReading)
{
	const auto expected = Dir("Z:\\*.*");
	ASSERT_EQ(expected.size(), 5u);
	DriveSearch parent(*union_drive, 25, dos.dta());
	DriveSearch abandoned(*union_drive, 25, dos.tables.tempdta);
	ASSERT_TRUE(parent.First(""));
	std::vector<std::string> names = {parent.Name()};
	while (names.size() <= expected.size()) {
		for (int i = 0; i < MAX_OPENDIRS / 2; ++i) {
			ASSERT_TRUE(abandoned.First("DATA")) << i;
		}
		if (!parent.Next()) {
			break;
		}
		names.push_back(parent.Name());
	}
	EXPECT_EQ(names, expected);
}

TEST_F(UnionDriveTest, AReadOnlyHandleSeesTheCopyUpAnotherHandleMade)
{
	// As on a real disk, two handles on one file see each other's writes.
	uint16_t reader = 0;
	uint16_t writer = 0;
	ASSERT_TRUE(DOS_OpenFile("Z:\\SAVE.DAT", OPEN_READ, &reader, fcb));
	uint8_t buf[16] = {};
	uint16_t n      = 4;
	ASSERT_TRUE(DOS_ReadFile(reader, buf, &n, fcb));
	EXPECT_EQ(std::string(reinterpret_cast<char*>(buf), n), "old ");

	ASSERT_TRUE(DOS_OpenFile("Z:\\SAVE.DAT", OPEN_READWRITE, &writer, fcb));
	n = 3;
	ASSERT_TRUE(DOS_WriteFile(
	        writer, reinterpret_cast<uint8_t*>(const_cast<char*>("NEW")), &n, fcb));
	DOS_CloseFile(writer, fcb);

	// The reader carries on at its position, then rereads from the start.
	n = 4;
	ASSERT_TRUE(DOS_ReadFile(reader, buf, &n, fcb));
	EXPECT_EQ(std::string(reinterpret_cast<char*>(buf), n), "save");
	uint32_t pos = 0;
	ASSERT_TRUE(DOS_SeekFile(reader, &pos, DOS_SEEK_SET, fcb));
	n = 8;
	ASSERT_TRUE(DOS_ReadFile(reader, buf, &n, fcb));
	EXPECT_EQ(std::string(reinterpret_cast<char*>(buf), n), "NEW save");
	DOS_CloseFile(reader, fcb);
}

TEST_F(UnionDriveTest, RemoveDirLeavesTheFcbSearchBufferAlone)
{
	// FCB find (AH=11h/12h) keeps its search in the temp DTA.
	ASSERT_TRUE(DOS_MakeDir("Z:\\EMPTY1"));
	ASSERT_TRUE(DOS_MakeDir("Z:\\FULL"));
	ASSERT_TRUE(WriteDos("Z:\\FULL\\X.TXT", "x", true));
	const auto buffer         = RealToPhysical(dos.tables.tempdta);
	constexpr int DtaBytes    = 43;
	std::vector<uint8_t> mark = {};
	for (int i = 0; i < DtaBytes; ++i) {
		mark.push_back(static_cast<uint8_t>(0xa5 ^ i));
		mem_writeb(buffer + i, mark.back());
	}
	EXPECT_FALSE(DOS_RemoveDir("Z:\\FULL"));
	EXPECT_FALSE(DOS_RemoveDir("Z:\\DATA"));
	EXPECT_TRUE(DOS_RemoveDir("Z:\\EMPTY1"));
	for (int i = 0; i < DtaBytes; ++i) {
		EXPECT_EQ(mem_readb(buffer + i), mark[i]) << "byte " << i;
	}
}

TEST_F(UnionDriveTest, AFullRecordRefusesADeleteBeforeAnythingChanges)
{
	// A copy in the sidecar that shadows a member, made before the fill.
	ASSERT_TRUE(WriteDos("Z:\\README.TXT", "mine", false));
	ASSERT_TRUE(union_drive->Flush(&record_error)) << record_error;
	// Root names as long as SAVE.DAT, so the fill stops exactly where that
	// delete stops fitting, whatever the header's paths are on this host.
	auto full = union_drive->Record();
	for (int i = 0; full.HasRoomForDelete("SAVE.DAT") && i < 200000; ++i) {
		const auto n = std::to_string(i);
		ASSERT_TRUE(full.AddDelete("F" + std::string(7 - n.size(), '0') + n));
	}
	ASSERT_FALSE(full.HasRoomForDelete("SAVE.DAT"));
	ASSERT_TRUE(full.Save(sidecar / SidecarTomlName, &record_error))
	        << record_error;
	ASSERT_TRUE(Mount());
	const auto changes = union_drive->Record().changes.size();

	EXPECT_FALSE(DOS_UnlinkFile("Z:\\SAVE.DAT"));
	EXPECT_EQ(dos.errorcode, DOSERR_ACCESS_DENIED);
	EXPECT_EQ(ReadDos("Z:\\SAVE.DAT"), "old save");
	EXPECT_FALSE(DOS_UnlinkFile("Z:\\README.TXT"));
	EXPECT_EQ(dos.errorcode, DOSERR_ACCESS_DENIED);
	EXPECT_EQ(Host("README.TXT"), "mineo");
	EXPECT_FALSE(DOS_Rename("Z:\\SAVE.DAT", "Z:\\SAVE.OLD"));
	EXPECT_FALSE(std_fs::exists(sidecar / "SAVE.OLD"));
	EXPECT_EQ(union_drive->Record().changes.size(), changes);
}

TEST_F(UnionDriveTest, AFailedCopyUpTakesBackTheParentsItMade)
{
	// LEVEL.WAD fails its CRC once one stored byte flips; the archive's
	// size and mtime stay, so the drive stays Ready and only the member
	// fails.
	const auto stamp = std_fs::last_write_time(archive);
	{
		std::fstream zip(archive,
		                 std::ios::in | std::ios::out | std::ios::binary);
		std::string bytes((std::istreambuf_iterator<char>(zip)), {});
		const auto at = bytes.find(std::string(64, 'w'));
		ASSERT_NE(at, std::string::npos);
		zip.seekp(static_cast<std::streamoff>(at + 32));
		zip.put('x');
	}
	std_fs::last_write_time(archive, stamp);
	EXPECT_FALSE(union_drive->Rename("DATA\\LEVEL.WAD", "NEW1\\NEW2\\LEVEL.WAD"));
	EXPECT_FALSE(std_fs::exists(sidecar / "NEW1"));
	EXPECT_EQ(ArchiveDriveOf(union_drive)->State(), ArchiveMountState::Ready);
}

TEST_F(UnionDriveTest, ASecondHandlesFirstWriteKeepsTheFirstHandlesBytes)
{
	// Two read-write handles on a lower file: the second's first write
	// must not copy the lower bytes up again over the first's writes.
	uint16_t a = 0;
	uint16_t b = 0;
	ASSERT_TRUE(DOS_OpenFile("Z:\\SAVE.DAT", OPEN_READWRITE, &a, fcb));
	ASSERT_TRUE(DOS_OpenFile("Z:\\SAVE.DAT", OPEN_READWRITE, &b, fcb));
	uint16_t n = 3;
	ASSERT_TRUE(DOS_WriteFile(
	        a, reinterpret_cast<uint8_t*>(const_cast<char*>("NEW")), &n, fcb));
	uint32_t pos = 7;
	ASSERT_TRUE(DOS_SeekFile(b, &pos, DOS_SEEK_SET, fcb));
	n = 1;
	ASSERT_TRUE(DOS_WriteFile(
	        b, reinterpret_cast<uint8_t*>(const_cast<char*>("X")), &n, fcb));
	DOS_CloseFile(a, fcb);
	DOS_CloseFile(b, fcb);
	EXPECT_EQ(ReadDos("Z:\\SAVE.DAT"), "NEW savX");
}

#if !defined(WIN32)
TEST_F(UnionDriveTest, ALowercaseLinkInTheSidecarIsRefusedLikeAnUppercaseOne)
{
	// localDrive matches names without case, so the link check must see
	// the host spelling and not the DOS one.
	const auto outside = tmp / "outside";
	std_fs::create_directories(outside);
	std_fs::create_directory_symlink(outside, sidecar / "saves");
	const auto victim = tmp / "victim.txt";
	{
		std::ofstream out(victim);
		out << "keep";
	}
	std_fs::create_symlink(victim, sidecar / "save.dat");
	ASSERT_TRUE(Mount());
	EXPECT_FALSE(WriteDos("Z:\\SAVES\\NEW.TXT", "n", true));
	EXPECT_TRUE(std_fs::is_empty(outside));
	EXPECT_FALSE(WriteDos("Z:\\SAVE.DAT", "X", false));
	EXPECT_EQ(Host("../victim.txt"), "keep");
}
#endif

// E4: "NAME." and "NAME" are the same file in DOS. The sidecar record
// must key by the canonical form so DEL one hides both and a create
// through either unhides.
TEST_F(UnionDriveTest, ATrailingDotDeleteHidesTheCanonicalName)
{
	EXPECT_TRUE(DOS_FileExists("Z:\\SKIEGA"));
	EXPECT_TRUE(DOS_UnlinkFile("Z:\\SKIEGA."));
	EXPECT_FALSE(DOS_FileExists("Z:\\SKIEGA"));
	EXPECT_FALSE(DOS_FileExists("Z:\\SKIEGA."));
	auto names = Dir("Z:\\*.*");
	EXPECT_EQ(std::count(names.begin(), names.end(), "SKIEGA"), 0);
}

TEST_F(UnionDriveTest, ACanonicalDeleteHidesTheTrailingDotForm)
{
	EXPECT_TRUE(DOS_UnlinkFile("Z:\\SKIEGA"));
	EXPECT_FALSE(DOS_FileExists("Z:\\SKIEGA."));
	EXPECT_FALSE(DOS_FileExists("Z:\\SKIEGA"));
}

TEST_F(UnionDriveTest, ACreateAfterATrailingDotDeleteUnhides)
{
	EXPECT_TRUE(DOS_UnlinkFile("Z:\\SKIEGA."));
	EXPECT_FALSE(DOS_FileExists("Z:\\SKIEGA"));
	ASSERT_TRUE(WriteDos("Z:\\SKIEGA", "back", true));
	EXPECT_EQ(ReadDos("Z:\\SKIEGA"), "back");
	EXPECT_EQ(ReadDos("Z:\\SKIEGA."), "back");
}

} // namespace
