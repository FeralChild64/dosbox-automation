// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//
#include "dos/programs/append.h"
#include "dos/programs/mount_run.h"

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "dos/dos.h"
#include "dos/drives.h"
#include "dosbox_test_fixture.h"
#include "test_temp_dir.h"

namespace {

using namespace testing;
using MountRun::Refusal;
using MountRun::Resolve;

class MountRunTest : public DOSBoxTestFixture {
protected:
	void SetUp() override
	{
		DOSBoxTestFixture::SetUp();
		tmp = TestTempDir::MakeUnique("mount_run_");
		ASSERT_FALSE(tmp.empty());
		previous_drive    = Drives.at(DriveC);
		Drives.at(DriveC) = std::make_shared<localDrive>(
		        (tmp.string() + CROSS_FILESPLIT).c_str(),
		        512,
		        32,
		        32765,
		        16000,
		        0xf8,
		        false,
		        true);
	}
	void TearDown() override
	{
		Drives.at(DriveC)  = previous_drive;
		std::error_code ec = {};
		std::filesystem::remove_all(tmp, ec);
		DOSBoxTestFixture::TearDown();
	}
	// The drive's directory cache does not see host changes made after
	// it was read, so each change refreshes it as a fresh MOUNT would.
	void File(const std::string& relative)
	{
		const auto path = tmp / relative;
		std::filesystem::create_directories(path.parent_path());
		std::ofstream(path, std::ios::binary) << "x";
		Drives.at(DriveC)->EmptyCache();
	}
	void Folder(const std::string& relative)
	{
		std::filesystem::create_directories(tmp / relative);
		Drives.at(DriveC)->EmptyCache();
	}
	void Remove(const std::string& relative)
	{
		std::filesystem::remove(tmp / relative);
		Drives.at(DriveC)->EmptyCache();
	}

	static constexpr uint8_t DriveC           = 2;
	std::filesystem::path tmp                 = {};
	std::shared_ptr<DOS_Drive> previous_drive = {};
};

// APPEND /x answers DOS finds from its list; the engine's own probe of
// the mounted drive must not be answered from somewhere else.
TEST_F(MountRunTest, ARunTargetIsNeverFoundThroughTheAppendList)
{
	// A missing folder is "path not found", which stops APPEND on real
	// DOS too; the folder exists and the file does not.
	Folder("GAME");
	File("UTIL/SETUP.EXE");
	auto& state        = AppendCommand::ResidentState();
	const auto before  = state;
	state.installed    = true;
	state.enabled      = true;
	state.exec_allowed = true;
	state.search_exec  = true;
	state.list         = "C:\\UTIL";
	EXPECT_TRUE(DOS_FindFirst("C:\\GAME\\SETUP.EXE",
	                          FatAttributeFlags::NotVolume));
	const auto target = Resolve('C', "GAME\\SETUP.EXE");
	EXPECT_NE(target.refusal, Refusal::None);
	EXPECT_TRUE(target.lines.empty());
	state = before;
}

TEST_F(MountRunTest, ANamedProgramAtTheRootRuns)
{
	File("GAME.EXE");
	const auto target = Resolve('C', "game.exe");
	EXPECT_EQ(target.refusal, Refusal::None);
	EXPECT_THAT(target.lines, ElementsAre("C:", "C:\\GAME.EXE"));
	EXPECT_EQ(Resolve('c', "\\GAME.EXE").lines, target.lines);
}

TEST_F(MountRunTest, ANamedProgramInAFolderChangesThereFirst)
{
	File("GAME/BIN/START.EXE");
	for (const auto* name :
	     {"GAME\\BIN\\START.EXE", "game/bin/start.exe", "\\GAME\\BIN\\START.EXE"}) {
		const auto target = Resolve('C', name);
		EXPECT_EQ(target.refusal, Refusal::None) << name;
		EXPECT_THAT(target.lines,
		            ElementsAre("C:", "CD \\GAME\\BIN", "C:\\GAME\\BIN\\START.EXE"))
		        << name;
	}
}

TEST_F(MountRunTest, ANameWithoutExtensionTakesComBeforeExe)
{
	File("GAME.EXE");
	File("GAME.BAT");
	File("GAME.COM");
	EXPECT_THAT(Resolve('C', "GAME").lines, ElementsAre("C:", "C:\\GAME.COM"));
	Remove("GAME.COM");
	EXPECT_THAT(Resolve('C', "GAME").lines, ElementsAre("C:", "C:\\GAME.EXE"));
	Remove("GAME.EXE");
	EXPECT_THAT(Resolve('C', "GAME").lines,
	            ElementsAre("C:", "CALL C:\\GAME.BAT"));
}

TEST_F(MountRunTest, ABatchIsCalled)
{
	File("SETUP/START.BAT");
	EXPECT_THAT(Resolve('C', "SETUP\\START.BAT").lines,
	            ElementsAre("C:", "CD \\SETUP", "CALL C:\\SETUP\\START.BAT"));
}

TEST_F(MountRunTest, TheOnlyProgramAtTheRootIsFound)
{
	File("README.TXT");
	File("GAME.EXE");
	File("SUB/OTHER.EXE");
	Folder("TOOLS.EXE");
	const auto target = Resolve('C', "");
	EXPECT_EQ(target.refusal, Refusal::None);
	EXPECT_THAT(target.lines, ElementsAre("C:", "C:\\GAME.EXE"));
}

TEST_F(MountRunTest, SeveralProgramsAreListedNotRun)
{
	File("B.EXE");
	File("A.COM");
	File("C.BAT");
	File("README.TXT");
	const auto target = Resolve('C', "");
	EXPECT_EQ(target.refusal, Refusal::SeveralPrograms);
	EXPECT_TRUE(target.lines.empty());
	EXPECT_THAT(target.candidates, ElementsAre("A.COM", "B.EXE", "C.BAT"));
}

TEST_F(MountRunTest, NoProgramIsListedAsNone)
{
	File("README.TXT");
	File("SUB/GAME.EXE");
	const auto target = Resolve('C', "");
	EXPECT_EQ(target.refusal, Refusal::NoProgram);
	EXPECT_TRUE(target.lines.empty());
	EXPECT_TRUE(target.candidates.empty());
}

TEST_F(MountRunTest, TraversalAndDriveLettersAreRefused)
{
	File("X.EXE");
	File("GAME/X.EXE");
	for (const auto* name : {"..\\X.EXE",
	                         "C:X.EXE",
	                         "C:\\X.EXE",
	                         "\\..\\X.EXE",
	                         "GAME\\..\\X.EXE",
	                         ".\\X.EXE",
	                         "GAME\\.\\X.EXE",
	                         "\\\\X.EXE",
	                         "GAME\\\\X.EXE",
	                         "GAME\\",
	                         ".",
	                         ".."}) {
		const auto target = Resolve('C', name);
		EXPECT_EQ(target.refusal, Refusal::BadName) << name;
		EXPECT_TRUE(target.lines.empty()) << name;
	}
}

TEST_F(MountRunTest, ShellMetacharactersAreRefused)
{
	File("A.EXE");
	for (const auto& name : {std::string("A&B.EXE"),
	                         std::string("A|B"),
	                         std::string("A>B"),
	                         std::string("A<B"),
	                         std::string("\"A\""),
	                         std::string("A B.EXE"),
	                         std::string("A.EXE;DEL"),
	                         std::string("A,B"),
	                         std::string("A\tB"),
	                         std::string("A.EXE\r\nDEL"),
	                         std::string("A\0.EXE", 6),
	                         std::string("A*.EXE"),
	                         std::string("A?.EXE"),
	                         std::string("A=B"),
	                         std::string("A+B"),
	                         std::string("[A]"),
	                         std::string("\xe9.EXE")}) {
		const auto target = Resolve('C', name);
		EXPECT_EQ(target.refusal, Refusal::BadName) << name;
		EXPECT_TRUE(target.lines.empty()) << name;
	}
}

TEST_F(MountRunTest, NamesOutsideEightDotThreeAreRefused)
{
	File("GAME.EXE");
	for (const auto& name : {std::string("TOOLONGNM.EXE"),
	                         std::string("GAME.EXEC"),
	                         std::string("GAME."),
	                         std::string(".EXE"),
	                         std::string("A.B.EXE"),
	                         std::string("LONGFOLDER\\GAME.EXE"),
	                         std::string(""),
	                         std::string(65, 'A')}) {
		if (name.empty()) {
			continue; // no name is the root search
		}
		EXPECT_EQ(Resolve('C', name).refusal, Refusal::BadName) << name;
	}
	auto longest = std::string();
	for (int i = 0; i < 7; ++i) {
		longest += "ABCDEFGH\\";
	}
	longest += "ABCDE.EXE";
	ASSERT_EQ(longest.size(), 72u);
	EXPECT_EQ(Resolve('C', longest).refusal, Refusal::BadName);
	EXPECT_EQ(Resolve('C', longest.substr(9)).refusal, Refusal::NotFound);
}

// The shell takes the stem of TYPE.EXE for its TYPE command; a path on the
// drive never matches a built-in.
TEST_F(MountRunTest, AProgramNamedLikeAShellCommandIsNamedByItsPath)
{
	File("TYPE.EXE");
	File("TOOLS/SET.BAT");
	EXPECT_THAT(Resolve('C', "TYPE").lines, ElementsAre("C:", "C:\\TYPE.EXE"));
	EXPECT_THAT(Resolve('C', "TOOLS\\SET").lines,
	            ElementsAre("C:", "CD \\TOOLS", "CALL C:\\TOOLS\\SET.BAT"));
}

TEST_F(MountRunTest, ALeadingDashIsRefusedSinceMountWouldTakeItForAnOption)
{
	File("-GAME.EXE");
	EXPECT_EQ(Resolve('C', "-GAME.EXE").refusal, Refusal::BadName);
	EXPECT_EQ(Resolve('C', "SUB\\-GAME.EXE").refusal, Refusal::BadName);
}

TEST_F(MountRunTest, TheDosFilenameSetIsAccepted)
{
	File("_~!#$%'(.EXE");
	File("){}@^-09.COM");
	EXPECT_THAT(Resolve('C', "_~!#$%'(.EXE").lines,
	            ElementsAre("C:", "C:\\_~!#$%'(.EXE"));
	EXPECT_THAT(Resolve('C', "){}@^-09").lines,
	            ElementsAre("C:", "C:\\){}@^-09.COM"));
}

TEST_F(MountRunTest, AMissingNameIsNotFound)
{
	File("SETUP.EXE");
	File("GAME.EXE");
	const auto target = Resolve('C', "PLAY.EXE");
	EXPECT_EQ(target.refusal, Refusal::NotFound);
	EXPECT_TRUE(target.lines.empty());
	EXPECT_THAT(target.candidates, ElementsAre("GAME.EXE", "SETUP.EXE"));
	EXPECT_EQ(Resolve('C', "PLAY").refusal, Refusal::NotFound);
	EXPECT_EQ(Resolve('C', "NOWHERE\\GAME.EXE").refusal, Refusal::NotFound);
}

TEST_F(MountRunTest, AFolderIsNotAProgram)
{
	File("GAME.EXE");
	Folder("PLAY");
	Folder("TOOLS.EXE");
	File("README.TXT");
	for (const auto* name : {"PLAY", "TOOLS.EXE", "README.TXT"}) {
		const auto target = Resolve('C', name);
		EXPECT_EQ(target.refusal, Refusal::NotAProgram) << name;
		EXPECT_TRUE(target.lines.empty()) << name;
		EXPECT_THAT(target.candidates, ElementsAre("GAME.EXE")) << name;
	}
}

TEST_F(MountRunTest, ADriveThatIsNotMountedIsRefused)
{
	File("GAME.EXE");
	for (const char drive : {'Q', '1', '\0', '@', '['}) {
		EXPECT_EQ(Resolve(drive, "GAME.EXE").refusal, Refusal::NoDrive)
		        << static_cast<int>(drive);
		EXPECT_EQ(Resolve(drive, "").refusal, Refusal::NoDrive)
		        << static_cast<int>(drive);
	}
}

TEST_F(MountRunTest, DeviceNamesAreNotPrograms)
{
	File("GAME.EXE");
	for (const auto* name : {"CON", "NUL.EXE", "AUX.COM", "PRN", "CLOCK$"}) {
		const auto target = Resolve('C', name);
		EXPECT_NE(target.refusal, Refusal::None) << name;
		EXPECT_TRUE(target.lines.empty()) << name;
	}
}

TEST(MountRunName, ProgramNamesFollowTheResolveRule)
{
	for (const auto* name :
	     {"GAME.EXE", "game", "\\GAME\\START.EXE", "A/B/C.BAT", "_~!#$%'(.COM"}) {
		EXPECT_TRUE(MountRun::IsProgramName(name)) << name;
	}
	for (const auto* name : {"",
	                         "..\\X.EXE",
	                         "C:X.EXE",
	                         "A B",
	                         "X >NUL",
	                         "A|B",
	                         "TOOLONGNM.EXE",
	                         "GAME.",
	                         "-GAME.EXE",
	                         "GAME\\-X.EXE"}) {
		EXPECT_FALSE(MountRun::IsProgramName(name)) << name;
	}
}

TEST(MountRunDescribe, ARunTargetSaysNothing)
{
	MountRun::Target target = {};
	target.lines            = {"D:", "GAME.EXE"};
	EXPECT_EQ(MountRun::Describe(target, "GAME.EXE", 'D', MountRun::EnglishTexts()),
	          "");
}

TEST(MountRunDescribe, AMissingNameIsNamedWithTheCandidates)
{
	MountRun::Target target = {};
	target.refusal          = Refusal::NotFound;
	target.candidates       = {"GAME.EXE", "SETUP.EXE"};
	EXPECT_EQ(MountRun::Describe(target, "play.exe", 'D', MountRun::EnglishTexts()),
	          "play.exe was not found on drive D:.\n"
	          "Programs at the root: GAME.EXE SETUP.EXE\n");
}

TEST(MountRunDescribe, EachRefusalHasItsReason)
{
	const auto& texts = MountRun::EnglishTexts();
	auto describe     = [&](const Refusal refusal,
                            std::vector<std::string>
                                    candidates,
                            const std::string& name) {
                MountRun::Target target = {};
                target.refusal          = refusal;
                target.candidates       = std::move(candidates);
                return MountRun::Describe(target, name, 'C', texts);
	};
	EXPECT_EQ(describe(Refusal::NotAProgram, {"GAME.EXE"}, "README.TXT"),
	          "README.TXT is not a .COM, .EXE or .BAT file.\n"
	          "Programs at the root: GAME.EXE\n");
	EXPECT_EQ(describe(Refusal::NotAProgram, {}, "README.TXT"),
	          "README.TXT is not a .COM, .EXE or .BAT file.\n");
	EXPECT_EQ(describe(Refusal::BadName, {}, "..\\X.EXE"),
	          "..\\X.EXE is not a program name on the drive: use an 8.3 "
	          "path without a drive letter.\n");
	EXPECT_EQ(describe(Refusal::NoProgram, {}, ""),
	          "Drive C: has no .COM, .EXE or .BAT file at its root.\n");
	EXPECT_EQ(describe(Refusal::SeveralPrograms, {"A.COM", "B.EXE"}, ""),
	          "Drive C: has several programs at its root; name one with "
	          "-run NAME.\n"
	          "Programs at the root: A.COM B.EXE\n");
	EXPECT_EQ(describe(Refusal::NoDrive, {}, "GAME.EXE"),
	          "Drive C: is not mounted.\n");
}

TEST(MountRunDescribe, TheEchoedNameHasNoControlCharactersAndAnEnd)
{
	MountRun::Target target = {};
	target.refusal          = Refusal::BadName;
	const auto& texts       = MountRun::EnglishTexts();
	const auto shown = MountRun::Describe(target, "A\x1b[2J\r\nB\x7f\xe9", 'C', texts);
	EXPECT_EQ(shown.substr(0, 11), "A?[2J??B?? ");
	const auto long_shown =
	        MountRun::Describe(target, std::string(200, 'A'), 'C', texts);
	EXPECT_EQ(long_shown.substr(0, 68), std::string(64, 'A') + "... ");
}

TEST_F(MountRunTest, APercentInTheNameSurvivesTheQueue)
{
	File("100%.EXE");
	const auto target = Resolve('C', "100%.EXE");
	EXPECT_EQ(target.refusal, Refusal::None);
	EXPECT_THAT(target.lines, ElementsAre("C:", "C:\\100%.EXE"));
}

} // namespace
